#include "archive/unpack.h"
#include "archive/unpack_internal.h"

#include "util/error.h"

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

void fr_unpack_deferred_links_free(deferred_links *set) {
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
int fr_unpack_write_symlink_member(const fr_archive_member *member, const native_root *root,
                                   deferred_links *deferred, fr_unpack_report *report,
                                   fr_error *err) {
    if (fr_unpack_refuse_unsafe_link_target(member, err) != FR_OK) return FR_ERR;

    char native[UNPACK_MAX_NATIVE];
    if (fr_unpack_join_under_root(root, member->name, native, sizeof native, err) != FR_OK) return FR_ERR;
    if (fr_unpack_make_parent_directories(native, root->length, member->name, report, err) != FR_OK) {
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
int fr_unpack_restore_deferred_links(const deferred_links *deferred, const native_root *root,
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
        if (fr_unpack_join_under_root(root, resolved, source, sizeof source, err) != FR_OK) return FR_ERR;

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
        if (fr_unpack_join_under_root(root, link->name, destination, sizeof destination, err) != FR_OK) {
            free(bytes);
            return FR_ERR;
        }
        int written = fr_unpack_write_file_bytes(destination, bytes, length, link->name, err);
        free(bytes);
        if (written != FR_OK) return FR_ERR;

        report->symlinks_copied++;
    }
    return FR_OK;
}
