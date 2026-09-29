#include "unpack.h"

#include "error.h"
#include "lua_sandbox.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define UNPACK_MAX_NATIVE 4096

#ifdef _WIN32
#define NATIVE_SEPARATOR '\\'
#else
#define NATIVE_SEPARATOR '/'
#endif

const fr_unpack_limits FR_UNPACK_DEFAULTS = {
    .max_total_bytes = 1024u * 1024u * 1024u,
    .max_members = 262144u,
    .max_member_bytes = 256u * 1024u * 1024u,
    .max_path_length = 1024u,
};

static int component_is_unsafe(const char *begin, const char *end) {
    size_t length = (size_t) (end - begin);
    if (length == 0) return 1;
    if (length == 1 && begin[0] == '.') return 1;
    if (length == 2 && begin[0] == '.' && begin[1] == '.') return 1;
    return 0;
}

int fr_unpack_name_is_safe(const char *name) {
    if (name == NULL || name[0] == '\0') return 0;
    if (fr_lua_sandbox_climbs_out(name)) return 0;
    if (strchr(name, '\\') != NULL) return 0;

    size_t length = strlen(name);
    if (name[length - 1] == '/') length--;
    if (length == 0) return 0;

    const char *component = name;
    for (size_t index = 0; index <= length; index++) {
        if (index != length && name[index] != '/') continue;
        if (component_is_unsafe(component, name + index)) return 0;
        component = name + index + 1;
    }
    return 1;
}

/* The same two forms fr_lua_sandbox_climbs_out refuses first, repeated rather
   than borrowed: that helper also refuses every "..", and a link target may
   legitimately carry one. */
static int target_is_absolute(const char *target) {
    if (target[0] == '/' || target[0] == '\\') return 1;
    return target[1] == ':';
}

static size_t directory_depth_of(const char *name) {
    size_t length = strlen(name);
    if (length > 0 && name[length - 1] == '/') length--;

    size_t depth = 0;
    for (size_t index = 0; index < length; index++) {
        if (name[index] == '/') depth++;
    }
    return depth;
}

/* A target is relative to the directory the LINK sits in, not to the
   destination root, so "bin/x" -> "../lib/real" stays inside while
   "bin/x" -> "../../outside" does not. Resolving against the root instead gets
   both of those backwards. */
static int target_stays_inside(const char *name, const char *target) {
    size_t depth = directory_depth_of(name);
    const char *component = target;
    for (const char *cursor = target;; cursor++) {
        if (*cursor != '\0' && *cursor != '/') continue;

        size_t length = (size_t) (cursor - component);
        if (length == 2 && component[0] == '.' && component[1] == '.') {
            if (depth == 0) return 0;
            depth--;
        } else if (length > 0 && !(length == 1 && component[0] == '.')) {
            depth++;
        }

        if (*cursor == '\0') return 1;
        component = cursor + 1;
    }
}

/* Decided from the target's own bytes and before anything is created, so no
   link ever exists whose target has not been judged. */
static int refuse_unsafe_link_target(const fr_archive_member *member, fr_error *err) {
    const char *target = member->link_target;
    if (target == NULL || target[0] == '\0') {
        fr_error_set(err, "the symlink member \"%s\" has an empty target", member->name);
        return FR_ERR;
    }
    if (target_is_absolute(target)) {
        fr_error_set(err, "the symlink member \"%s\" points at an absolute target", member->name);
        return FR_ERR;
    }
    /* The walker below knows one separator, so "..\\..\\x" would be a single
       component; refused outright, as a member name already is. */
    if (strchr(target, '\\') != NULL) {
        fr_error_set(err, "the symlink member \"%s\" points at a target holding a backslash",
                     member->name);
        return FR_ERR;
    }
    if (!target_stays_inside(member->name, target)) {
        fr_error_set(err, "the symlink member \"%s\" points outside the destination",
                     member->name);
        return FR_ERR;
    }
    return FR_OK;
}

typedef struct {
    char text[UNPACK_MAX_NATIVE];
    size_t length;
} native_root;

/* @implNote The "\\?\" prefix a deep member needs past MAX_PATH also turns off
   the normalisation that would otherwise accept a forward slash or a relative
   segment, so the destination is made fully qualified and backslashed here,
   once, and every join below inherits both. Member names stay forward-slashed
   everywhere else in this file. */
