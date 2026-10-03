#include "archive/unpack.h"

#include "lua/lua_sandbox.h"
#include "util/error.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
/* Present since Windows 10 build 14972 but absent from older SDK headers, and
   a build that quietly dropped it would create no link and copy everything. */
#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif
#else
#include <fcntl.h>
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

/* @implNote max_total_bytes was 1 GiB and refused the largest archive daukle
   actually provisions: xpack clang 21.1.8-1 for win32-x64 expands to 1.353 GiB
   over 9881 members, measured from its published central directory. The other
   three rows of that toolchain fit (linux 0.679, macos 0.317 and 0.307 GiB),
   so the bound was red on one platform only. 2 GiB is the next power of two
   above the measured largest, and a bound raised to fit a real archive should
   be raised against a measurement rather than to a round number. D-54. */
const fr_unpack_limits FR_UNPACK_DEFAULTS = {
    .max_total_bytes = 2u * 1024u * 1024u * 1024u,
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

/* @implNote O_NOFOLLOW is belt and braces over the ancestor check below: it is
   what stops a file landing THROUGH a link on the final component even if that
   check is somehow wrong. Windows creates no symlink, so plain fopen there. */
static FILE *open_new_file(const char *native) {
#ifdef _WIN32
    return fopen(native, "wb");
#else
    int descriptor = open(native, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0666);
    if (descriptor < 0) return NULL;

    FILE *file = fdopen(descriptor, "wb");
    if (file == NULL) close(descriptor);
    return file;
#endif
}

static int write_file_bytes(const char *native, const char *bytes, size_t length,
                            const char *relative, fr_error *err) {
    FILE *file = open_new_file(native);
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

static int name_set_contains(const name_set *set, const char *name) {
    if (set->slots == NULL) return 0;

    size_t index = (size_t) name_hash(name) & set->mask;
    while (set->slots[index] != NULL) {
        if (strcmp(set->slots[index], name) == 0) return 1;
        index = (index + 1) & set->mask;
    }
    return 0;
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

/* @implNote The lexical target check is judged per member, but an earlier
   symlink changes what a later member's path MEANS: "a/b" -> ".." is contained
   on its own, and "a/b/c/evil" written through it is not. Every link daukle
   creates is recorded here and no member may be reached through one, which
   closes the chain the per-member check cannot see. A fresh temporary tree with
   a single writer makes the record complete and unraceable. The name is
   recorded on Windows too, where no link is created, so the refusal is the same
   sentence on both platforms rather than one the host happens to provide. */
static int refuse_reaching_through_a_link(const char *name, const name_set *links,
                                          fr_error *err) {
    char prefix[UNPACK_MAX_NATIVE];
    size_t length = strlen(name);
    if (length >= sizeof prefix) {
        fr_error_set(err, "the member \"%s\" is too long to check against the links already"
                          " created", name);
        return FR_ERR;
    }
    memcpy(prefix, name, length + 1);

    for (size_t index = 0; index < length; index++) {
        if (prefix[index] != '/') continue;

        prefix[index] = '\0';
        int through_a_link = name_set_contains(links, prefix);
        prefix[index] = '/';
        if (through_a_link) {
            fr_error_set(err, "the member \"%s\" would be written through the symlink \"%.*s\"",
                         name, (int) index, name);
            return FR_ERR;
        }
    }
    return FR_OK;
}

/* A member name may carry the one trailing separator the name check allows,
   and an ancestor prefix never does, so it is stripped before recording. */
static int record_link(const char *name, name_set *links, fr_error *err) {
    char trimmed[UNPACK_MAX_NATIVE];
    size_t length = strlen(name);
    while (length > 0 && name[length - 1] == '/') length--;
    if (length == 0 || length >= sizeof trimmed) {
        fr_error_set(err, "the symlink member \"%s\" cannot be recorded", name);
        return FR_ERR;
    }

    memcpy(trimmed, name, length);
    trimmed[length] = '\0';
    if (name_set_add(links, trimmed) < 0) {
        fr_error_set(err, "out of memory while unpacking \"%s\"", name);
        return FR_ERR;
    }
    return FR_OK;
}

static int refuse_oversized_member(const fr_archive_member *member,
                                   const fr_unpack_limits *limits, size_t *total, fr_error *err) {
    if (member->payload_length > limits->max_member_bytes) {
        fr_error_set(err, "the member \"%s\" is larger than the %zu byte limit", member->name,
                     limits->max_member_bytes);
        return FR_ERR;
    }
    if (member->payload_length > limits->max_total_bytes - *total) {
        fr_error_set(err, "the archive expands past the %zu byte limit", limits->max_total_bytes);
        return FR_ERR;
    }

    *total += member->payload_length;
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

static char *duplicate_text(const char *text) {
    size_t length = strlen(text) + 1;
    char *copy = malloc(length);
    if (copy != NULL) memcpy(copy, text, length);
    return copy;
}

static int g_links_supported = 1;

void fr_unpack_set_links_supported(int supported) {
    g_links_supported = supported;
}

/* Non-zero when a real link now exists at native. The target is validated by
   refuse_unsafe_link_target before this runs, so nothing here judges it again.

   @implNote the target is passed through as the archive spelled it, relative
   and forward-slashed, because that is what makes the link mean the same thing
   wherever the tree is later moved to. Windows is asked for a FILE link even
   though the target may not exist yet: every link in every archive this project
   provisions points at a file (measured, 52 of 52 in xpack clang 21.1.8-1), and
   guessing wrong costs a link rather than the unpack, since a refused create
   falls through to the copy below. */
static int create_native_link(const char *native, const char *target) {
    if (!g_links_supported) return 0;
#ifdef _WIN32
    char backslashed[UNPACK_MAX_NATIVE];
    int written = snprintf(backslashed, sizeof backslashed, "%s", target);
    if (written < 0 || (size_t) written >= sizeof backslashed) return 0;
    for (char *cursor = backslashed; *cursor != '\0'; cursor++) {
        if (*cursor == '/') *cursor = NATIVE_SEPARATOR;
    }
    return CreateSymbolicLinkA(native, backslashed,
                               SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != 0;
#else
    return symlink(target, native) == 0;
#endif
}

/* A link whose create was refused, kept until the stream ends. It cannot be
   resolved where it is found: a link's target may arrive later in the archive
   and may itself be a link, which 9 of xpack clang's 52 are. D-57. */
typedef struct {
    char *name;
    char *target;
} deferred_link;

typedef struct {
    deferred_link *items;
    size_t count;
} deferred_links;

static int deferred_links_add(deferred_links *set, const char *name, const char *target) {
    deferred_link *items = realloc(set->items, (set->count + 1) * sizeof *items);
    if (items == NULL) return 0;
    set->items = items;

    deferred_link *entry = &items[set->count];
    entry->name = duplicate_text(name);
    entry->target = duplicate_text(target);
    if (entry->name == NULL || entry->target == NULL) {
        free(entry->name);
        free(entry->target);
        return 0;
    }
    set->count++;
    return 1;
}

static void deferred_links_free(deferred_links *set) {
    for (size_t index = 0; index < set->count; index++) {
        free(set->items[index].name);
        free(set->items[index].target);
    }
    free(set->items);
    set->items = NULL;
    set->count = 0;
}

/* D-38 said daukle must not DEPEND on symlinks, which this keeps: a link is
   created where the platform has them and the target's bytes are copied where
   it does not, so the tree is complete either way and no filesystem is
   required to support anything. Skipping the member outright, which is what
   this did until 2026-10-03, left four load-bearing links out of a provisioned
   clang, two of them SONAME aliases without which the compiler will not
   start. D-57. */
static int write_symlink_member(const fr_archive_member *member, const native_root *root,
                                deferred_links *deferred, fr_unpack_report *report,
                                fr_error *err) {
    if (refuse_unsafe_link_target(member, err) != FR_OK) return FR_ERR;

    char native[UNPACK_MAX_NATIVE];
    if (join_under_root(root, member->name, native, sizeof native, err) != FR_OK) return FR_ERR;
    if (make_parent_directories(native, root->length, member->name, report, err) != FR_OK) {
        return FR_ERR;
    }

    if (create_native_link(native, member->link_target)) {
        report->symlinks_created++;
        return FR_OK;
    }
    if (!deferred_links_add(deferred, member->name, member->link_target)) {
        fr_error_set(err, "out of memory recording the link \"%s\"", member->name);
        return FR_ERR;
    }
    return FR_OK;
}

/* Joins a link's target onto the directory the link sits in and normalises the
   result to a destination-relative path. The target is known to stay inside by
   refuse_unsafe_link_target, so this only has to fold "." and ".." rather than
   judge them. */
static int resolve_target_name(const char *link_name, const char *target, char *out,
                               size_t out_size) {
    char joined[UNPACK_MAX_NATIVE];
    const char *last_slash = strrchr(link_name, '/');
    int written = last_slash == NULL
                ? snprintf(joined, sizeof joined, "%s", target)
                : snprintf(joined, sizeof joined, "%.*s/%s",
                           (int) (last_slash - link_name), link_name, target);
    if (written < 0 || (size_t) written >= sizeof joined) return 0;

    char *stack[256];
    size_t depth = 0;
    for (char *component = joined; component != NULL; ) {
        char *slash = strchr(component, '/');
        if (slash != NULL) *slash = '\0';
        if (strcmp(component, "..") == 0) {
            if (depth > 0) depth--;
        } else if (component[0] != '\0' && strcmp(component, ".") != 0) {
            if (depth == sizeof stack / sizeof stack[0]) return 0;
            stack[depth++] = component;
        }
        component = slash == NULL ? NULL : slash + 1;
    }

    size_t length = 0;
    for (size_t index = 0; index < depth; index++) {
        int part = snprintf(out + length, out_size - length, index == 0 ? "%s" : "/%s",
                            stack[index]);
        if (part < 0 || (size_t) part >= out_size - length) return 0;
        length += (size_t) part;
    }
    return length > 0;
}

static const deferred_link *deferred_links_find(const deferred_links *set, const char *name) {
    for (size_t index = 0; index < set->count; index++) {
        if (strcmp(set->items[index].name, name) == 0) return &set->items[index];
    }
    return NULL;
}

/* A link may point at another link, and 9 of the 52 in xpack clang 21.1.8-1
   do. The bound is what stops a hostile archive looping; the real archives
   measured need 2. */
#define UNPACK_MAX_LINK_HOPS 8

static int resolve_through_links(const deferred_links *deferred, const char *name, char *out,
                                 size_t out_size) {
    char current[UNPACK_MAX_NATIVE];
    if (snprintf(current, sizeof current, "%s", name) < 0) return 0;

    for (size_t hop = 0; hop <= UNPACK_MAX_LINK_HOPS; hop++) {
        const deferred_link *link = deferred_links_find(deferred, current);
        if (link == NULL) {
            return snprintf(out, out_size, "%s", current) > 0;
        }
        char next[UNPACK_MAX_NATIVE];
        if (!resolve_target_name(current, link->target, next, sizeof next)) return 0;
        /* A link resolving to itself would otherwise sit under the hop bound
           forever rather than being refused by it. */
        if (strcmp(next, current) == 0) return 0;
        memcpy(current, next, strlen(next) + 1);
    }
    return 0;
}

static int read_whole_file(const char *native, char **out, size_t *out_length) {
    FILE *file = fopen(native, "rb");
    if (file == NULL) return 0;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 0; }
    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return 0; }

    char *bytes = malloc((size_t) length + 1);
    if (bytes == NULL) { fclose(file); return 0; }
    size_t read = length == 0 ? 0 : fread(bytes, 1, (size_t) length, file);
    fclose(file);
    if (read != (size_t) length) { free(bytes); return 0; }

    *out = bytes;
    *out_length = read;
    return 1;
}

static void record_unresolved(const char *name, fr_unpack_report *report) {
    if (report->symlinks_unresolved == 0) {
        snprintf(report->first_symlink_unresolved, sizeof report->first_symlink_unresolved,
                 "%.*s", (int) (sizeof report->first_symlink_unresolved - 1), name);
    }
    report->symlinks_unresolved++;
}

/* Runs once the stream has ended, which is the only point at which a link's
   target is known to have arrived.

   An unresolvable target is REPORTED AND NOT FATAL, which is measured rather
   than lenient: xpack clang 21.1.8-1 ships bin/llvm-ml64 pointing at llvm-ml,
   a file no member of that archive writes. Failing here would refuse the real
   archive outright on every filesystem without links. A target that resolves
   to a directory is reported the same way: of 52 links in that archive none
   points at one, so copying a tree would be unbounded work for a case the
   archives do not have. */
static int restore_deferred_links(const deferred_links *deferred, const native_root *root,
                                  const fr_unpack_limits *limits, size_t *total,
                                  fr_unpack_report *report, fr_error *err) {
    for (size_t index = 0; index < deferred->count; index++) {
        const deferred_link *link = &deferred->items[index];

        char resolved[UNPACK_MAX_NATIVE];
        if (!resolve_through_links(deferred, link->name, resolved, sizeof resolved)) {
            record_unresolved(link->name, report);
            continue;
        }

        char source[UNPACK_MAX_NATIVE];
        if (join_under_root(root, resolved, source, sizeof source, err) != FR_OK) return FR_ERR;

        char *bytes = NULL;
        size_t length = 0;
        if (!read_whole_file(source, &bytes, &length)) {
            record_unresolved(link->name, report);
            continue;
        }

        /* The copy is expansion the archive did not declare, so it is held to
           the same ceiling every member is. D-54. */
        if (length > limits->max_total_bytes - *total) {
            free(bytes);
            fr_error_set(err, "the archive expands to more than %zu bytes",
                         limits->max_total_bytes);
            return FR_ERR;
        }
        *total += length;

        char destination[UNPACK_MAX_NATIVE];
        if (join_under_root(root, link->name, destination, sizeof destination, err) != FR_OK) {
            free(bytes);
            return FR_ERR;
        }
        int written = write_file_bytes(destination, bytes, length, link->name, err);
        free(bytes);
        if (written != FR_OK) return FR_ERR;

        report->symlinks_copied++;
    }
    return FR_OK;
}

static int write_member(const fr_archive_member *member, const native_root *root,
                        const fr_unpack_limits *limits, name_set *seen, name_set *links,
                        deferred_links *deferred, size_t *total, fr_unpack_report *report,
                        fr_error *err) {
    if (refuse_unwritable_name(member, limits, seen, err) != FR_OK) return FR_ERR;
    if (refuse_reaching_through_a_link(member->name, links, err) != FR_OK) return FR_ERR;
    /* Counted before the kind is looked at, because the reader has already
       consumed (and for a .tar.gz inflated) the payload of every kind, so a
       ceiling that skipped a directory member would not bound the expansion it
       claims to. */
    if (refuse_oversized_member(member, limits, total, err) != FR_OK) return FR_ERR;

    if (member->kind == FR_MEMBER_SYMLINK) {
        if (write_symlink_member(member, root, deferred, report, err) != FR_OK) return FR_ERR;
        return record_link(member->name, links, err);
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
    name_set links;
    memset(&links, 0, sizeof links);
    deferred_links deferred;
    memset(&deferred, 0, sizeof deferred);

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

        result = write_member(member, &root, limits, &seen, &links, &deferred, &total, report,
                              err);
        if (result != FR_OK) break;
    }

    /* Only once the stream ended cleanly: a half-read archive's links would be
       resolved against a tree that is missing the members still to come, and
       every one of them would be reported unresolved for a reason that is not
       the archive's. */
    if (result == FR_OK) {
        result = restore_deferred_links(&deferred, &root, limits, &total, report, err);
    }

    deferred_links_free(&deferred);
    name_set_free(&seen);
    name_set_free(&links);
    return result;
}
