#include "plugin/plugins_internal.h"

#include "plugin/plugins.h"
#include "util/error.h"

#include <stdlib.h>
#include <string.h>

static fr_plugin_report_entry *report_entries;
static size_t report_count;

static void free_report_entry(fr_plugin_report_entry *entry) {
    free(entry->label);
    free(entry->resolver);
    free(entry->url);
    free(entry->resolved);
    free(entry->required_by);
    free(entry->alias);
    for (size_t index = 0; index < entry->uses_count; index++) free(entry->uses[index]);
    free(entry->uses);
}

static char **report_unused_resolvers;
static size_t report_unused_resolver_count;

void fr_plugins_report_clear(void) {
    for (size_t index = 0; index < report_count; index++) free_report_entry(&report_entries[index]);
    free(report_entries);
    report_entries = NULL;
    report_count = 0;

    for (size_t index = 0; index < report_unused_resolver_count; index++) {
        free(report_unused_resolvers[index]);
    }
    free(report_unused_resolvers);
    report_unused_resolvers = NULL;
    report_unused_resolver_count = 0;
}

const fr_plugin_report *fr_plugins_report(void) {
    static fr_plugin_report view;
    view.entries = report_entries;
    view.count = report_count;
    view.unused_resolvers = report_unused_resolvers;
    view.unused_resolver_count = report_unused_resolver_count;
    return &view;
}

size_t fr_plugins_report_declared_count(const fr_plugin_report *report) {
    size_t count = 0;
    for (size_t index = 0; index < report->count; index++) {
        if (report->entries[index].required_by == NULL) count++;
    }
    return count;
}

int fr_plugins_report_append(const fr_plugin_entry *entry, const char *origin,
                             const char *resolved, const char *digest,
                             const fr_plugin_declaration *declaration, fr_error *err) {
    fr_plugin_report_entry *grown = realloc(report_entries, (report_count + 1) * sizeof *grown);
    if (grown == NULL) {
        fr_error_set(err, "out of memory recording plugin \"%s\" in the report", entry->label);
        return FR_ERR;
    }
    report_entries = grown;

    fr_plugin_report_entry *slot = &report_entries[report_count];
    memset(slot, 0, sizeof *slot);
    slot->kind = entry->kind;
    memcpy(slot->sha256, digest, sizeof slot->sha256);

    slot->label = fr_dup_string(entry->label);
    slot->resolved = fr_dup_string(resolved != NULL ? resolved : origin);
    slot->url = entry->kind != FR_PLUGIN_PATH ? fr_dup_string(origin) : NULL;
    slot->resolver = fr_dup_string(entry->resolver);
    slot->uses = declaration->uses_count > 0
                     ? malloc(declaration->uses_count * sizeof *slot->uses)
                     : NULL;
    if (slot->label == NULL || slot->resolved == NULL
        || (entry->kind != FR_PLUGIN_PATH && slot->url == NULL)
        || (entry->resolver != NULL && slot->resolver == NULL)
        || (declaration->uses_count > 0 && slot->uses == NULL)) {
        free_report_entry(slot);
        fr_error_set(err, "out of memory recording plugin \"%s\" in the report", entry->label);
        return FR_ERR;
    }

    for (size_t index = 0; index < declaration->uses_count; index++) {
        slot->uses[index] = fr_dup_string(declaration->uses[index]);
        if (slot->uses[index] == NULL) {
            slot->uses_count = index;
            free_report_entry(slot);
            fr_error_set(err, "out of memory recording plugin \"%s\" in the report", entry->label);
            return FR_ERR;
        }
    }
    slot->uses_count = declaration->uses_count;

    report_count++;
    return FR_OK;
}

/* Appends one dependency's row, owning copies of everything row borrows: row's strings point into
   the deps graph, which fr_plugin_deps_close (load_one's own cleanup) tears down before the report
   is ever read. sha256 is row->digest, the value computed while acquiring the node, never
   recomputed here, so the printed digest is provably the bytes that were verified. */