static int native_root_of(const char *destination, native_root *out, fr_error *err) {
#ifdef _WIN32
    char absolute[UNPACK_MAX_NATIVE];
    if (_fullpath(absolute, destination, sizeof absolute) == NULL) {
        fr_error_set(err, "the destination \"%s\" cannot be made absolute", destination);
        return FR_ERR;
    }
    int written = absolute[0] == '\\' && absolute[1] == '\\'
                ? snprintf(out->text, sizeof out->text, "\\\\?\\UNC\\%s", absolute + 2)
                : snprintf(out->text, sizeof out->text, "\\\\?\\%s", absolute);
#else
    int written = snprintf(out->text, sizeof out->text, "%s", destination);
#endif
    if (written < 0 || (size_t) written + 2 >= sizeof out->text) {
        fr_error_set(err, "the destination is longer than %zu bytes", sizeof out->text);
        return FR_ERR;
    }

    out->length = (size_t) written;
    if (out->length > 1 && out->text[out->length - 1] == NATIVE_SEPARATOR
        && out->text[out->length - 2] != ':') {
        out->text[--out->length] = '\0';
    }
    return FR_OK;
}

static int join_under_root(const native_root *root, const char *relative, char *out,
                           size_t out_size, fr_error *err) {
    size_t relative_length = strlen(relative);
    if (relative_length + 2 > out_size - root->length) {
        fr_error_set(err, "the destination for \"%s\" is longer than %zu bytes", relative,
                     out_size);
        return FR_ERR;
    }

    memcpy(out, root->text, root->length);
    out[root->length] = NATIVE_SEPARATOR;
    memcpy(out + root->length + 1, relative, relative_length + 1);
#ifdef _WIN32
    for (char *cursor = out + root->length + 1; *cursor != '\0'; cursor++) {
        if (*cursor == '/') *cursor = NATIVE_SEPARATOR;
    }
#endif
    return FR_OK;
}

/* 1 created, 0 already there, -1 refused. */
static int make_one_directory(const char *native) {
#ifdef _WIN32
    if (_mkdir(native) == 0) return 1;
#else
    if (mkdir(native, 0777) == 0) return 1;
#endif
    return errno == EEXIST ? 0 : -1;
}

static int make_directory_tree(char *native, size_t root_length, const char *relative,
                               fr_unpack_report *report, fr_error *err) {
    for (char *cursor = native + root_length + 1;; cursor++) {
        if (*cursor != '\0' && *cursor != NATIVE_SEPARATOR) continue;

        char saved = *cursor;
        *cursor = '\0';
        int made = make_one_directory(native);
        *cursor = saved;

        if (made < 0) {
            fr_error_set(err, "cannot create a directory for the member \"%s\"", relative);
            return FR_ERR;
        }
        if (made > 0) report->directories_created++;
        if (saved == '\0') return FR_OK;
    }
}

static int make_parent_directories(char *native, size_t root_length, const char *relative,
                                   fr_unpack_report *report, fr_error *err) {
    char *last = strrchr(native + root_length + 1, NATIVE_SEPARATOR);
    if (last == NULL) return FR_OK;

    *last = '\0';
    int made = make_directory_tree(native, root_length, relative, report, err);
    *last = NATIVE_SEPARATOR;
    return made;
}

/* Windows carries no mode to apply: an extension decides what runs there. */
static int apply_permissions(const char *native, const fr_archive_member *member, fr_error *err) {
#ifdef _WIN32
    (void) native;
    (void) member;
    (void) err;
    return FR_OK;
#else
    if (chmod(native, member->executable ? 0755 : 0644) != 0) {
        fr_error_set(err, "cannot set the mode of the member \"%s\"", member->name);
        return FR_ERR;
    }
    return FR_OK;
#endif
}

static int write_file_bytes(const char *native, const char *bytes, size_t length,
                            const char *relative, fr_error *err) {
    FILE *file = fopen(native, "wb");
    if (file == NULL) {
        fr_error_set(err, "cannot create the member \"%s\"", relative);
        return FR_ERR;
    }

    size_t written = length == 0 ? 0 : fwrite(bytes, 1, length, file);
    int closed = fclose(file);
    if (written != length || closed != 0) {
        remove(native);
        fr_error_set(err, "cannot write the member \"%s\"", relative);
        return FR_ERR;
    }
    return FR_OK;
}

