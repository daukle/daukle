#include "plugin/plugins.h"

#include "plugin/plugins_internal.h"

#include "cJSON.h"
#include "cache/cache.h"
#include "config/config_lua.h"
#include "lua/lua_sandbox.h"
#include "lua/lua_verbs.h"
#include "plugin/plugin_fetch.h"
#include "plugin/plugin_modules.h"
#include "plugin/resolvers.h"
#include "project/region.h"
#include "util/error.h"
#include "util/sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>

char *fr_dup_string(const char *text) {
    if (text == NULL) return NULL;
    size_t length = strlen(text) + 1;
    char *copy = malloc(length);
    if (copy != NULL) memcpy(copy, text, length);
    return copy;
}

char *fr_dup_prefix(const char *text, size_t length) {
    char *copy = malloc(length + 1);
    if (copy == NULL) return NULL;
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

int fr_plugins_out_of_memory(const char *label, fr_error *err) {
    fr_error_set(err, "out of memory reading plugin \"%s\"", label);
    return FR_ERR;
}

/* Keeps only whole labels and marks the cut, rather than ending the one
   diagnostic a reader gets when a coordinate does not resolve mid-label. */
void fr_plugins_describe_declared_resolvers(char *out, size_t out_size,
                                            const fr_resolver_entry *resolvers, size_t count) {
    if (count == 0) {
        snprintf(out, out_size, "none");
        return;
    }

    static const char ELLIPSIS[] = ", ...";
    size_t used = 0;
    out[0] = '\0';
    for (size_t index = 0; index < count; index++) {
        const char *separator = index == 0 ? "" : ", ";
        size_t needed = strlen(separator) + strlen(resolvers[index].label);
        if (used + needed + sizeof ELLIPSIS > out_size) {
            snprintf(out + used, out_size - used, "%s", used == 0 ? "..." : ELLIPSIS);
            return;
        }
        used += (size_t) snprintf(out + used, out_size - used, "%s%s", separator,
                                  resolvers[index].label);
    }
}

static int unknown_resolver(const fr_plugin_entry *entry, const fr_resolver_entry *resolvers,
                            size_t resolver_count, fr_error *err) {
    char declared[160];
    fr_plugins_describe_declared_resolvers(declared, sizeof declared, resolvers, resolver_count);
    fr_error_set(err, "plugin \"%s\": no resolver \"%s\" is declared. Declared resolvers: %s",
                 entry->label, entry->resolver, declared);
    return FR_ERR;
}

/* Acquires entry's chunk and reports where it came from: origin is what the
   digest pin is checked against and what a failed pin discards, resolved is
   the resolver's own answer and stays NULL for the two kinds core names
   itself. */
/* stat and not access: access(path, X_OK) returns 0 for a directory, so it
   cannot tell one from a file at all. */
static int path_is_directory(const char *path) {
#ifdef _WIN32
    struct _stat info;
    return _stat(path, &info) == 0 && (info.st_mode & _S_IFDIR) != 0;
#else
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
#endif
}

static int acquire(const fr_plugin_entry *entry, const fr_resolver_entry *resolvers,
                   size_t resolver_count, char **out_text, size_t *out_length, char **out_origin,
                   char **out_resolved, int *out_is_directory, fr_error *err) {
    *out_text = NULL;
    *out_length = 0;
    *out_origin = NULL;
    *out_resolved = NULL;
    *out_is_directory = 0;

    if (entry->kind == FR_PLUGIN_PATH) {
        lua_State *state = fr_lua_runtime_state();
        /* The directory resolver, although this may name a file: it is the one
           that opens both kinds. fopen refuses a directory on Windows, so
           resolving as a file first would fail before anything could ask which
           kind it is. Containment is the same rule either way. */
        if (fr_lua_sandbox_resolve_dir(state, entry->path, out_origin, err) != FR_OK) {
            return FR_ERR;
        }
        if (path_is_directory(*out_origin)) {
            /* No bytes to hand back: a directory has no single byte string, which
               is also why it cannot be pinned. Its members are read as they are
               asked for. */
            *out_is_directory = 1;
            return FR_OK;
        }
        if (fr_file_read_bytes(*out_origin, out_text, out_length, err) != FR_OK) {
            free(*out_origin);
            *out_origin = NULL;
            return FR_ERR;
        }
        return FR_OK;
    }

    fr_http_headers headers = { { { NULL, NULL } }, 0 };
    if (entry->kind == FR_PLUGIN_URL) {
        *out_origin = fr_dup_string(entry->url);
        if (*out_origin == NULL) return fr_plugins_out_of_memory(entry->label, err);
    } else {
        const fr_resolver_entry *resolver =
            fr_resolvers_find(resolvers, resolver_count, entry->resolver);
        if (resolver == NULL) return unknown_resolver(entry, resolvers, resolver_count, err);
        if (fr_resolvers_use(resolver, entry->coordinate, out_origin, out_resolved, &headers, err)
            != FR_OK) {
            return FR_ERR;
        }
    }

    fr_http_header sent[FR_HTTP_MAX_HEADERS];
    size_t sent_count = fr_http_headers_borrow(&headers, sent);
    int status = fr_plugin_fetch(*out_origin, sent, sent_count, out_text, out_length, err);
    fr_http_headers_free(&headers);
    if (status != FR_OK) {
        free(*out_origin);
        free(*out_resolved);
        *out_origin = NULL;
        *out_resolved = NULL;
        return FR_ERR;
    }
    return FR_OK;
}

int fr_plugins_acquire_source(const fr_plugin_entry *entry, const fr_resolver_entry *resolvers,
                              size_t resolver_count, char **out_text, size_t *out_length,
                              char **out_origin, char **out_resolved, char out_digest[65],
                              fr_plugin_source **out_source, fr_error *err) {
    *out_text = NULL;
    *out_length = 0;
    *out_origin = NULL;
    *out_resolved = NULL;
    out_digest[0] = '\0';
    *out_source = NULL;

    int is_directory = 0;
    if (acquire(entry, resolvers, resolver_count, out_text, out_length, out_origin, out_resolved,
               &is_directory, err)
        != FR_OK) {
        return FR_ERR;
    }

    /* Computed for every plugin that HAS a byte string, pinned or not: an
       unpinned entry's digest is what "daukle config print" shows, so adopting a
       pin is a copy and a paste. Checked before the source is opened, which for
       an archive already reads its central directory: verifying after would
       mean unpinned bytes had already been trusted that far. */
    if (!is_directory) {
        fr_sha256_hex(*out_text, *out_length, out_digest);
        if (entry->sha256 != NULL && !fr_sha256_hex_equal(out_digest, entry->sha256)) {
            fr_error_set(err, "plugin \"%s\": expected sha256 %s but the file is %s",
                        entry->label, entry->sha256, out_digest);
            if (entry->kind != FR_PLUGIN_PATH) fr_plugin_fetch_discard(*out_origin);
            free(*out_text); *out_text = NULL;
            free(*out_origin); *out_origin = NULL;
            free(*out_resolved); *out_resolved = NULL;
            return FR_ERR;
        }
    } else if (entry->sha256 != NULL) {
        /* Refused rather than approximated: a digest over some canonical reading
           of a directory would be neither the published archive's digest nor
           anything a standard tool reproduces, so copying it out of
           "daukle config print" would fail the day the entry became a url. */
        fr_error_set(err, "plugin \"%s\": \"%s\" is a directory and cannot carry a sha256; pin the"
                          " published archive instead",
                    entry->label, entry->path);
        free(*out_text); *out_text = NULL;
        free(*out_origin); *out_origin = NULL;
        free(*out_resolved); *out_resolved = NULL;
        return FR_ERR;
    }

    fr_plugin_source *source = NULL;
    int opened = is_directory
                     ? fr_plugin_source_open_directory(fr_lua_runtime_state(), entry->path, &source,
                                                       err)
                     : fr_plugin_source_open_bytes(*out_text, *out_length, &source, err);
    if (opened != FR_OK) {
        free(*out_text); *out_text = NULL;
        free(*out_origin); *out_origin = NULL;
        free(*out_resolved); *out_resolved = NULL;
        return FR_ERR;
    }

    *out_source = source;
    return FR_OK;
}

int fr_plugins_load(fr_registry *registry, const struct cJSON *document, const char *base_dir,
                    fr_error *err) {
    fr_plugins_report_clear();
    fr_resolvers_clear();

    fr_resolver_entry *resolvers = NULL;
    size_t resolver_count = 0;
    if (fr_resolvers_parse(document, &resolvers, &resolver_count, err) != FR_OK) return FR_ERR;

    fr_plugin_entry *entries = NULL;
    size_t count = 0;
    int status = fr_plugins_parse(document, resolvers, resolver_count, &entries, &count, err);
    if (status == FR_OK) {
        status = fr_plugins_report_record_unused_resolvers(entries, count, resolvers,
                                                           resolver_count, err);
    }
    if (status == FR_OK && count > 0) {
        status = fr_lua_runtime_begin(base_dir, registry, err);
        for (size_t index = 0; index < count && status == FR_OK; index++) {
            status = fr_plugins_load_entry(&entries[index], resolvers, resolver_count, err);
        }
    }

    fr_plugins_free(entries, count);
    fr_resolvers_free(resolvers, resolver_count);
    return status;
}

/* config print reports a module as "<label>/<module>", so that spelling names
   the entry too: the modules share one fetched artifact, and clearing it is
   all an update of one of them can mean. */
static int label_names_entry(const fr_plugin_entry *entry, const char *label) {
    if (strcmp(entry->label, label) == 0) return 1;
    size_t label_length = strlen(entry->label);
    if (strncmp(label, entry->label, label_length) != 0 || label[label_length] != '/') return 0;
    const char *module = label + label_length + 1;
    for (size_t index = 0; index < entry->module_count; index++) {
        if (strcmp(entry->modules[index], module) == 0) return 1;
    }
    return 0;
}

int fr_plugins_update_cache(const fr_plugin_entry *entries, size_t count,
                            const fr_resolver_entry *resolvers, size_t resolver_count,
                            const char *label, size_t *out_removed_count, fr_error *err) {
    *out_removed_count = 0;

    int was_refreshing = fr_cache_refreshing();
    int was_enabled = fr_cache_enabled();
    fr_cache_set_refreshing(1);
    fr_cache_set_enabled(1);

    int status = FR_OK;
    int matched = label == NULL;

    for (size_t index = 0; index < count && status == FR_OK; index++) {
        const fr_plugin_entry *entry = &entries[index];
        if (label != NULL && !label_names_entry(entry, label)) continue;
        matched = 1;

        if (entry->kind == FR_PLUGIN_URL) {
            fr_plugin_fetch_discard(entry->url);
            (*out_removed_count)++;
        } else if (entry->kind == FR_PLUGIN_RESOLVED) {
            const fr_resolver_entry *resolver =
                fr_resolvers_find(resolvers, resolver_count, entry->resolver);
            if (resolver == NULL) {
                status = unknown_resolver(entry, resolvers, resolver_count, err);
            } else {
                char *url = NULL;
                char *resolved = NULL;
                fr_http_headers headers = { { { NULL, NULL } }, 0 };
                status = fr_resolvers_use(resolver, entry->coordinate, &url, &resolved, &headers,
                                          err);
                if (status == FR_OK) {
                    fr_plugin_fetch_discard(url);
                    (*out_removed_count)++;
                }
                fr_http_headers_free(&headers);
                free(url);
                free(resolved);
            }
        }

        if (label != NULL) break;
    }

    fr_cache_set_refreshing(was_refreshing);
    fr_cache_set_enabled(was_enabled);

    if (status != FR_OK) return FR_ERR;
    if (!matched) {
        fr_error_set(err, "no plugin named \"%s\"", label);
        return FR_ERR;
    }
    return FR_OK;
}
