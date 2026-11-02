#include "migrate/proof.h"
#include "migrate/proof_internal.h"

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
#define PATH_SIZE 1024

static int join_path(const char *root, const char *relative, char *out, fr_error *err) {
    int written = snprintf(out, PATH_SIZE, "%s/%s", root, relative);
    if (written < 0 || written >= PATH_SIZE) {
        fr_error_set(err, "comparing build outputs: the path \"%s\" below \"%s\" is too long",
                     relative, root);
        return FR_ERR;
    }
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

typedef struct {
    proof_line line;
    size_t index;
} proof_sorted_line;

static int compare_lines(proof_line left, proof_line right) {
    size_t shorter = left.length < right.length ? left.length : right.length;
    int order = shorter > 0 ? memcmp(left.start, right.start, shorter) : 0;
    if (order != 0) return order;
    return (left.length > right.length) - (left.length < right.length);
}

static int compare_sorted_lines(const void *left, const void *right) {
    const proof_sorted_line *a = left;
    const proof_sorted_line *b = right;
    int order = compare_lines(a->line, b->line);
    if (order != 0) return order;
    return (a->index > b->index) - (a->index < b->index);
}

static proof_sorted_line *sorted_copy(const proof_lines *lines) {
    proof_sorted_line *sorted = malloc((lines->count > 0 ? lines->count : 1) * sizeof *sorted);
    if (sorted == NULL) return NULL;
    for (size_t index = 0; index < lines->count; index++) {
        sorted[index].line = lines->lines[index];
        sorted[index].index = index;
    }
    qsort(sorted, lines->count, sizeof *sorted, compare_sorted_lines);
    return sorted;
}

/* @implNote sorted and merged rather than searched pairwise, because a proof file can be a
   generated listing of hundreds of thousands of lines and a pairwise search is quadratic. */
static int match_lines(proof_lines *old_lines, proof_lines *new_lines) {
    proof_sorted_line *old_sorted = sorted_copy(old_lines);
    proof_sorted_line *new_sorted = sorted_copy(new_lines);
    if (old_sorted == NULL || new_sorted == NULL) {
        free(old_sorted);
        free(new_sorted);
        return FR_ERR;
    }
    size_t old_at = 0;
    size_t new_at = 0;
    while (old_at < old_lines->count && new_at < new_lines->count) {
        int order = compare_lines(old_sorted[old_at].line, new_sorted[new_at].line);
        if (order < 0) {
            old_at++;
        } else if (order > 0) {
            new_at++;
        } else {
            old_lines->matched[old_sorted[old_at++].index] = 1;
            new_lines->matched[new_sorted[new_at++].index] = 1;
        }
    }
    free(old_sorted);
    free(new_sorted);
    return FR_OK;
}

static int same_sequence(const proof_lines *old_lines, const proof_lines *new_lines) {
    if (old_lines->count != new_lines->count) return 0;
    for (size_t index = 0; index < old_lines->count; index++) {
        if (compare_lines(old_lines->lines[index], new_lines->lines[index]) != 0) return 0;
    }
    return 1;
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
        && split_lines(new_bytes, new_length, &new_lines) == FR_OK
        && match_lines(&old_lines, &new_lines) == FR_OK) {
        size_t differing = report_side(report, "old", &old_lines);
        differing += report_side(report, "new", &new_lines);
        if (differing == 0) {
            report_append(report, same_sequence(&old_lines, &new_lines)
                                      ? "only the final newline differs"
                                      : "the same lines in a different order");
        }
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
        return fr_proof_compare_archives(old_full, new_full, out_same, difference, difference_size, err);
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