/* Insert-only open addressing, because a linear scan over a toolchain's tens of
   thousands of members is quadratic where the duplicate rule is not. */
typedef struct {
    char **slots;
    size_t mask;
    size_t count;
} name_set;

static unsigned long long name_hash(const char *name) {
    unsigned long long hash = 14695981039346656037ull;
    for (const unsigned char *cursor = (const unsigned char *) name; *cursor != '\0'; cursor++) {
        hash ^= *cursor;
        hash *= 1099511628211ull;
    }
    return hash;
}

static int name_set_grow(name_set *set) {
    size_t capacity = set->slots == NULL ? 64 : (set->mask + 1) * 2;
    char **slots = calloc(capacity, sizeof *slots);
    if (slots == NULL) return 0;

    size_t mask = capacity - 1;
    for (size_t index = 0; set->slots != NULL && index <= set->mask; index++) {
        char *existing = set->slots[index];
        if (existing == NULL) continue;
        size_t target = (size_t) name_hash(existing) & mask;
        while (slots[target] != NULL) target = (target + 1) & mask;
        slots[target] = existing;
    }

    free(set->slots);
    set->slots = slots;
    set->mask = mask;
    return 1;
}

/* 1 added, 0 already there, -1 out of memory. */
static int name_set_add(name_set *set, const char *name) {
    if (set->slots == NULL || (set->count + 1) * 4 > (set->mask + 1) * 3) {
        if (!name_set_grow(set)) return -1;
    }

    size_t index = (size_t) name_hash(name) & set->mask;
    while (set->slots[index] != NULL) {
        if (strcmp(set->slots[index], name) == 0) return 0;
        index = (index + 1) & set->mask;
    }

    size_t length = strlen(name);
    char *copy = malloc(length + 1);
    if (copy == NULL) return -1;
    memcpy(copy, name, length + 1);

    set->slots[index] = copy;
    set->count++;
    return 1;
}

static void name_set_free(name_set *set) {
    for (size_t index = 0; set->slots != NULL && index <= set->mask; index++) {
        free(set->slots[index]);
    }
    free(set->slots);
}

static int refuse_unwritable_name(const fr_archive_member *member,
                                  const fr_unpack_limits *limits, name_set *seen, fr_error *err) {
    if (!fr_unpack_name_is_safe(member->name)) {
        fr_error_set(err, "the member \"%s\" would be written outside the destination",
                     member->name);
        return FR_ERR;
    }
    if (strlen(member->name) > limits->max_path_length) {
        fr_error_set(err, "the member \"%s\" is longer than the %zu byte limit", member->name,
                     limits->max_path_length);
        return FR_ERR;
    }

    int added = name_set_add(seen, member->name);
    if (added < 0) {
        fr_error_set(err, "out of memory while unpacking \"%s\"", member->name);
        return FR_ERR;
    }
    if (added == 0) {
        fr_error_set(err, "the archive names \"%s\" twice", member->name);
        return FR_ERR;
    }
    return FR_OK;
}

static int refuse_oversized_member(const fr_archive_member *member,
                                   const fr_unpack_limits *limits, size_t *total, fr_error *err) {
    if (member->length > limits->max_member_bytes) {
        fr_error_set(err, "the member \"%s\" is larger than the %zu byte limit", member->name,
                     limits->max_member_bytes);
        return FR_ERR;
    }
    if (member->length > limits->max_total_bytes - *total) {
        fr_error_set(err, "the archive expands past the %zu byte limit", limits->max_total_bytes);
        return FR_ERR;
    }

    *total += member->length;
    return FR_OK;
}

static int write_directory_member(const fr_archive_member *member, const native_root *root,
                                  fr_unpack_report *report, fr_error *err) {
    char native[UNPACK_MAX_NATIVE];
    if (join_under_root(root, member->name, native, sizeof native, err) != FR_OK) return FR_ERR;

    size_t length = strlen(native);
    while (length > root->length + 1 && native[length - 1] == NATIVE_SEPARATOR) {
        native[--length] = '\0';
    }
    return make_directory_tree(native, root->length, member->name, report, err);
}