static int append_dependency_report_entry(const fr_plugin_deps_row *row, fr_error *err) {
    fr_plugin_report_entry *grown = realloc(report_entries, (report_count + 1) * sizeof *grown);
    if (grown == NULL) {
        fr_error_set(err, "out of memory recording dependency \"%s\" in the report", row->alias);
        return FR_ERR;
    }
    report_entries = grown;

    fr_plugin_report_entry *slot = &report_entries[report_count];
    memset(slot, 0, sizeof *slot);
    slot->kind = row->kind;
    memcpy(slot->sha256, row->digest, sizeof slot->sha256);
    slot->overridden = row->overridden;

    slot->label = fr_dup_string(row->alias);
    slot->alias = fr_dup_string(row->alias);
    slot->required_by = fr_dup_string(row->required_by);
    slot->url = fr_dup_string(row->url);
    slot->uses = row->uses_count > 0 ? malloc(row->uses_count * sizeof *slot->uses) : NULL;
    if (slot->label == NULL || slot->alias == NULL || slot->required_by == NULL || slot->url == NULL
        || (row->uses_count > 0 && slot->uses == NULL)) {
        free_report_entry(slot);
        fr_error_set(err, "out of memory recording dependency \"%s\" in the report", row->alias);
        return FR_ERR;
    }

    for (size_t index = 0; index < row->uses_count; index++) {
        slot->uses[index] = fr_dup_string(row->uses[index]);
        if (slot->uses[index] == NULL) {
            slot->uses_count = index;
            free_report_entry(slot);
            fr_error_set(err, "out of memory recording dependency \"%s\" in the report", row->alias);
            return FR_ERR;
        }
    }
    slot->uses_count = row->uses_count;

    report_count++;
    return FR_OK;
}

/* fr_plugin_deps_acquire walks a requires table with lua_next, whose iteration order is
   unspecified, so the order nodes were acquired in varies between runs and machines for one
   unchanged manifest. "daukle config print" is what a person diffs across both, so rows are
   sorted here, before anything is appended, rather than left in acquisition order. */
static int compare_deps_rows(const void *left, const void *right) {
    const fr_plugin_deps_row *a = left;
    const fr_plugin_deps_row *b = right;
    int by_required_by = strcmp(a->required_by, b->required_by);
    return by_required_by != 0 ? by_required_by : strcmp(a->alias, b->alias);
}

int fr_plugins_report_append_dependencies(fr_plugin_deps *deps, fr_error *err) {
    size_t count = fr_plugin_deps_row_count(deps);
    if (count == 0) return FR_OK;

    /* One row per binding rather than one per node, so this can exceed
       FR_PLUGIN_DEPS_MAX_NODES when a url is bound under more than one alias:
       heap-allocated because the worst case (every node's own requires table
       full) is larger than a stack buffer should carry. */
    fr_plugin_deps_row *rows = malloc(count * sizeof *rows);
    if (rows == NULL) {
        fr_error_set(err, "out of memory recording dependencies in the report");
        return FR_ERR;
    }
    for (size_t index = 0; index < count; index++) fr_plugin_deps_row_at(deps, index, &rows[index]);
    qsort(rows, count, sizeof *rows, compare_deps_rows);

    int status = FR_OK;
    for (size_t index = 0; index < count && status == FR_OK; index++) {
        status = append_dependency_report_entry(&rows[index], err);
    }
    free(rows);
    return status;
}

int fr_plugins_report_record_unused_resolvers(const fr_plugin_entry *entries, size_t entry_count,
                                              const fr_resolver_entry *resolvers,
                                              size_t resolver_count, fr_error *err) {
    if (resolver_count == 0) return FR_OK;

    char **unused = malloc(resolver_count * sizeof *unused);
    if (unused == NULL) {
        fr_error_set(err, "out of memory recording unused resolvers");
        return FR_ERR;
    }

    size_t unused_count = 0;
    for (size_t index = 0; index < resolver_count; index++) {
        const char *label = resolvers[index].label;
        int used = 0;
        for (size_t entry_index = 0; entry_index < entry_count && !used; entry_index++) {
            used = entries[entry_index].kind == FR_PLUGIN_RESOLVED
                && strcmp(entries[entry_index].resolver, label) == 0;
        }
        if (used) continue;

        char *copy = fr_dup_string(label);
        if (copy == NULL) {
            for (size_t free_index = 0; free_index < unused_count; free_index++) free(unused[free_index]);
            free(unused);
            fr_error_set(err, "out of memory recording unused resolvers");
            return FR_ERR;
        }
        unused[unused_count] = copy;
        unused_count++;
    }

    if (unused_count == 0) {
        free(unused);
        return FR_OK;
    }

    report_unused_resolvers = unused;
    report_unused_resolver_count = unused_count;
    return FR_OK;
}
