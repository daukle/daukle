#include "resolvers.h"

#include "cJSON.h"
#include "config_lua.h"
#include "error.h"
#include "jsonx.h"
#include "lua_sandbox.h"
#include "plugin_fetch.h"
#include "plugins.h"
#include "region.h"
#include "sha256.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A resolver entry names exactly one of "url" (remote) or "path" (local). A
   url entry must also carry a "sha256" pin: unlike a plugin, a resolver is
   what chooses where every other plugin comes from, so an unpinned fetch here
   would let a compromised host redirect the whole acquisition path. */
static int parse_resolver_entry(const char *label, const cJSON *member, fr_resolver_entry *out,
                                fr_error *err) {
    int has_url = cJSON_GetObjectItemCaseSensitive(member, "url") != NULL;
    int has_path = cJSON_GetObjectItemCaseSensitive(member, "path") != NULL;
    if (has_url && has_path) {
        fr_error_set(err, "resolver \"%s\" names both \"url\" and \"path\"; it takes one", label);
        return FR_ERR;
    }
    if (!has_url && !has_path) {
        fr_error_set(err, "resolver \"%s\" names neither \"url\" nor \"path\"", label);
        return FR_ERR;
    }

    if (has_url) {
        const char *url = NULL;
        if (fr_json_string(member, "url", label, &url, err) != FR_OK) return FR_ERR;

        if (cJSON_GetObjectItemCaseSensitive(member, "sha256") == NULL) {
            fr_error_set(err, "resolver \"%s\" must carry a sha256: it is fetched over the network"
                              " and it chooses where every other plugin comes from", label);
            return FR_ERR;
        }
        const char *sha256 = NULL;
        if (fr_json_string(member, "sha256", label, &sha256, err) != FR_OK) return FR_ERR;

        out->url = fr_dup_string(url);
        out->sha256 = fr_dup_string(sha256);
        if (out->url == NULL || out->sha256 == NULL) {
            fr_error_set(err, "out of memory reading resolver \"%s\"", label);
            return FR_ERR;
        }
        return FR_OK;
    }

    const char *path = NULL;
    if (fr_json_string(member, "path", label, &path, err) != FR_OK) return FR_ERR;

    const char *sha256 = NULL;
    if (cJSON_GetObjectItemCaseSensitive(member, "sha256") != NULL) {
        if (fr_json_string(member, "sha256", label, &sha256, err) != FR_OK) return FR_ERR;
    }

    out->path = fr_dup_string(path);
    out->sha256 = sha256 != NULL ? fr_dup_string(sha256) : NULL;
    if (out->path == NULL || (sha256 != NULL && out->sha256 == NULL)) {
        fr_error_set(err, "out of memory reading resolver \"%s\"", label);
        return FR_ERR;
    }
    return FR_OK;
}

int fr_resolvers_parse(const struct cJSON *document, fr_resolver_entry **out, size_t *out_count,
                       fr_error *err) {
    *out = NULL;
    *out_count = 0;

    const cJSON *resolvers = cJSON_GetObjectItemCaseSensitive(document, "resolvers");
    if (resolvers == NULL) return FR_OK;

    if (!cJSON_IsObject(resolvers)) {
        fr_error_set(err, "resolvers must be a table");
        return FR_ERR;
    }

    int size = cJSON_GetArraySize(resolvers);
    if (size == 0) return FR_OK;

    fr_resolver_entry *entries = calloc((size_t) size, sizeof *entries);
    if (entries == NULL) {
        fr_error_set(err, "out of memory reading resolvers");
        return FR_ERR;
    }

    size_t count = 0;
    const cJSON *member = NULL;
    cJSON_ArrayForEach(member, resolvers) {
        const char *label = member->string;
        count = count + 1;

        fr_resolver_entry *slot = &entries[count - 1];
        slot->label = fr_dup_string(label);
        if (slot->label == NULL) {
            fr_error_set(err, "out of memory reading resolver \"%s\"", label);
            fr_resolvers_free(entries, count);
            return FR_ERR;
        }

        int status;
        if (cJSON_IsObject(member)) {
            slot->block = member;
            status = parse_resolver_entry(label, member, slot, err);
        } else {
            fr_error_set(err, "resolver \"%s\" must be a table", label);
            status = FR_ERR;
        }

        if (status != FR_OK) {
            fr_resolvers_free(entries, count);
            return FR_ERR;
        }
    }

    *out = entries;
    *out_count = count;
    return FR_OK;
}

