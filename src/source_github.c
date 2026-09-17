#include "source_github.h"

#include "cache.h"
#include "error.h"
#include "http.h"
#include "jsonx.h"
#include "manifest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int optional_string(const cJSON *block, const char *key, const char *path,
                           const char *fallback, const char **out, fr_error *err) {
    if (cJSON_GetObjectItemCaseSensitive(block, key) == NULL) {
        *out = fallback;
        return FR_OK;
    }
    return fr_json_string(block, key, path, out, err);
}

/* Base capacity covers a no-growth substitution; each match then adds only the amount version_len exceeds placeholder_len, the exact per-match growth. */
static char *substitute_version(const char *tag_template, const char *version, fr_error *err) {
    const char *placeholder = "{version}";
    size_t placeholder_len = strlen(placeholder);
    size_t version_len = strlen(version);
    size_t extra_per_match = version_len > placeholder_len ? version_len - placeholder_len : 0;

    size_t occurrences = 0;
    for (const char *cursor = tag_template; (cursor = strstr(cursor, placeholder)) != NULL; cursor += placeholder_len) {
        occurrences++;
    }

    char *result = malloc(strlen(tag_template) + 1 + occurrences * extra_per_match);
    if (result == NULL) {
        fr_error_set(err, "out of memory substituting version into tag");
        return NULL;
    }

    char *write = result;
    const char *read = tag_template;
    const char *match;
    while ((match = strstr(read, placeholder)) != NULL) {
        size_t prefix_len = (size_t) (match - read);
        memcpy(write, read, prefix_len);
        write += prefix_len;
        memcpy(write, version, version_len);
        write += version_len;
        read = match + placeholder_len;
    }
    strcpy(write, read);
    return result;
}

static char *build_release_url(const char *repo, const char *tag, const char *asset) {
    size_t length = strlen("https://github.com/") + strlen(repo) + strlen("/releases/download/")
                   + strlen(tag) + 1 + strlen(asset) + 1;
    char *url = malloc(length);
    if (url != NULL) {
        snprintf(url, length, "https://github.com/%s/releases/download/%s/%s", repo, tag, asset);
    }
    return url;
}

static char *build_auth_header_value(void) {
    const char *token = getenv("DAUKLE_TOKEN");
    if (token == NULL || token[0] == '\0') token = getenv("GITHUB_TOKEN");
    if (token == NULL || token[0] == '\0') return NULL;

    size_t length = strlen("Bearer ") + strlen(token) + 1;
    char *value = malloc(length);
    if (value != NULL) snprintf(value, length, "Bearer %s", token);
    return value;
}

/* out_from_network tells the caller whether *out_text came from the network
   (so it is only worth caching once it parses) rather than from an existing
   cache hit (already validated when it was written). */
static int fetch_manifest_text(const char *project, const char *version, const char *url,
                               char **out_text, size_t *out_length, int *out_from_network,
                               fr_error *err) {
    *out_from_network = 0;
    *out_length = 0;
    if (fr_cache_read(project, version, url, out_text, err) != FR_OK) return FR_ERR;
    if (*out_text != NULL) {
        *out_length = strlen(*out_text);
        return FR_OK;
    }

    char *auth_value = build_auth_header_value();
    fr_http_header header;
    const fr_http_header *headers = NULL;
    size_t header_count = 0;
    if (auth_value != NULL) {
        header.name = "Authorization";
        header.value = auth_value;
        headers = &header;
        header_count = 1;
    }

    char *body = NULL;
    size_t length = 0;
    int result = fr_http_get(url, headers, header_count, &body, &length, err);
    free(auth_value);
    if (result != FR_OK) return FR_ERR;

    char *text = malloc(length + 1);
    if (text == NULL) {
        free(body);
        fr_error_set(err, "out of memory reading response body");
        return FR_ERR;
    }
    memcpy(text, body, length);
    text[length] = '\0';
    free(body);

    *out_text = text;
    *out_length = length;
    *out_from_network = 1;
    return FR_OK;
}

static int source_github_load(void *state, const char *project, const cJSON *block,
                              const char *base_dir, fr_project *out, fr_error *err) {
    (void) state;
    (void) base_dir;
    memset(out, 0, sizeof *out);

    char path[256];
    snprintf(path, sizeof path, "sources.%s", project);

    const char *repo = NULL;
    if (fr_json_string(block, "repo", path, &repo, err) != FR_OK) return FR_ERR;

    const char *version = NULL;
    if (fr_json_string(block, "version", path, &version, err) != FR_OK) return FR_ERR;

    const char *asset = NULL;
    if (optional_string(block, "asset", path, "daukle.json", &asset, err) != FR_OK) return FR_ERR;

    const char *tag_template = NULL;
    if (optional_string(block, "tag", path, "{version}", &tag_template, err) != FR_OK) return FR_ERR;

    char *tag = substitute_version(tag_template, version, err);
    if (tag == NULL) return FR_ERR;

    char *url = build_release_url(repo, tag, asset);
    free(tag);
    if (url == NULL) {
        fr_error_set(err, "out of memory building url for \"%s\"", project);
        return FR_ERR;
    }

    char *text = NULL;
    size_t text_length = 0;
    int from_network = 0;
    if (fetch_manifest_text(project, version, url, &text, &text_length, &from_network, err) != FR_OK) {
        free(url);
        return FR_ERR;
    }

    int result = fr_project_parse(text, url, out, err);
    if (result == FR_OK && from_network) {
        fr_cache_write(project, version, url, text, text_length);
    }
    free(text);
    free(url);
    return result;
}

const fr_source_plugin FR_SOURCE_GITHUB = { "daukle.source/github-releases", source_github_load, NULL };
