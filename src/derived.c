#include "derived.h"

#include "cache.h"
#include "error.h"
#include "lua_sandbox.h"
#include "plugins.h"
#include "region.h"
#include "sha256.h"
#include "strbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FR_DERIVED_LEDGER ".daukle-generated"
#define FR_DERIVED_MAX_FILES 64

typedef struct {
    char *path;
    char digest[65];
} ledger_entry;

static void (*notice_sink)(const char *message);

void fr_derived_set_notice_sink(void (*sink)(const char *message)) {
    notice_sink = sink;
}

static void notice(const char *fmt, ...) {
    if (notice_sink == NULL) return;
    char message[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof message, fmt, args);
    va_end(args);
    notice_sink(message);
}

/* Every path this module joins onto a directory goes through here, so a
   truncation (a path over 1023 bytes) is caught once rather than risking a
   shortened path that still names a real file, which in the sweep below
   would mean deleting the wrong one. The message names only dir, never
   relative: relative is exactly what is huge when this branch fires, and
   fr_error's 512-byte buffer is far short of what a format embedding both
   could need to even reach its own "too long" text. */
static int derived_join(char *out, size_t out_size, const char *dir, const char *relative,
                        fr_error *err) {
    int written = snprintf(out, out_size, "%s/%s", dir, relative);
    if (written < 0 || (size_t) written >= out_size) {
        fr_error_set(err, "a path under \"%s\" is too long to join", dir);
        return FR_ERR;
    }
    return FR_OK;
}

/* A missing or malformed file yields zero entries, which is what makes
   "a missing ledger removes nothing" true by construction rather than by a
   special case later. A line naming a path fr_lua_sandbox_climbs_out rejects
   is dropped rather than kept: the ledger lives in a directory third-party
   tools also write into, so a planted line must never resolve to a delete
   outside derived_dir. */
static int ledger_read(const char *derived_dir, ledger_entry *out, size_t capacity,
                       size_t *out_count, fr_error *err) {
    *out_count = 0;

    char path[1024];
    if (derived_join(path, sizeof path, derived_dir, FR_DERIVED_LEDGER, err) != FR_OK) return FR_ERR;

    char *text = NULL;
    fr_error read_err;
    if (fr_file_read_text(path, &text, &read_err) != FR_OK) return FR_OK;

    size_t count = 0;
    char *cursor = text;
    while (*cursor != '\0' && count < capacity) {
        char *line_end = strchr(cursor, '\n');
        if (line_end != NULL) *line_end = '\0';

        char *tab = strchr(cursor, '\t');
        if (tab != NULL && strlen(tab + 1) == 64) {
            *tab = '\0';
            if (!fr_lua_sandbox_climbs_out(cursor)) {
                out[count].path = fr_dup_string(cursor);
                if (out[count].path != NULL) {
                    memcpy(out[count].digest, tab + 1, 65);
                    count++;
                }
            }
        }
        if (line_end == NULL) break;
        cursor = line_end + 1;
    }
    free(text);
    *out_count = count;
    return FR_OK;
}

static int ledger_entry_order(const void *left, const void *right) {
    return strcmp(((const ledger_entry *) left)->path, ((const ledger_entry *) right)->path);
}

/* Sorted, so two runs that generate the same set produce the same bytes. */
static int ledger_write(const char *derived_dir, ledger_entry *entries, size_t count, fr_error *err) {
    qsort(entries, count, sizeof *entries, ledger_entry_order);

    fr_strbuf buffer;
    fr_strbuf_init(&buffer);
    for (size_t index = 0; index < count; index++) {
        fr_strbuf_append(&buffer, entries[index].path);
        fr_strbuf_append(&buffer, "\t");
        fr_strbuf_append(&buffer, entries[index].digest);
        fr_strbuf_append(&buffer, "\n");
    }

    if (buffer.failed) {
        fr_strbuf_free(&buffer);
        fr_error_set(err, "out of memory building the ledger for \"%s\"", derived_dir);
        return FR_ERR;
    }

    char path[1024];
    if (derived_join(path, sizeof path, derived_dir, FR_DERIVED_LEDGER, err) != FR_OK) {
        fr_strbuf_free(&buffer);
        return FR_ERR;
    }

    int status = fr_cache_write_atomic(path, buffer.data, buffer.length);
    fr_strbuf_free(&buffer);
    if (status != FR_OK) fr_error_set(err, "could not write the ledger for \"%s\"", derived_dir);
    return status;
}

