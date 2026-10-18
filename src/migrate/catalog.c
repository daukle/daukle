#include "migrate/catalog_internal.h"

#include "config/jsonx.h"
#include "net/http.h"
#include "plugin/plugins.h"
#include "project/region.h"
#include "util/error.h"
#include "util/strbuf.h"

#include <stdlib.h>
#include <string.h>

int fr_catalog_pattern_has_bare_hyphen(const char *pattern) {
    int in_set = 0;
    for (size_t index = 0; pattern[index] != '\0'; index++) {
        char c = pattern[index];
        if (c == '%') {
            if (pattern[index + 1] == '\0') return 0;
            index++;
            continue;
        }
        if (in_set) {
            if (c == ']') in_set = 0;
            continue;
        }
        if (c == '[') {
            in_set = 1;
            if (pattern[index + 1] == '^') index++;
            if (pattern[index + 1] == ']') index++;
            continue;
        }
        if (c == '-') return 1;
    }
    return 0;
}

const fr_catalog_plugin *fr_catalog_find(const fr_catalog *catalog, const char *id) {
    for (size_t index = 0; index < catalog->count; index++) {
        if (strcmp(catalog->plugins[index].id, id) == 0) return &catalog->plugins[index];
    }
    return NULL;
}

void fr_catalog_free(fr_catalog *catalog) {
    for (size_t index = 0; index < catalog->count; index++) {
        fr_catalog_plugin_release(&catalog->plugins[index]);
    }
    for (size_t index = 0; index < catalog->warning_count; index++) free(catalog->warnings[index]);
    free(catalog->plugins);
    free(catalog->warnings);
    memset(catalog, 0, sizeof *catalog);
}

static int out_of_memory(fr_error *err, const char *location) {
    fr_error_set(err, "reading catalog %s: out of memory", location);
    return FR_ERR;
}

static int add_warning(fr_catalog *catalog, char *text) {
    if (text == NULL) return FR_ERR;
    char **grown = realloc(catalog->warnings, (catalog->warning_count + 1) * sizeof *grown);
    if (grown == NULL) {
        free(text);
        return FR_ERR;
    }
    catalog->warnings = grown;
    catalog->warnings[catalog->warning_count++] = text;
    return FR_OK;
}

static int warn_refused(fr_catalog *catalog, const char *location, const char *id, const char *reason) {
    fr_strbuf text;
    fr_strbuf_init(&text);
    fr_strbuf_append_format(&text, "%s: catalog entry \"%s\" %s; it was not evaluated",
                            location, id != NULL ? id : "?", reason);
    return add_warning(catalog, fr_strbuf_release(&text));
}

static int warn_duplicate(fr_catalog *catalog, const char *location, const fr_catalog_plugin *kept) {
    fr_strbuf text;
    fr_strbuf_init(&text);
    fr_strbuf_append_format(&text, "%s: catalog entry \"%s\" is already supplied by %s; the first one is kept",
                            location, kept->id, kept->location);
    return add_warning(catalog, fr_strbuf_release(&text));
}

static int append_plugin(fr_catalog *catalog, fr_catalog_plugin *plugin) {
    fr_catalog_plugin *grown = realloc(catalog->plugins, (catalog->count + 1) * sizeof *grown);
    if (grown == NULL) return FR_ERR;
    catalog->plugins = grown;
    catalog->plugins[catalog->count++] = *plugin;
    return FR_OK;
}

static int take_entry(const cJSON *json, const char *location, size_t catalog_index,
                      fr_catalog *into) {
    fr_catalog_plugin plugin;
    memset(&plugin, 0, sizeof plugin);
    fr_error reason;
    const char *id_seen = NULL;
    int status = fr_catalog_read_entry(json, &plugin, &reason, &id_seen);
    if (status == FR_ENTRY_NO_MEMORY) return FR_ERR;
    if (status == FR_ENTRY_REFUSED) return warn_refused(into, location, id_seen, reason.message);
    const fr_catalog_plugin *earlier = fr_catalog_find(into, plugin.id);
    if (earlier != NULL) {
        int warned = warn_duplicate(into, location, earlier);
        fr_catalog_plugin_release(&plugin);
        return warned;
    }
    plugin.catalog_index = catalog_index;
    plugin.location = fr_dup_string(location);
    if (plugin.location == NULL || append_plugin(into, &plugin) != FR_OK) {
        fr_catalog_plugin_release(&plugin);
        return FR_ERR;
    }
    return FR_OK;
}

