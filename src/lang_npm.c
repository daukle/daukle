#include "lang_npm.h"

#include "error.h"
#include "jsonedit.h"
#include "jsonx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *LEDGER_PATH = "tiestone.managed";

typedef struct {
    const char *package;
    const char *range;
} npm_entry;

static int compare_strings(const void *left, const void *right) {
    return strcmp(*(const char *const *) left, *(const char *const *) right);
}

static int read_entries(const fr_resolved *resolved, size_t count, npm_entry **out, fr_error *err) {
    *out = NULL;
    if (count == 0) return FR_OK;

    npm_entry *entries = malloc(count * sizeof *entries);
    if (entries == NULL) {
        fr_error_set(err, "out of memory reading %zu npm blocks", count);
        return FR_ERR;
    }
    for (size_t index = 0; index < count; index++) {
        char path[256];
        snprintf(path, sizeof path, "modules.%s.npm", resolved[index].module);
        if (fr_json_string(resolved[index].block, "package", path, &entries[index].package, err) != FR_OK
            || fr_json_string(resolved[index].block, "range", path, &entries[index].range, err) != FR_OK) {
            free(entries);
            return FR_ERR;
        }
    }
    *out = entries;
    return FR_OK;
}

/* The ledger is read from the document by parsing a copy, because reading needs
   no formatting fidelity and writing needs nothing else. */
static int read_ledger(const char *original_text, const char *configuration,
                       char ***out, size_t *count, fr_error *err) {
    *out = NULL;
    *count = 0;

    cJSON *document = cJSON_Parse(original_text);
    if (document == NULL) {
        fr_error_set(err, "is not valid json");
        return FR_ERR;
    }

    const cJSON *tiestone = cJSON_GetObjectItemCaseSensitive(document, "tiestone");
    const cJSON *managed = tiestone != NULL ? cJSON_GetObjectItemCaseSensitive(tiestone, "managed") : NULL;
    const cJSON *owned = managed != NULL ? cJSON_GetObjectItemCaseSensitive(managed, configuration) : NULL;
    if (!cJSON_IsArray(owned)) {
        cJSON_Delete(document);
        return FR_OK;
    }

    int status = fr_json_array_of_strings(managed, configuration, LEDGER_PATH, out, count, err);
    cJSON_Delete(document);
    return status;
}

static char *duplicate(const char *text) {
    size_t length = strlen(text) + 1;
    char *copy = malloc(length);
    if (copy != NULL) memcpy(copy, text, length);
    return copy;
}

static int contains(const npm_entry *entries, size_t count, const char *package) {
    for (size_t index = 0; index < count; index++) {
        if (strcmp(entries[index].package, package) == 0) return 1;
    }
    return 0;
}

static int replace_text(char **text, char *next, int status) {
    if (status != FR_OK) return FR_ERR;
    free(*text);
    *text = next;
    return FR_OK;
}

static int write_dependencies(char **text, const npm_entry *entries, size_t count,
                              const char *configuration, fr_error *err) {
    for (size_t index = 0; index < count; index++) {
        char *next = NULL;
        int status = fr_json_edit_set_string(*text, configuration, entries[index].package,
                                             entries[index].range, &next, err);
        if (replace_text(text, next, status) != FR_OK) return FR_ERR;
    }
    return FR_OK;
}

static int remove_dropped(char **text, char *const *ledger, size_t ledger_count,
                          const npm_entry *entries, size_t count,
                          const char *configuration, fr_error *err) {
    for (size_t index = 0; index < ledger_count; index++) {
        if (contains(entries, count, ledger[index])) continue;
        char *next = NULL;
        int status = fr_json_edit_remove(*text, configuration, ledger[index], &next, err);
        if (replace_text(text, next, status) != FR_OK) return FR_ERR;
    }
    return FR_OK;
}

static int write_ledger(char **text, const npm_entry *entries, size_t count,
                        const char *configuration, fr_error *err) {
    const char **packages = NULL;
    if (count > 0) {
        packages = malloc(count * sizeof *packages);
        if (packages == NULL) {
            fr_error_set(err, "out of memory recording %zu owned packages", count);
            return FR_ERR;
        }
        for (size_t index = 0; index < count; index++) packages[index] = entries[index].package;
        qsort(packages, count, sizeof *packages, compare_strings);
    }

    char *next = NULL;
    int status = fr_json_edit_set_string_array(*text, LEDGER_PATH, configuration,
                                               packages, count, &next, err);
    free(packages);
    return replace_text(text, next, status);
}

static int npm_apply(void *state, const fr_consumer *consumer, const fr_resolved *resolved,
                     size_t count, const char *original_text, char **out_text, fr_error *err) {
    (void) state;
    *out_text = NULL;

    if (consumer->configuration == NULL || consumer->configuration[0] == '\0') {
        fr_error_set(err, "consumer has no \"configuration\" for the npm language plugin");
        return FR_ERR;
    }
    if (strchr(consumer->configuration, '.') != NULL) {
        fr_error_set(err, "npm \"configuration\" \"%s\" must name one member, not a path",
                     consumer->configuration);
        return FR_ERR;
    }

    npm_entry *entries = NULL;
    if (read_entries(resolved, count, &entries, err) != FR_OK) return FR_ERR;

    char **ledger = NULL;
    size_t ledger_count = 0;
    if (read_ledger(original_text, consumer->configuration, &ledger, &ledger_count, err) != FR_OK) {
        free(entries);
        return FR_ERR;
    }

    char *text = duplicate(original_text);
    if (text == NULL) {
        fr_error_set(err, "out of memory copying the document");
        fr_string_array_free(ledger, ledger_count);
        free(entries);
        return FR_ERR;
    }

    int status = FR_OK;
    if (count > 0 || ledger_count > 0) {
        status = write_dependencies(&text, entries, count, consumer->configuration, err);
        if (status == FR_OK) {
            status = remove_dropped(&text, ledger, ledger_count, entries, count,
                                    consumer->configuration, err);
        }
        if (status == FR_OK) {
            status = write_ledger(&text, entries, count, consumer->configuration, err);
        }
    }

    fr_string_array_free(ledger, ledger_count);
    free(entries);
    if (status != FR_OK) {
        free(text);
        return FR_ERR;
    }
    *out_text = text;
    return FR_OK;
}

const fr_language_plugin FR_LANGUAGE_NPM = { "tiestone.language/npm", npm_apply, NULL };
