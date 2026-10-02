#include "plugin/plugins.h"

#include "plugin/plugins_internal.h"

#include "cJSON.h"
#include "cache/cache.h"
#include "config/config_lua.h"
#include "config/jsonx.h"
#include "lua/lua_sandbox.h"
#include "lua/lua_verbs.h"
#include "lua/luax.h"
#include "plugin/plugin_fetch.h"
#include "plugin/plugin_modules.h"
#include "plugin/resolvers.h"
#include "project/region.h"
#include "util/error.h"
#include "util/sha256.h"

#include "lauxlib.h"

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

static int out_of_memory(const char *label, fr_error *err) {
    fr_error_set(err, "out of memory reading plugin \"%s\"", label);
    return FR_ERR;
}

/* Keeps only whole labels and marks the cut, rather than ending the one
   diagnostic a reader gets when a coordinate does not resolve mid-label. */
static void describe_declared_resolvers(char *out, size_t out_size,
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

static int no_resolver_named(const char *label, const char *value,
                             const fr_resolver_entry *resolvers, size_t resolver_count,
                             fr_error *err) {
    char declared[160];
    describe_declared_resolvers(declared, sizeof declared, resolvers, resolver_count);
    fr_error_set(err, "plugin \"%s\": \"%s\" names no resolver: write a file path, a url, or"
                      " \"<resolver>:<coordinate>\". Declared resolvers: %s",
                 label, value, declared);
    return FR_ERR;
}

/* "C:/plugins/mine.lua" read as the resolver "C" is the diagnostic this
   prevents, which is why the check comes before the colon split below. */
static int is_drive_path(const char *value) {
    return ((value[0] >= 'A' && value[0] <= 'Z') || (value[0] >= 'a' && value[0] <= 'z'))
        && value[1] == ':' && (value[2] == '/' || value[2] == '\\');
}

static int parse_string_form(const char *label, const char *value,
                             const fr_resolver_entry *resolvers, size_t resolver_count,
                             fr_plugin_entry *out, fr_error *err) {
    if (value[0] == '.' || value[0] == '/' || is_drive_path(value)) {
        out->kind = FR_PLUGIN_PATH;
        out->path = fr_dup_string(value);
        return out->path != NULL ? FR_OK : out_of_memory(label, err);
    }

    if (strncmp(value, "http://", 7) == 0 || strncmp(value, "https://", 8) == 0) {
        out->kind = FR_PLUGIN_URL;
        out->url = fr_dup_string(value);
        return out->url != NULL ? FR_OK : out_of_memory(label, err);
    }

    const char *colon = strchr(value, ':');
    if (colon == NULL || colon == value || colon[1] == '\0') {
        return no_resolver_named(label, value, resolvers, resolver_count, err);
    }

    out->kind = FR_PLUGIN_RESOLVED;
    out->resolver = fr_dup_prefix(value, (size_t) (colon - value));
    out->coordinate = fr_dup_string(colon + 1);
    return (out->resolver != NULL && out->coordinate != NULL) ? FR_OK : out_of_memory(label, err);
}

/* A table entry names exactly one of "path", "url", or "resolver" with
   "coordinate", the three forms the string form discriminates between, and any
   of them may also carry a "sha256" pin. */
static int parse_table_form(const char *label, const cJSON *member, fr_plugin_entry *out,
                            fr_error *err) {
    /* Not validated here: plugin_deps.c is the sole reader, and validating an
       absent table would tax the common case (no requires at all) for
       nothing. */
    out->overrides = cJSON_GetObjectItemCaseSensitive(member, "requires");

    if (cJSON_GetObjectItemCaseSensitive(member, "sha256") != NULL) {
        const char *sha256 = NULL;
        if (fr_json_string(member, "sha256", label, &sha256, err) != FR_OK) return FR_ERR;
        out->sha256 = fr_dup_string(sha256);
        if (out->sha256 == NULL) return out_of_memory(label, err);
    }

    int has_path = cJSON_GetObjectItemCaseSensitive(member, "path") != NULL;
    int has_url = cJSON_GetObjectItemCaseSensitive(member, "url") != NULL;
    int has_resolver = cJSON_GetObjectItemCaseSensitive(member, "resolver") != NULL;
    int has_coordinate = cJSON_GetObjectItemCaseSensitive(member, "coordinate") != NULL;

    int named_groups = has_path + has_url + (has_resolver || has_coordinate);
    if (named_groups > 1) {
        fr_error_set(err, "plugin \"%s\": a table entry names one of path, url, or resolver with"
                          " coordinate, not several", label);
        return FR_ERR;
    }
    if (named_groups == 0) {
        fr_error_set(err, "plugin \"%s\": a table entry needs one of path, url, or resolver with"
                          " coordinate", label);
        return FR_ERR;
    }

    if (has_path) {
        const char *path = NULL;
        if (fr_json_string(member, "path", label, &path, err) != FR_OK) return FR_ERR;

        out->kind = FR_PLUGIN_PATH;
        out->path = fr_dup_string(path);
        return out->path != NULL ? FR_OK : out_of_memory(label, err);
    }

    if (has_url) {
        const char *url = NULL;
        if (fr_json_string(member, "url", label, &url, err) != FR_OK) return FR_ERR;

        out->kind = FR_PLUGIN_URL;
        out->url = fr_dup_string(url);
        return out->url != NULL ? FR_OK : out_of_memory(label, err);
    }

    if (!has_resolver || !has_coordinate) {
        fr_error_set(err, "plugin \"%s\": a resolver entry names both resolver and coordinate;"
                          " \"%s\" is missing", label, has_resolver ? "coordinate" : "resolver");
        return FR_ERR;
    }

    const char *resolver = NULL;
    const char *coordinate = NULL;
    if (fr_json_string(member, "resolver", label, &resolver, err) != FR_OK) return FR_ERR;
    if (fr_json_string(member, "coordinate", label, &coordinate, err) != FR_OK) return FR_ERR;

    out->kind = FR_PLUGIN_RESOLVED;
    out->resolver = fr_dup_string(resolver);
    out->coordinate = fr_dup_string(coordinate);
    return (out->resolver != NULL && out->coordinate != NULL) ? FR_OK : out_of_memory(label, err);
}

int fr_plugins_parse_entry(const char *label, const cJSON *member,
                           const fr_resolver_entry *resolvers, size_t resolver_count,
                           fr_plugin_entry *out, fr_error *err) {
    if (cJSON_IsString(member) && member->valuestring != NULL) {
        return parse_string_form(label, member->valuestring, resolvers, resolver_count, out, err);
    }
    if (cJSON_IsObject(member)) return parse_table_form(label, member, out, err);

    fr_error_set(err, "plugin \"%s\" must be a string or a table", label);
    return FR_ERR;
}

int fr_plugins_parse(const struct cJSON *document, const fr_resolver_entry *resolvers,
                     size_t resolver_count, fr_plugin_entry **out, size_t *out_count,
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
        slot->label = fr_dup_string(label);
        if (slot->label == NULL) {
            out_of_memory(label, err);
            fr_plugins_free(entries, count);
            return FR_ERR;
        }

        if (fr_plugins_parse_entry(label, member, resolvers, resolver_count, slot, err) != FR_OK) {
            fr_plugins_free(entries, count);
            return FR_ERR;
        }
    }

    *out = entries;
    *out_count = count;
    return FR_OK;
}

