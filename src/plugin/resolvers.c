#include "plugin/resolvers.h"

#include "cJSON.h"
#include "config/config_lua.h"
#include "config/jsonx.h"
#include "lua/lua_sandbox.h"
#include "plugin/plugin_fetch.h"
#include "plugin/plugins.h"
#include "project/region.h"
#include "util/error.h"
#include "util/sha256.h"

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

static char *loaded_label;
static char *loaded_source;

void fr_resolvers_clear(void) {
    free(loaded_label);
    loaded_label = NULL;
    free(loaded_source);
    loaded_source = NULL;
}

static const char *entry_source(const fr_resolver_entry *entry) {
    return entry->url != NULL ? entry->url : entry->path;
}

/* The memo skips acquisition entirely, so it answers "is the callback in the
   runtime this entry's own?" rather than "has some entry by this name been
   loaded?": a label alone would let two entries sharing one name, across two
   manifests read in one process, resolve through each other's chunk. */
static int is_the_loaded_resolver(const fr_resolver_entry *entry) {
    return loaded_label != NULL && strcmp(loaded_label, entry->label) == 0
        && loaded_source != NULL && strcmp(loaded_source, entry_source(entry)) == 0;
}

static int remember_loaded(const fr_resolver_entry *entry, fr_error *err) {
    char *label = fr_dup_string(entry->label);
    char *source = fr_dup_string(entry_source(entry));
    if (label == NULL || source == NULL) {
        free(label);
        free(source);
        fr_error_set(err, "out of memory recording resolver \"%s\" as loaded", entry->label);
        return FR_ERR;
    }
    fr_resolvers_clear();
    loaded_label = label;
    loaded_source = source;
    return FR_OK;
}

/* Reads entry's chunk (local path or pinned url), verifies its digest before
   running it, then loads it and confirms it declared a resolver. Mirrors
   load_one in plugins.c step for step, for the same reason: verifying the
   digest after the chunk ran would mean the mismatched code already executed. */
static int acquire(const fr_resolver_entry *entry, fr_error *err) {
    lua_State *state = fr_lua_runtime_state();

    char *path = NULL;
    char *text = NULL;
    size_t length = 0;
    if (entry->path != NULL) {
        if (fr_lua_sandbox_resolve(state, entry->path, &path, err) != FR_OK) return FR_ERR;
        if (fr_file_read_bytes(path, &text, &length, err) != FR_OK) {
            free(path);
            return FR_ERR;
        }
    } else {
        if (fr_plugin_fetch(entry->url, NULL, 0, &text, &length, err) != FR_OK) return FR_ERR;
    }

    char digest[65];
    fr_sha256_hex(text, length, digest);
    if (entry->sha256 != NULL && !fr_sha256_hex_equal(digest, entry->sha256)) {
        fr_error_set(err, "resolver \"%s\": expected sha256 %s but the file is %s",
                    entry->label, entry->sha256, digest);
        if (entry->url != NULL) fr_plugin_fetch_discard(entry->url);
        free(text);
        free(path);
        return FR_ERR;
    }

    const char *origin = entry->path != NULL ? path : entry->url;

    /* A resolver is acquired the way a plugin is, so it may be an archive too and
       nothing here is written twice to say so. */
    fr_plugin_source *source = NULL;
    if (fr_plugin_source_open_bytes(text, length, &source, err) != FR_OK) {
        free(text);
        free(path);
        return FR_ERR;
    }

    const char *chunk = NULL;
    size_t chunk_length = 0;
    int status = fr_plugin_source_entry(source, &chunk, &chunk_length, err);

    fr_plugin_declaration declaration;
    memset(&declaration, 0, sizeof declaration);
    if (status == FR_OK) {
        status = fr_plugins_read_declaration(chunk, chunk_length, origin, "resolver", entry->label,
                                             &declaration, err);
    }
    if (status == FR_OK) {
        fr_lua_set_acquiring_resolver(1);
        status = fr_lua_plugin_load(chunk, chunk_length, origin,
                                    (const char *const *) declaration.uses, declaration.uses_count,
                                    source, NULL, err);
        fr_lua_set_acquiring_resolver(0);
    }
    fr_plugins_free_declaration(&declaration);
    fr_plugin_source_close(source);
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
                     char **out_resolved, fr_http_headers *out_headers, fr_error *err) {
    out_headers->count = 0;
    if (entry == NULL) {
        fr_error_set(err, "no resolver to use");
        return FR_ERR;
    }

    if (!is_the_loaded_resolver(entry)) {
        if (acquire(entry, err) != FR_OK) return FR_ERR;
        if (remember_loaded(entry, err) != FR_OK) return FR_ERR;
    }

    if (fr_lua_resolver_call(coordinate, entry->block, out_url, out_resolved, out_headers, err)
        == FR_OK) {
        return FR_OK;
    }

    char reason[sizeof err->message];
    snprintf(reason, sizeof reason, "%s", err->message);
    fr_error_set(err, "resolver \"%s\": coordinate \"%s\": %s", entry->label, coordinate, reason);
    return FR_ERR;
}