static int write_file_member(const fr_archive_member *member, const native_root *root,
                             fr_unpack_report *report, fr_error *err) {
    char native[UNPACK_MAX_NATIVE];
    if (join_under_root(root, member->name, native, sizeof native, err) != FR_OK) return FR_ERR;

    if (make_parent_directories(native, root->length, member->name, report, err) != FR_OK) {
        return FR_ERR;
    }
    if (write_file_bytes(native, member->bytes, member->length, member->name, err) != FR_OK) {
        return FR_ERR;
    }
    if (apply_permissions(native, member, err) != FR_OK) return FR_ERR;

    report->files_written++;
    return FR_OK;
}

/* @implNote Windows creates no link at all: SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
   needs developer mode, and a toolchain that silently half-unpacks is worse
   than one that says which links it left out. The target is validated on both
   platforms anyway, so the refusal does not depend on the creation. */
static int write_symlink_member(const fr_archive_member *member, const native_root *root,
                                fr_unpack_report *report, fr_error *err) {
    if (refuse_unsafe_link_target(member, err) != FR_OK) return FR_ERR;

#ifdef _WIN32
    (void) root;
    if (report->symlinks_skipped == 0) {
        snprintf(report->first_symlink_skipped, sizeof report->first_symlink_skipped, "%s",
                 member->name);
    }
    report->symlinks_skipped++;
    return FR_OK;
#else
    char native[UNPACK_MAX_NATIVE];
    if (join_under_root(root, member->name, native, sizeof native, err) != FR_OK) return FR_ERR;
    if (make_parent_directories(native, root->length, member->name, report, err) != FR_OK) {
        return FR_ERR;
    }
    if (symlink(member->link_target, native) != 0) {
        fr_error_set(err, "cannot create the symlink member \"%s\"", member->name);
        return FR_ERR;
    }

    report->symlinks_created++;
    return FR_OK;
#endif
}

static int write_member(const fr_archive_member *member, const native_root *root,
                        const fr_unpack_limits *limits, name_set *seen, size_t *total,
                        fr_unpack_report *report, fr_error *err) {
    if (refuse_unwritable_name(member, limits, seen, err) != FR_OK) return FR_ERR;

    if (member->kind == FR_MEMBER_SYMLINK) {
        return write_symlink_member(member, root, report, err);
    }
    if (member->kind == FR_MEMBER_DIRECTORY) {
        return write_directory_member(member, root, report, err);
    }

    /* Regular files only: a setgid directory is routine in a tarball made on
       macOS or BSD, and apply_permissions never carries an archive's mode
       across anyway. */
    if (member->setuid) {
        fr_error_set(err, "the member \"%s\" carries a setuid or setgid bit", member->name);
        return FR_ERR;
    }
    if (refuse_oversized_member(member, limits, total, err) != FR_OK) return FR_ERR;
    return write_file_member(member, root, report, err);
}

int fr_unpack(fr_archive *archive, const char *destination, const fr_unpack_limits *limits,
              fr_unpack_report *report, fr_error *err) {
    fr_unpack_report discarded;
    if (report == NULL) report = &discarded;
    memset(report, 0, sizeof *report);
    if (limits == NULL) limits = &FR_UNPACK_DEFAULTS;

    native_root root;
    if (native_root_of(destination, &root, err) != FR_OK) return FR_ERR;

    name_set seen;
    memset(&seen, 0, sizeof seen);

    size_t members = 0;
    size_t total = 0;
    int result = FR_OK;
    for (;;) {
        const fr_archive_member *member = NULL;
        if (fr_archive_next(archive, &member, err) != FR_OK) {
            result = FR_ERR;
            break;
        }
        if (member == NULL) break;

        if (members >= limits->max_members) {
            fr_error_set(err, "the archive holds more than %zu members", limits->max_members);
            result = FR_ERR;
            break;
        }
        members++;

        result = write_member(member, &root, limits, &seen, &total, report, err);
        if (result != FR_OK) break;
    }

    name_set_free(&seen);
    return result;
}
