#include "migrate/proof.h"

#include "archive/archive.h"
#include "plugin/plugins.h"
#include "project/region.h"
#include "util/error.h"
#include "util/sha256.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REPORTED_LINES 5u
#define REPORTED_LINE_BYTES 80u
#define MAX_MEMBER_BYTES (64u * 1024u * 1024u)
#define MAX_MEMBERS 100000u
#define PATH_SIZE 1024

typedef struct {
    char *name;
    char digest[65];
    int is_symlink;
} proof_member;

typedef struct {
    proof_member *members;
    size_t count;
    size_t capacity;
} proof_members;

static void free_members(proof_members *list) {
    for (size_t index = 0; index < list->count; index++) free(list->members[index].name);
    free(list->members);
    list->members = NULL;
    list->count = 0;
    list->capacity = 0;
}

static int join_path(const char *root, const char *relative, char *out, fr_error *err) {
    int written = snprintf(out, PATH_SIZE, "%s/%s", root, relative);
    if (written < 0 || written >= PATH_SIZE) {
        fr_error_set(err, "comparing build outputs: the path \"%s\" below \"%s\" is too long",
                     relative, root);
        return FR_ERR;
    }
    return FR_OK;
}

static int grow_members(proof_members *list, fr_error *err) {
    if (list->count < list->capacity) return FR_OK;
    size_t capacity = list->capacity == 0 ? 16 : list->capacity * 2;
    proof_member *grown = realloc(list->members, capacity * sizeof *grown);
    if (grown == NULL) {
        fr_error_set(err, "out of memory listing the members of an archive");
        return FR_ERR;
    }
    list->members = grown;
    list->capacity = capacity;
    return FR_OK;
}

static int add_member(proof_members *list, const fr_archive_member *member, const char *path,
                      fr_error *err) {
    if (list->count >= MAX_MEMBERS) {
        fr_error_set(err, "comparing \"%s\": it holds more than %u members", path, MAX_MEMBERS);
        return FR_ERR;
    }
    if (grow_members(list, err) != FR_OK) return FR_ERR;
    proof_member *entry = &list->members[list->count];
    entry->name = fr_dup_string(member->name);
    if (entry->name == NULL) {
        fr_error_set(err, "out of memory listing the members of \"%s\"", path);
        return FR_ERR;
    }
    entry->is_symlink = member->kind == FR_MEMBER_SYMLINK;
    entry->digest[0] = '\0';
    if (!entry->is_symlink) fr_sha256_hex(member->bytes, member->length, entry->digest);
    list->count++;
    return FR_OK;
}

static int compare_members_by_name(const void *left, const void *right) {
    return strcmp(((const proof_member *) left)->name, ((const proof_member *) right)->name);
}

static int list_archive(const char *path, proof_members *list, fr_error *err) {
    fr_archive *archive = NULL;
    if (fr_archive_open(path, MAX_MEMBER_BYTES, &archive, err) != FR_OK) return FR_ERR;
    const fr_archive_member *member = NULL;
    for (;;) {
        if (fr_archive_next(archive, &member, err) != FR_OK) break;
        if (member == NULL) {
            fr_archive_close(archive);
            qsort(list->members, list->count, sizeof *list->members, compare_members_by_name);
            return FR_OK;
        }
        if (member->kind == FR_MEMBER_DIRECTORY) continue;
        if (add_member(list, member, path, err) != FR_OK) break;
    }
    fr_archive_close(archive);
    free_members(list);
    return FR_ERR;
}

static int members_equal(const proof_member *old_member, const proof_member *new_member,
                         char *difference, size_t difference_size) {
    if (old_member->is_symlink || new_member->is_symlink) {
        snprintf(difference, difference_size, "member \"%s\" is a symlink in the %s archive",
                 old_member->name, old_member->is_symlink ? "old" : "new");
        return 0;
    }
    if (strcmp(old_member->digest, new_member->digest) != 0) {
        snprintf(difference, difference_size,
                 "member \"%s\" has different bytes in the old and the new archive",
                 old_member->name);
        return 0;
    }
    return 1;
}

