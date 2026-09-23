#include "resolvers.h"

#include "cJSON.h"
#include "error.h"
#include "jsonx.h"
#include "plugins.h"

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