int fr_catalog_parse(const char *text, const char *location, size_t catalog_index,
                     fr_catalog *into, fr_error *err) {
    cJSON *document = NULL;
    fr_error inner;
    if (fr_json_parse(text, &document, &inner) != FR_OK) {
        fr_error_set(err, "%s: the catalog is not valid JSON: %s", location, inner.message);
        return FR_ERR;
    }
    const cJSON *plugins = cJSON_IsObject(document)
        ? cJSON_GetObjectItemCaseSensitive(document, "plugins") : NULL;
    if (!cJSON_IsArray(plugins)) {
        cJSON_Delete(document);
        fr_error_set(err, "%s: the catalog is not an object with a \"plugins\" array", location);
        return FR_ERR;
    }
    const cJSON *entry;
    cJSON_ArrayForEach(entry, plugins) {
        if (take_entry(entry, location, catalog_index, into) != FR_OK) {
            cJSON_Delete(document);
            return out_of_memory(err, location);
        }
    }
    cJSON_Delete(document);
    return FR_OK;
}

#define PLAIN_HTTP "http://"

static int is_remote(const char *location) {
    return strncmp(location, PLAIN_HTTP, strlen(PLAIN_HTTP)) == 0
        || strncmp(location, "https://", 8) == 0;
}

static int is_loopback_host(const char *authority, size_t length) {
    static const char *const LOOPBACK[] = { "127.0.0.1", "localhost", "[::1]" };
    for (size_t index = 0; index < sizeof LOOPBACK / sizeof *LOOPBACK; index++) {
        size_t host_length = strlen(LOOPBACK[index]);
        if (length >= host_length && strncmp(authority, LOOPBACK[index], host_length) == 0
            && (length == host_length || authority[host_length] == ':')) {
            return 1;
        }
    }
    return 0;
}

/* A catalog pins the plugin code migrate runs, URL and digest alike, so it is
   only as trustworthy as its transport; plain http stays usable for a server
   on this machine. A userinfo part is refused outright, since it is how
   "localhost@" would otherwise pass for a loopback host. */
static int is_plain_http_off_this_machine(const char *location) {
    if (strncmp(location, PLAIN_HTTP, strlen(PLAIN_HTTP)) != 0) return 0;
    const char *authority = location + strlen(PLAIN_HTTP);
    size_t length = strcspn(authority, "/?#");
    if (memchr(authority, '@', length) != NULL) return 1;
    return !is_loopback_host(authority, length);
}

static int read_location(const char *location, char **body, fr_error *err) {
    if (is_plain_http_off_this_machine(location)) {
        fr_error_set(err, "reading catalog %s: a catalog pins the plugin code migrate runs, so it"
                          " must arrive over https; plain http is accepted only from 127.0.0.1,"
                          " localhost or [::1]", location);
        return FR_ERR;
    }
    size_t length = 0;
    fr_error inner;
    int status = is_remote(location) ? fr_http_get(location, NULL, 0, body, &length, &inner)
                                     : fr_file_read_bytes(location, body, &length, &inner);
    if (status != FR_OK) {
        fr_error_set(err, "reading catalog %s: %s", location, inner.message);
        return FR_ERR;
    }
    if (*body == NULL || length == 0) {
        free(*body);
        *body = NULL;
        fr_error_set(err, "reading catalog %s: the catalog is empty", location);
        return FR_ERR;
    }
    return FR_OK;
}

int fr_catalog_load(const char *const *locations, size_t count, fr_catalog *out, fr_error *err) {
    memset(out, 0, sizeof *out);
    for (size_t index = 0; index < count; index++) {
        char *body = NULL;
        if (read_location(locations[index], &body, err) != FR_OK) {
            fr_catalog_free(out);
            return FR_ERR;
        }
        int status = fr_catalog_parse(body, locations[index], index, out, err);
        free(body);
        if (status != FR_OK) {
            fr_catalog_free(out);
            return FR_ERR;
        }
    }
    return FR_OK;
}

static int is_separator(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static size_t count_items(const char *text) {
    size_t items = 0;
    int inside = 0;
    for (; *text != '\0'; text++) {
        if (is_separator(*text)) {
            inside = 0;
        } else if (!inside) {
            inside = 1;
            items++;
        }
    }
    return items;
}

static size_t split_in_place(char *text, const char **items) {
    size_t found = 0;
    int inside = 0;
    for (; *text != '\0'; text++) {
        if (is_separator(*text)) {
            *text = '\0';
            inside = 0;
        } else if (!inside) {
            inside = 1;
            items[found++] = text;
        }
    }
    return found;
}

int fr_catalog_locations(const char *environment, const char *const *flags, size_t flag_count,
                         const char ***out, size_t *out_count, fr_error *err) {
    const char *source = environment != NULL ? environment : "";
    size_t text_size = strlen(source) + 1;
    size_t environment_count = count_items(source);
    size_t total = environment_count + flag_count;
    const char **block = malloc((total + 1) * sizeof *block + text_size);
    if (block == NULL) {
        fr_error_set(err, "reading the catalog locations: out of memory");
        return FR_ERR;
    }
    char *text = (char *) (block + total + 1);
    memcpy(text, source, text_size);
    split_in_place(text, block);
    for (size_t index = 0; index < flag_count; index++) block[environment_count + index] = flags[index];
    *out = block;
    *out_count = total;
    return FR_OK;
}