static int walk_sorted_members(const proof_members *old_list, const proof_members *new_list,
                               char *difference, size_t difference_size) {
    size_t old_index = 0;
    size_t new_index = 0;
    while (old_index < old_list->count || new_index < new_list->count) {
        int order = 0;
        if (old_index >= old_list->count) order = 1;
        else if (new_index >= new_list->count) order = -1;
        else order = strcmp(old_list->members[old_index].name, new_list->members[new_index].name);

        if (order < 0) {
            snprintf(difference, difference_size,
                     "member \"%s\" is in the old archive but not the new archive",
                     old_list->members[old_index].name);
            return 0;
        }
        if (order > 0) {
            snprintf(difference, difference_size,
                     "member \"%s\" is in the new archive but not the old archive",
                     new_list->members[new_index].name);
            return 0;
        }
        if (!members_equal(&old_list->members[old_index], &new_list->members[new_index],
                           difference, difference_size)) {
            return 0;
        }
        old_index++;
        new_index++;
    }
    return 1;
}

static int compare_archives(const char *old_path, const char *new_path, int *out_same,
                            char *difference, size_t difference_size, fr_error *err) {
    proof_members old_list = { 0 };
    proof_members new_list = { 0 };
    if (list_archive(old_path, &old_list, err) != FR_OK) return FR_ERR;
    if (list_archive(new_path, &new_list, err) != FR_OK) {
        free_members(&old_list);
        return FR_ERR;
    }
    *out_same = walk_sorted_members(&old_list, &new_list, difference, difference_size);
    free_members(&old_list);
    free_members(&new_list);
    return FR_OK;
}

typedef struct {
    const char *start;
    size_t length;
} proof_line;

typedef struct {
    proof_line *lines;
    unsigned char *matched;
    size_t count;
} proof_lines;

typedef struct {
    char *text;
    size_t size;
    size_t used;
} proof_report;

static void report_append(proof_report *report, const char *format, ...) {
    if (report->used + 1 >= report->size) return;
    va_list arguments;
    va_start(arguments, format);
    int written = vsnprintf(report->text + report->used, report->size - report->used, format,
                            arguments);
    va_end(arguments);
    if (written < 0) return;
    size_t room = report->size - report->used - 1;
    report->used += (size_t) written < room ? (size_t) written : room;
}

static void report_quoted(proof_report *report, proof_line line) {
    size_t shown = line.length < REPORTED_LINE_BYTES ? line.length : REPORTED_LINE_BYTES;
    report_append(report, "\"");
    for (size_t index = 0; index < shown; index++) {
        char byte = line.start[index];
        if (byte == '\r') report_append(report, "\\r");
        else if (byte == '\t') report_append(report, "\\t");
        else if (byte == '"') report_append(report, "\\\"");
        else report_append(report, "%c", byte);
    }
    report_append(report, shown < line.length ? "...\"" : "\"");
}

static int split_lines(const char *bytes, size_t length, proof_lines *out) {
    size_t count = 0;
    for (size_t index = 0; index < length; index++) {
        if (bytes[index] == '\n') count++;
    }
    if (length > 0 && bytes[length - 1] != '\n') count++;
    out->lines = calloc(count > 0 ? count : 1, sizeof *out->lines);
    out->matched = calloc(count > 0 ? count : 1, 1);
    if (out->lines == NULL || out->matched == NULL) return FR_ERR;
    size_t begin = 0;
    out->count = 0;
    for (size_t index = 0; index <= length; index++) {
        if (index < length && bytes[index] != '\n') continue;
        if (index == length && begin == length) break;
        out->lines[out->count].start = bytes + begin;
        out->lines[out->count].length = index - begin;
        out->count++;
        begin = index + 1;
    }
    return FR_OK;
}

static void match_lines(proof_lines *old_lines, proof_lines *new_lines) {
    for (size_t old_index = 0; old_index < old_lines->count; old_index++) {
        proof_line wanted = old_lines->lines[old_index];
        for (size_t new_index = 0; new_index < new_lines->count; new_index++) {
            proof_line candidate = new_lines->lines[new_index];
            if (new_lines->matched[new_index] || candidate.length != wanted.length
                || memcmp(candidate.start, wanted.start, wanted.length) != 0) {
                continue;
            }
            old_lines->matched[old_index] = 1;
            new_lines->matched[new_index] = 1;
            break;
        }
    }
}