int fr_plugins_reject_in_fetched(const struct cJSON *document, const char *project, fr_error *err) {
    if (cJSON_GetObjectItemCaseSensitive(document, "plugins") == NULL) return FR_OK;
    fr_error_set(err, "project \"%s\" declares plugins, which only the root manifest may do",
                 project);
    return FR_ERR;
}

void fr_plugins_free_entry(fr_plugin_entry *entry) {
    free(entry->label);
    free(entry->path);
    free(entry->url);
    free(entry->resolver);
    free(entry->coordinate);
    free(entry->sha256);
    memset(entry, 0, sizeof *entry);
}

void fr_plugins_free(fr_plugin_entry *entries, size_t count) {
    if (entries == NULL) return;
    for (size_t index = 0; index < count; index++) fr_plugins_free_entry(&entries[index]);
    free(entries);
}

static int unknown_resolver(const fr_plugin_entry *entry, const fr_resolver_entry *resolvers,
                            size_t resolver_count, fr_error *err) {
    char declared[160];
    describe_declared_resolvers(declared, sizeof declared, resolvers, resolver_count);
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
        if (*out_origin == NULL) return out_of_memory(entry->label, err);
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

static int load_one(const fr_plugin_entry *entry, const fr_resolver_entry *resolvers,
                    size_t resolver_count, fr_error *err) {
    char *origin = NULL;
    char *text = NULL;
    char *resolved = NULL;
    size_t length = 0;
    char digest[65];
    fr_plugin_source *source = NULL;
    if (fr_plugins_acquire_source(entry, resolvers, resolver_count, &text, &length, &origin,
                                  &resolved, digest, &source, err)
        != FR_OK) {
        return FR_ERR;
    }

    /* The digest above covers the whole artifact; what RUNS is its entry, which
       for an archive is one member of it. */
    const char *chunk = NULL;
    size_t chunk_length = 0;
    int status = fr_plugin_source_entry(source, &chunk, &chunk_length, err);

    lua_State *state = fr_lua_runtime_state();
    fr_plugin_declaration declaration;
    memset(&declaration, 0, sizeof declaration);
    declaration.kind = "plugin";
    declaration.label = entry->label;
    if (status == FR_OK) {
        status = fr_plugins_declaration_read(state, chunk, chunk_length, origin, &declaration,
                                             err);
    }

    fr_plugin_deps *deps = NULL;
    if (status == FR_OK) {
        status = fr_plugin_deps_acquire(&declaration, entry->overrides, resolvers, resolver_count,
                                        &deps, err);
    }
    if (status == FR_OK) {
        fr_lua_verbs_set_declared_env((const char *const *) declaration.env,
                                      declaration.env_count);
        fr_lua_declare_set_required_aliases(declaration.requires, declaration.requires_count);
        status = fr_lua_plugin_load(chunk, chunk_length, origin,
                                    (const char *const *) declaration.uses,
                                    declaration.uses_count, source, deps, err);
        fr_lua_declare_set_required_aliases(NULL, 0);
        fr_lua_verbs_set_declared_env(NULL, 0);
    }
    if (status == FR_OK) {
        status = fr_plugins_report_append(entry, origin, resolved, digest, &declaration, err);
    }
    if (status == FR_OK) {
        status = fr_plugins_report_append_dependencies(deps, err);
    }

    fr_plugin_deps_close(deps);
    fr_plugins_free_declaration(&declaration);
    fr_plugin_source_close(source);
    free(text);
    free(origin);
    free(resolved);
    return status;
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
            status = load_one(&entries[index], resolvers, resolver_count, err);
        }
    }

    fr_plugins_free(entries, count);
    fr_resolvers_free(resolvers, resolver_count);
    return status;
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
        if (label != NULL && strcmp(entry->label, label) != 0) continue;
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
