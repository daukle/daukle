#include "toolreport.h"

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

/* An unsafe label is treated the same as an absent one. Neither producer
   function below has a way to refuse a caller's argument, so silently
   keeping an unsafe label would hand the terminal to whatever supplied it,
   which is the exact outcome fr_toolreport_label_is_safe exists to prevent. */
static const char *effective_label(const char *label, const char *fallback) {
    if (label != NULL && fr_toolreport_label_is_safe(label)) return label;
    return fallback;
}

static const char *url_last_component(const char *url) {
    if (url == NULL) return "";
    const char *slash = strrchr(url, '/');
    return slash != NULL ? slash + 1 : url;
}

static int already_reported(row_kind kind, const char *key) {
    for (size_t index = 0; index < g_row_count; index++) {
        if (g_kinds[index] != kind) continue;
        const char *existing = kind == ROW_INSTALLED ? g_rows[index].url : g_rows[index].digest;
        if (strcmp(existing, key) == 0) return 1;
    }
    return 0;
}

static void record(row_kind kind, const char *label, const char *url, const char *digest,
                   int cached) {
    if (g_row_count == TOOLREPORT_CAPACITY) return;

    fr_toolreport_row *row = &g_rows[g_row_count];
    snprintf(row->label, sizeof row->label, "%s", label);
    snprintf(row->url, sizeof row->url, "%s", url);
    snprintf(row->digest, sizeof row->digest, "%s", digest);
    row->cached = cached;
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

    const char *shown_label = effective_label(label, url_last_component(url));
    fprintf(stderr, "provisioning %s (%s)\n  %s\n  sha256 %s\n", shown_label,
            cached ? "cached" : "downloaded", url, digest);
    record(ROW_PROVISIONED, shown_label, url, digest, cached);
}

size_t fr_toolreport_row_count(void) {
    return g_row_count;
}

const fr_toolreport_row *fr_toolreport_row_at(size_t index) {
    if (index >= g_row_count) return NULL;
    return &g_rows[index];
}