static size_t report_side(proof_report *report, const char *side, const proof_lines *lines) {
    size_t unmatched = 0;
    for (size_t index = 0; index < lines->count; index++) unmatched += !lines->matched[index];
    if (unmatched == 0) return 0;
    report_append(report, "%slines only in the %s build: ", report->used > 0 ? "; " : "", side);
    size_t shown = 0;
    for (size_t index = 0; index < lines->count && shown < REPORTED_LINES; index++) {
        if (lines->matched[index]) continue;
        if (shown > 0) report_append(report, ", ");
        report_quoted(report, lines->lines[index]);
        shown++;
    }
    if (unmatched > shown) report_append(report, " and %zu more", unmatched - shown);
    return unmatched;
}

/* @implNote lines are compared as a multiset, so a sorted listing such as a dependency tree
   names exactly the entries that differ; a binary file, recognised by a NUL byte, keeps the
   digest-only report because its "lines" mean nothing. */
static void report_line_difference(proof_report *report, const char *old_bytes, size_t old_length,
                                   const char *new_bytes, size_t new_length) {
    if ((old_length > 0 && memchr(old_bytes, '\0', old_length) != NULL)
        || (new_length > 0 && memchr(new_bytes, '\0', new_length) != NULL)) {
        return;
    }
    proof_lines old_lines = { 0 };
    proof_lines new_lines = { 0 };
    if (split_lines(old_bytes, old_length, &old_lines) == FR_OK
        && split_lines(new_bytes, new_length, &new_lines) == FR_OK) {
        match_lines(&old_lines, &new_lines);
        size_t differing = report_side(report, "old", &old_lines);
        differing += report_side(report, "new", &new_lines);
        if (differing == 0) report_append(report, "the same lines in a different order");
    }
    free(old_lines.lines);
    free(old_lines.matched);
    free(new_lines.lines);
    free(new_lines.matched);
}

static int compare_bytes(const char *old_path, const char *new_path, int *out_same,
                         char *difference, size_t difference_size, fr_error *err) {
    char *old_bytes = NULL;
    char *new_bytes = NULL;
    size_t old_length = 0;
    size_t new_length = 0;
    if (fr_file_read_bytes(old_path, &old_bytes, &old_length, err) != FR_OK) return FR_ERR;
    if (fr_file_read_bytes(new_path, &new_bytes, &new_length, err) != FR_OK) {
        free(old_bytes);
        return FR_ERR;
    }
    char old_digest[65];
    char new_digest[65];
    fr_sha256_hex(old_bytes, old_length, old_digest);
    fr_sha256_hex(new_bytes, new_length, new_digest);
    *out_same = fr_sha256_hex_equal(old_digest, new_digest);
    if (!*out_same && difference_size > 0) {
        proof_report report = { difference, difference_size, 0 };
        difference[0] = '\0';
        report_line_difference(&report, old_bytes, old_length, new_bytes, new_length);
        report_append(&report, "%sthe old build's sha256 is %s and the new build's is %s",
                      report.used > 0 ? "; " : "", old_digest, new_digest);
    }
    free(old_bytes);
    free(new_bytes);
    return FR_OK;
}

int fr_proof_compare(const char *root, const char *old_path, const char *new_path,
                     fr_proof_kind kind, int *out_same, char *difference,
                     size_t difference_size, fr_error *err) {
    *out_same = 0;
    if (difference_size > 0) difference[0] = '\0';
    char old_full[PATH_SIZE];
    char new_full[PATH_SIZE];
    if (join_path(root, old_path, old_full, err) != FR_OK
        || join_path(root, new_path, new_full, err) != FR_OK) {
        return FR_ERR;
    }
    if (kind == FR_PROOF_ARCHIVE) {
        return compare_archives(old_full, new_full, out_same, difference, difference_size, err);
    }
    return compare_bytes(old_full, new_full, out_same, difference, difference_size, err);
}

int fr_proof_kind_parse(const char *text, fr_proof_kind *out) {
    if (strcmp(text, "bytes") == 0) {
        *out = FR_PROOF_BYTES;
        return FR_OK;
    }
    if (strcmp(text, "archive") == 0) {
        *out = FR_PROOF_ARCHIVE;
        return FR_OK;
    }
    return FR_ERR;
}
