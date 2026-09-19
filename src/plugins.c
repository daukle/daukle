#include "plugins.h"

#include "cJSON.h"
#include "error.h"
#include "jsonx.h"

#include <stdlib.h>
#include <string.h>

static char *dup_string(const char *text) {
    if (text == NULL) return NULL;
    size_t length = strlen(text) + 1;
    char *copy = malloc(length);
    if (copy != NULL) memcpy(copy, text, length);
    return copy;
}

static char *dup_prefix(const char *text, size_t length) {
    char *copy = malloc(length + 1);
    if (copy == NULL) return NULL;
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

/* A bare "owner/name" without "@range" is rejected rather than guessed at,
   so a manifest typo surfaces as a diagnostic instead of an unpinned fetch. */
static int parse_string_form(const char *label, const char *value, fr_plugin_entry *out,
                             fr_error *err) {
    if (value[0] == '.' || value[0] == '/') {
        out->kind = FR_PLUGIN_LOCAL;
        out->path = dup_string(value);
        return out->path != NULL ? FR_OK : FR_ERR;
    }

    const char *at = strchr(value, '@');
    if (at == NULL) {
        fr_error_set(err, "plugin \"%s\": \"%s\" needs a version, as \"owner/name@range\","
                          " or a path beginning with \".\" or \"/\"", label, value);
        return FR_ERR;
    }

    size_t repo_length = (size_t) (at - value);
    if (repo_length == 0 || at[1] == '\0') {
        fr_error_set(err, "plugin \"%s\": \"%s\" needs both a repo and a version around \"@\"",
                          label, value);
        return FR_ERR;
    }

    out->kind = FR_PLUGIN_REMOTE;
    out->repo = dup_prefix(value, repo_length);
    out->version = dup_string(at + 1);
    return (out->repo != NULL && out->version != NULL) ? FR_OK : FR_ERR;
}

static int parse_table_form(const char *label, const cJSON *member, fr_plugin_entry *out,
                            fr_error *err) {
    const char *repo = NULL;
    const char *version = NULL;
    if (fr_json_string(member, "repo", label, &repo, err) != FR_OK) return FR_ERR;
    if (fr_json_string(member, "version", label, &version, err) != FR_OK) return FR_ERR;

    const char *sha256 = NULL;
    if (cJSON_GetObjectItemCaseSensitive(member, "sha256") != NULL) {
        if (fr_json_string(member, "sha256", label, &sha256, err) != FR_OK) return FR_ERR;
    }

    out->kind = FR_PLUGIN_REMOTE;
    out->repo = dup_string(repo);
    out->version = dup_string(version);
    out->sha256 = sha256 != NULL ? dup_string(sha256) : NULL;
    if (out->repo == NULL || out->version == NULL || (sha256 != NULL && out->sha256 == NULL)) {
        return FR_ERR;
    }
    return FR_OK;
}

int fr_plugins_parse(const struct cJSON *document, fr_plugin_entry **out, size_t *out_count,
                     fr_error *err) {
    *out = NULL;
    *out_count = 0;

    const cJSON *plugins = cJSON_GetObjectItemCaseSensitive(document, "plugins");
    if (plugins == NULL) return FR_OK;

    if (!cJSON_IsObject(plugins)) {
        fr_error_set(err, "plugins must be a table");
        return FR_ERR;
    }

    int size = cJSON_GetArraySize(plugins);
    if (size == 0) return FR_OK;

    fr_plugin_entry *entries = calloc((size_t) size, sizeof *entries);
    if (entries == NULL) {
        fr_error_set(err, "out of memory reading plugins");
        return FR_ERR;
    }

    size_t count = 0;
    const cJSON *member = NULL;
    cJSON_ArrayForEach(member, plugins) {
        const char *label = member->string;
        count = count + 1;

        fr_plugin_entry *slot = &entries[count - 1];
        slot->label = dup_string(label);
        if (slot->label == NULL) {
            fr_error_set(err, "out of memory reading plugin \"%s\"", label);
            fr_plugins_free(entries, count);
            return FR_ERR;
        }

        int status;
        if (cJSON_IsString(member) && member->valuestring != NULL) {
            status = parse_string_form(label, member->valuestring, slot, err);
        } else if (cJSON_IsObject(member)) {
            status = parse_table_form(label, member, slot, err);
        } else {
            fr_error_set(err, "plugin \"%s\" must be a string or a table", label);
            status = FR_ERR;
        }

        if (status != FR_OK) {
            fr_plugins_free(entries, count);
            return FR_ERR;
        }
    }

    *out = entries;
    *out_count = count;
    return FR_OK;
}

void fr_plugins_free(fr_plugin_entry *entries, size_t count) {
    if (entries == NULL) return;
    for (size_t index = 0; index < count; index++) {
        free(entries[index].label);
        free(entries[index].path);
        free(entries[index].repo);
        free(entries[index].version);
        free(entries[index].sha256);
    }
    free(entries);
}
