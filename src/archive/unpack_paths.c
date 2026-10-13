#include "archive/unpack.h"
#include "archive/unpack_internal.h"

#include "lua/lua_sandbox.h"
#include "util/error.h"

#include <errno.h>
#include <stdio.h>
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
int fr_unpack_refuse_unsafe_link_target(const fr_archive_member *member, fr_error *err) {
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

/* @implNote The "\\?\" prefix a deep member needs past MAX_PATH also turns off
   the normalisation that would otherwise accept a forward slash or a relative
   segment, so the destination is made fully qualified and backslashed here,
   once, and every join below inherits both. Member names stay forward-slashed
   everywhere else in this file. */
int fr_unpack_native_root_of(const char *destination, native_root *out, fr_error *err) {
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

int fr_unpack_join_under_root(const native_root *root, const char *relative, char *out,
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

int fr_unpack_make_directory_tree(char *native, size_t root_length, const char *relative,
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

int fr_unpack_make_parent_directories(char *native, size_t root_length, const char *relative,
                                      fr_unpack_report *report, fr_error *err) {
    char *last = strrchr(native + root_length + 1, NATIVE_SEPARATOR);
    if (last == NULL) return FR_OK;

    *last = '\0';
    int made = fr_unpack_make_directory_tree(native, root_length, relative, report, err);
    *last = NATIVE_SEPARATOR;
    return made;
}

/* Windows carries no mode to apply: an extension decides what runs there. */
int fr_unpack_apply_permissions(const char *native, const fr_archive_member *member, fr_error *err) {
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

int fr_unpack_write_file_bytes(const char *native, const char *bytes, size_t length,
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