void fr_resolvers_free(fr_resolver_entry *entries, size_t count) {
    if (entries == NULL) return;
    for (size_t index = 0; index < count; index++) {
        free(entries[index].label);
        free(entries[index].url);
        free(entries[index].path);
        free(entries[index].sha256);
    }
    free(entries);
}

const fr_resolver_entry *fr_resolvers_find(const fr_resolver_entry *entries, size_t count,
                                           const char *label) {
    for (size_t index = 0; index < count; index++) {
        if (strcmp(entries[index].label, label) == 0) return &entries[index];
    }
    return NULL;
}

/* Mirrors plugins.c's own digest_matches: both compare a bounded, lowercased
   sha256 hex digest and neither is worth sharing across a header for one
   four-line helper. */
static int digest_matches(const char *actual, const char *pinned) {
    size_t length = strlen(actual);
    if (length != strlen(pinned) || length >= 65) return 0;
    for (size_t index = 0; index < length; index++) {
        if (tolower((unsigned char) actual[index]) != tolower((unsigned char) pinned[index])) {
            return 0;
        }
    }
    return 1;
}

static char *loaded_label;

void fr_resolvers_clear(void) {
    free(loaded_label);
    loaded_label = NULL;
}

/* Reads entry's chunk (local path or pinned url), verifies its digest before
   running it, then loads it and confirms it declared a resolver. Mirrors
   load_one in plugins.c step for step, for the same reason: verifying the
   digest after the chunk ran would mean the mismatched code already executed. */
static int acquire(const fr_resolver_entry *entry, fr_error *err) {
    lua_State *state = fr_lua_runtime_state();

    char *path = NULL;
    char *text = NULL;
    if (entry->path != NULL) {
        if (fr_lua_sandbox_resolve(state, entry->path, &path, err) != FR_OK) return FR_ERR;
        if (fr_file_read_text(path, &text, err) != FR_OK) {
            free(path);
            return FR_ERR;
        }
    } else {
        if (fr_plugin_fetch(entry->url, &text, err) != FR_OK) return FR_ERR;
    }

    char digest[65];
    fr_sha256_hex(text, strlen(text), digest);
    if (entry->sha256 != NULL && !digest_matches(digest, entry->sha256)) {
        fr_error_set(err, "resolver \"%s\": expected sha256 %s but the file is %s",
                    entry->label, entry->sha256, digest);
        if (entry->url != NULL) fr_plugin_fetch_discard(entry->url);
        free(text);
        free(path);
        return FR_ERR;
    }

    const char *origin = entry->path != NULL ? path : entry->url;

    char **uses = NULL;
    size_t uses_count = 0;
    int status = fr_plugins_read_uses(text, origin, "resolver", entry->label, &uses, &uses_count,
                                      err);
    if (status == FR_OK) {
        status = fr_lua_plugin_load(text, origin, (const char *const *) uses, uses_count, err);
    }
    fr_plugins_free_uses(uses, uses_count);
    free(text);
    free(path);
    if (status != FR_OK) return FR_ERR;

    if (!fr_lua_resolver_declared()) {
        fr_error_set(err, "resolver \"%s\" declares no resolver", entry->label);
        return FR_ERR;
    }
    return FR_OK;
}

int fr_resolvers_use(const fr_resolver_entry *entry, const char *coordinate, char **out_url,
                     char **out_resolved, fr_error *err) {
    if (entry == NULL) {
        fr_error_set(err, "no resolver to use");
        return FR_ERR;
    }

    if (loaded_label == NULL || strcmp(loaded_label, entry->label) != 0) {
        if (acquire(entry, err) != FR_OK) return FR_ERR;

        char *label = fr_dup_string(entry->label);
        if (label == NULL) {
            fr_error_set(err, "out of memory recording resolver \"%s\" as loaded", entry->label);
            return FR_ERR;
        }
        free(loaded_label);
        loaded_label = label;
    }

    if (fr_lua_resolver_call(coordinate, entry->block, out_url, out_resolved, err) == FR_OK) {
        return FR_OK;
    }

    char reason[sizeof err->message];
    snprintf(reason, sizeof reason, "%s", err->message);
    fr_error_set(err, "resolver \"%s\": coordinate \"%s\": %s", entry->label, coordinate, reason);
    return FR_ERR;
}
