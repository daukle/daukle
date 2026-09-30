#include "exec/toolreport.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define TOOLREPORT_CAPACITY 64

typedef enum { ROW_INSTALLED, ROW_PROVISIONED } row_kind;

static fr_toolreport_row g_rows[TOOLREPORT_CAPACITY];
static row_kind g_kinds[TOOLREPORT_CAPACITY];
static size_t g_row_count;

int fr_toolreport_label_is_safe(const char *label) {
    if (label == NULL) return 0;
    for (const unsigned char *cursor = (const unsigned char *) label; *cursor != '\0'; cursor++) {
        if (iscntrl(*cursor)) return 0;
    }
    return 1;
}

void fr_toolreport_reset(void) {
    g_row_count = 0;
}

/* An unsafe or empty label is treated the same as an absent one. Neither
   producer function below has a way to refuse a caller's argument, and an
   empty string passes fr_toolreport_label_is_safe vacuously, so both would
   otherwise print a blank where the rule "a missing label falls back to a
   fact" requires one. */
static const char *effective_label(const char *label, const char *fallback) {
    if (label != NULL && label[0] != '\0' && fr_toolreport_label_is_safe(label)) return label;
    return fallback;
}

/* Trailing separators are stripped before searching, so a url ending in "/"
   names its directory rather than nothing. If the url is nothing but
   separators, out is the whole url: still a fact, never blank. */
static void url_last_component(const char *url, char *out, size_t out_size) {
    if (url == NULL) {
        out[0] = '\0';
        return;
    }

    size_t end = strlen(url);
    while (end > 0 && url[end - 1] == '/') end--;

    size_t start = end;
    while (start > 0 && url[start - 1] != '/') start--;

    if (start == end) {
        snprintf(out, out_size, "%s", url);
        return;
    }
    size_t length = end - start;
    if (length >= out_size) length = out_size - 1;
    memcpy(out, url + start, length);
    out[length] = '\0';
}

static int already_reported(row_kind kind, const char *key) {
    for (size_t index = 0; index < g_row_count; index++) {
        if (g_kinds[index] != kind) continue;
        const char *existing = kind == ROW_INSTALLED ? g_rows[index].url : g_rows[index].digest;
        if (strcmp(existing, key) == 0) return 1;
    }
    return 0;
}

/* Cleared whole rather than field by field: fr_toolreport_reset drops the row
   count and leaves the storage, so this writes over a previous run's row, and a
   field left out here would be inherited rather than set. That is not only a
   stale number. fr_toolreport_symlinks_skipped refuses a row that already
   carries a count, so an inherited one SUPPRESSES the next run's real report,
   which is the silent degradation this module exists to prevent. */
static void record(row_kind kind, const char *label, const char *url, const char *digest,
                   int cached) {
    if (g_row_count == TOOLREPORT_CAPACITY) return;

    fr_toolreport_row *row = &g_rows[g_row_count];
    memset(row, 0, sizeof *row);
    snprintf(row->label, sizeof row->label, "%s", label);
    snprintf(row->url, sizeof row->url, "%s", url);
    snprintf(row->digest, sizeof row->digest, "%s", digest);
    row->cached = cached;
    row->provisioned = kind == ROW_PROVISIONED;
    g_kinds[g_row_count] = kind;
    g_row_count++;
}

void fr_toolreport_used_installed(const char *name, const char *label, const char *path) {
    if (already_reported(ROW_INSTALLED, path)) return;

    const char *shown_label = effective_label(label, name);
    fprintf(stderr, "using %s (%s)\n", shown_label, path);
    record(ROW_INSTALLED, shown_label, path, "", 0);
}

void fr_toolreport_provisioned(const char *label, const char *url, const char *digest,
                               int cached) {
    if (already_reported(ROW_PROVISIONED, digest)) return;

    /* Sized like fr_toolreport_row.url, since a whole-url fallback becomes it. */
    char last_component[1024];
    url_last_component(url, last_component, sizeof last_component);
    const char *shown_label = effective_label(label, last_component);
    fprintf(stderr, "provisioning %s (%s)\n  %s\n  sha256 %s\n", shown_label,
            cached ? "cached" : "downloaded", url, digest);
    record(ROW_PROVISIONED, shown_label, url, digest, cached);
}

static fr_toolreport_row *provisioned_row_for(const char *digest) {
    for (size_t index = 0; index < g_row_count; index++) {
        if (g_kinds[index] == ROW_PROVISIONED && strcmp(g_rows[index].digest, digest) == 0) {
            return &g_rows[index];
        }
    }
    return NULL;
}

void fr_toolreport_symlinks_skipped(const char *digest, size_t count, const char *first_name) {
    if (count == 0 || digest == NULL) return;

    fr_toolreport_row *row = provisioned_row_for(digest);
    if (row == NULL || row->symlinks_skipped != 0) return;

    row->symlinks_skipped = count;
    snprintf(row->first_symlink_skipped, sizeof row->first_symlink_skipped, "%s",
             first_name == NULL ? "" : first_name);
    fprintf(stderr, "  %zu symlink%s not created, first %s\n", count, count == 1 ? "" : "s",
            row->first_symlink_skipped);
}

size_t fr_toolreport_row_count(void) {
    return g_row_count;
}

const fr_toolreport_row *fr_toolreport_row_at(size_t index) {
    if (index >= g_row_count) return NULL;
    return &g_rows[index];
}