static const ledger_entry *entry_for(const ledger_entry *entries, size_t count, const char *path) {
    for (size_t index = 0; index < count; index++) {
        if (strcmp(entries[index].path, path) == 0) return &entries[index];
    }
    return NULL;
}

static int generated_this_run(const fr_generated_file *files, size_t count, const char *path) {
    for (size_t index = 0; index < count; index++) {
        if (strcmp(files[index].path, path) == 0) return 1;
    }
    return 0;
}

/* sync.c's report_adopt, copied rather than shared: a fr_derived_report is not
   a fr_sync_report, and path here is a caller-owned stack buffer, so this
   copies it rather than adopting ownership. */
static int report_add(fr_derived_report *report, const char *path, fr_error *err) {
    for (size_t index = 0; index < report->count; index++) {
        if (strcmp(report->paths[index], path) == 0) return FR_OK;
    }

    char **paths = realloc(report->paths, (report->count + 1) * sizeof *paths);
    if (paths == NULL) {
        fr_error_set(err, "out of memory recording \"%s\"", path);
        return FR_ERR;
    }
    report->paths = paths;

    char *copy = fr_dup_string(path);
    if (copy == NULL) {
        fr_error_set(err, "out of memory recording \"%s\"", path);
        return FR_ERR;
    }
    report->paths[report->count++] = copy;
    return FR_OK;
}

int fr_derived_ensure_root(const char *derived_root, fr_error *err) {
    char path[1024];
    if (derived_join(path, sizeof path, derived_root, ".gitignore", err) != FR_OK) return FR_ERR;

    FILE *probe = fopen(path, "rb");
    if (probe != NULL) {
        fclose(probe);
        return FR_OK;
    }

    if (fr_cache_write_atomic(path, "*\n", 2) != FR_OK) {
        fr_error_set(err, "could not write \"%s\"", path);
        return FR_ERR;
    }
    return FR_OK;
}

int fr_derived_apply(const char *derived_dir, const fr_generated_file *files, size_t count,
                     int write, fr_derived_report *report, fr_error *err) {
    memset(report, 0, sizeof *report);

    if (count > FR_DERIVED_MAX_FILES) {
        fr_error_set(err, "\"%s\" generates %zu files, more than the %d daukle tracks",
                    derived_dir, count, FR_DERIVED_MAX_FILES);
        return FR_ERR;
    }

    for (size_t index = 0; index < count; index++) {
        if (fr_lua_sandbox_climbs_out(files[index].path)) {
            fr_error_set(err, "generated path \"%s\" cannot escape \"%s\"",
                        files[index].path, derived_dir);
            return FR_ERR;
        }
    }

    ledger_entry previous[FR_DERIVED_MAX_FILES];
    size_t previous_count = 0;
    if (ledger_read(derived_dir, previous, FR_DERIVED_MAX_FILES, &previous_count, err) != FR_OK) {
        return FR_ERR;
    }

    if (count == 0 && previous_count == 0) return FR_OK;

    ledger_entry next[FR_DERIVED_MAX_FILES];
    size_t next_count = 0;
    int result = FR_OK;

    for (size_t index = 0; index < count && result == FR_OK; index++) {
        char full[1024];
        if (derived_join(full, sizeof full, derived_dir, files[index].path, err) != FR_OK) {
            result = FR_ERR;
            break;
        }

        char digest[65];
        fr_sha256_hex(files[index].text, strlen(files[index].text), digest);

        char *existing = NULL;
        fr_error read_err;
        int exists = fr_file_read_text(full, &existing, &read_err) == FR_OK;
        int unchanged = exists && strcmp(existing, files[index].text) == 0;

        if (exists && !unchanged) {
            const ledger_entry *recorded = entry_for(previous, previous_count, files[index].path);
            char existing_digest[65];
            fr_sha256_hex(existing, strlen(existing), existing_digest);
            if (recorded == NULL || strcmp(recorded->digest, existing_digest) != 0) {
                notice("\"%s\" was changed outside daukle and has been regenerated", full);
            }
        }
        free(existing);

        if (!unchanged) {
            if (write && fr_cache_write_atomic(full, files[index].text, strlen(files[index].text)) != FR_OK) {
                fr_error_set(err, "could not write \"%s\"", full);
                result = FR_ERR;
                break;
            }
            if (report_add(report, full, err) != FR_OK) {
                result = FR_ERR;
                break;
            }
        }

        next[next_count].path = fr_dup_string(files[index].path);
        if (next[next_count].path == NULL) {
            fr_error_set(err, "out of memory recording \"%s\"", files[index].path);
            result = FR_ERR;
            break;
        }
        memcpy(next[next_count].digest, digest, 65);
        next_count++;
    }

    for (size_t index = 0; index < previous_count && result == FR_OK; index++) {
        if (generated_this_run(files, count, previous[index].path)) continue;

        char full[1024];
        if (derived_join(full, sizeof full, derived_dir, previous[index].path, err) != FR_OK) {
            result = FR_ERR;
            break;
        }

        char *existing = NULL;
        fr_error read_err;
        if (fr_file_read_text(full, &existing, &read_err) != FR_OK) continue;

        char existing_digest[65];
        fr_sha256_hex(existing, strlen(existing), existing_digest);
        free(existing);

        if (strcmp(existing_digest, previous[index].digest) != 0) {
            notice("\"%s\" is no longer generated but was changed outside daukle, so it is kept",
                   full);
            continue;
        }
        if (write) remove(full);
        if (report_add(report, full, err) != FR_OK) result = FR_ERR;
    }

    if (result == FR_OK && write) {
        if (ledger_write(derived_dir, next, next_count, err) != FR_OK) result = FR_ERR;
    }

    for (size_t index = 0; index < next_count; index++) free(next[index].path);
    for (size_t index = 0; index < previous_count; index++) free(previous[index].path);
    if (result != FR_OK) fr_derived_report_free(report);
    return result;
}

void fr_derived_report_free(fr_derived_report *report) {
    if (report == NULL) return;
    for (size_t index = 0; index < report->count; index++) free(report->paths[index]);
    free(report->paths);
    report->paths = NULL;
    report->count = 0;
}

void fr_derived_free_files(fr_generated_file *files, size_t count) {
    if (files == NULL) return;
    for (size_t index = 0; index < count; index++) {
        free(files[index].path);
        free(files[index].text);
    }
    free(files);
}

int fr_derived_root(const char *manifest_dir, char **out_dir, fr_error *err) {
    *out_dir = NULL;
    const char *base = (manifest_dir == NULL || manifest_dir[0] == '\0') ? "." : manifest_dir;

    size_t length = strlen(base) + strlen("/build/daukle") + 1;
    char *path = malloc(length);
    if (path == NULL) {
        fr_error_set(err, "out of memory building the derived root");
        return FR_ERR;
    }
    snprintf(path, length, "%s/build/daukle", base);

    *out_dir = path;
    return FR_OK;
}

int fr_derived_dir(const char *manifest_dir, const char *toolchain, char **out_dir, fr_error *err) {
    *out_dir = NULL;
    if (toolchain[0] == '\0' || fr_lua_sandbox_climbs_out(toolchain)
        || strchr(toolchain, '/') != NULL || strchr(toolchain, '\\') != NULL) {
        fr_error_set(err, "toolchain name \"%s\" cannot be a directory name", toolchain);
        return FR_ERR;
    }

    char *root = NULL;
    if (fr_derived_root(manifest_dir, &root, err) != FR_OK) return FR_ERR;

    size_t length = strlen(root) + 1 + strlen(toolchain) + 1;
    char *path = malloc(length);
    if (path == NULL) {
        free(root);
        fr_error_set(err, "out of memory building the derived directory for \"%s\"", toolchain);
        return FR_ERR;
    }
    snprintf(path, length, "%s/%s", root, toolchain);
    free(root);

    *out_dir = path;
    return FR_OK;
}
