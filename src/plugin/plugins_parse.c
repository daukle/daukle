#include "plugin/plugins.h"

#include "plugin/plugins_internal.h"

#include "cJSON.h"
#include "config/jsonx.h"
#include "plugin/resolvers.h"
#include "util/error.h"

#include <stdlib.h>
#include <string.h>

static int no_resolver_named(const char *label, const char *value,
                             const fr_resolver_entry *resolvers, size_t resolver_count,
                             fr_error *err) {
    char declared[160];
    fr_plugins_describe_declared_resolvers(declared, sizeof declared, resolvers, resolver_count);
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
        return out->path != NULL ? FR_OK : fr_plugins_out_of_memory(label, err);
    }

    if (strncmp(value, "http://", 7) == 0 || strncmp(value, "https://", 8) == 0) {
        out->kind = FR_PLUGIN_URL;
        out->url = fr_dup_string(value);
        return out->url != NULL ? FR_OK : fr_plugins_out_of_memory(label, err);
    }

    const char *colon = strchr(value, ':');
    if (colon == NULL || colon == value || colon[1] == '\0') {
        return no_resolver_named(label, value, resolvers, resolver_count, err);
    }

    out->kind = FR_PLUGIN_RESOLVED;
    out->resolver = fr_dup_prefix(value, (size_t) (colon - value));
    out->coordinate = fr_dup_string(colon + 1);
    return (out->resolver != NULL && out->coordinate != NULL) ? FR_OK : fr_plugins_out_of_memory(label, err);
}

/* A table entry names exactly one of "path", "url", or "resolver" with
   "coordinate", the three forms the string form discriminates between, and any
   of them may also carry a "sha256" pin. */
/* Every key a table entry defines. A name outside this list used to be accepted
   and discarded, which cost a published AUTHORING.md an hour of being wrong:
   "asset" reads as doing something because the resolver really does read a key
   of that name, just never from here. D-44. */
static const char *const PLUGIN_ENTRY_KEYS[] = {
    "requires", "sha256", "path", "url", "resolver", "coordinate", "modules"
};

static int plugin_entry_key_is_known(const char *name) {
    size_t count = sizeof PLUGIN_ENTRY_KEYS / sizeof PLUGIN_ENTRY_KEYS[0];
    for (size_t index = 0; index < count; index++) {
        if (strcmp(PLUGIN_ENTRY_KEYS[index], name) == 0) return 1;
    }
    return 0;
}

static int refuse_an_unknown_entry_key(const char *label, const cJSON *member, fr_error *err) {
    for (const cJSON *field = member->child; field != NULL; field = field->next) {
        if (field->string == NULL || plugin_entry_key_is_known(field->string)) continue;
        /* "asset" is named on its own because it is the one that was measured
           reaching nothing, and because the form that DOES work is not
           guessable from a list of valid keys. */
        if (strcmp(field->string, "asset") == 0) {
            fr_error_set(err, "plugin \"%s\": \"asset\" belongs on the [resolvers.*] entry this"
                              " plugin resolves through, not here, and naming it here reached"
                              " nothing. Declare a second resolver carrying it", label);
        } else {
            fr_error_set(err, "plugin \"%s\": \"%s\" is not a key a plugin entry defines; it"
                              " takes path, url, resolver, coordinate, sha256, requires and"
                              " modules", label, field->string);
        }
        return FR_ERR;
    }
    return FR_OK;
}

static int module_name_is_plain(const char *name) {
    if (name[0] == '\0' || strlen(name) > 32) return 0;
    for (const char *cursor = name; *cursor != '\0'; cursor++) {
        char c = *cursor;
        int plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                    || c == '-' || c == '_';
        if (!plain) return 0;
    }
    return 1;
}

static int equal_ignoring_ascii_case(const char *left, const char *right) {
    for (; *left != '\0' && *right != '\0'; left++, right++) {
        char folded_left = (*left >= 'A' && *left <= 'Z') ? (char) (*left + 32) : *left;
        char folded_right = (*right >= 'A' && *right <= 'Z') ? (char) (*right + 32) : *right;
        if (folded_left != folded_right) return 0;
    }
    return *left == *right;
}

/* Case-blind, because two names differing only in case are one file on a
   case-insensitive file system, and loading it twice fails there as a
   capability collision that names neither. */
static const char *module_already_named(const fr_plugin_entry *out, const char *name) {
    for (size_t index = 0; index < out->module_count; index++) {
        if (equal_ignoring_ascii_case(out->modules[index], name)) return out->modules[index];
    }
    return NULL;
}

static int parse_modules(const char *label, const cJSON *member, fr_plugin_entry *out,
                         fr_error *err) {
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(member, "modules");
    if (list == NULL) return FR_OK;
    if (!cJSON_IsArray(list)) {
        fr_error_set(err, "plugin \"%s\": \"modules\" must be a list of module names", label);
        return FR_ERR;
    }
    if (cJSON_GetArraySize(list) == 0) {
        fr_error_set(err, "plugin \"%s\": \"modules\" names no module; leave it out to load"
                          " the plugin's main chunk", label);
        return FR_ERR;
    }
    for (const cJSON *item = list->child; item != NULL; item = item->next) {
        if (!cJSON_IsString(item) || !module_name_is_plain(item->valuestring)) {
            fr_error_set(err, "plugin \"%s\": every \"modules\" entry is a name of letters,"
                              " digits, - and _, at most 32 long", label);
            return FR_ERR;
        }
        const char *earlier = module_already_named(out, item->valuestring);
        if (earlier != NULL && strcmp(earlier, item->valuestring) == 0) {
            fr_error_set(err, "plugin \"%s\": \"modules\" names \"%s\" twice", label,
                         item->valuestring);
            return FR_ERR;
        }
        if (earlier != NULL) {
            fr_error_set(err, "plugin \"%s\": \"modules\" names \"%s\" and \"%s\", which differ"
                              " only in case and are one file on a case-insensitive file system",
                         label, earlier, item->valuestring);
            return FR_ERR;
        }
        if (out->module_count == FR_PLUGIN_MAX_MODULES) {
            fr_error_set(err, "plugin \"%s\": \"modules\" names more than %d modules", label,
                         FR_PLUGIN_MAX_MODULES);
            return FR_ERR;
        }
        out->modules[out->module_count] = fr_dup_string(item->valuestring);
        if (out->modules[out->module_count] == NULL) return fr_plugins_out_of_memory(label, err);
        out->module_count++;
    }
    return FR_OK;
}

static int parse_table_form(const char *label, const cJSON *member, fr_plugin_entry *out,
                            fr_error *err) {
    if (refuse_an_unknown_entry_key(label, member, err) != FR_OK) return FR_ERR;
    if (parse_modules(label, member, out, err) != FR_OK) return FR_ERR;

    /* Not validated here: plugin_deps.c is the sole reader, and validating an
       absent table would tax the common case (no requires at all) for
       nothing. */
    out->overrides = cJSON_GetObjectItemCaseSensitive(member, "requires");

    if (cJSON_GetObjectItemCaseSensitive(member, "sha256") != NULL) {
        const char *sha256 = NULL;
        if (fr_json_string(member, "sha256", label, &sha256, err) != FR_OK) return FR_ERR;
        out->sha256 = fr_dup_string(sha256);
        if (out->sha256 == NULL) return fr_plugins_out_of_memory(label, err);
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
        return out->path != NULL ? FR_OK : fr_plugins_out_of_memory(label, err);
    }

    if (has_url) {
        const char *url = NULL;
        if (fr_json_string(member, "url", label, &url, err) != FR_OK) return FR_ERR;

        out->kind = FR_PLUGIN_URL;
        out->url = fr_dup_string(url);
        return out->url != NULL ? FR_OK : fr_plugins_out_of_memory(label, err);
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
    return (out->resolver != NULL && out->coordinate != NULL) ? FR_OK : fr_plugins_out_of_memory(label, err);
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
        if (strchr(label, '/') != NULL) {
            fr_error_set(err, "plugin \"%s\": a [plugins] key may not contain \"/\", which names"
                              " one module of a plugin, as in \"<plugin>/<module>\"", label);
            fr_plugins_free(entries, count);
            return FR_ERR;
        }
        count = count + 1;

        fr_plugin_entry *slot = &entries[count - 1];
        slot->label = fr_dup_string(label);
        if (slot->label == NULL) {
            fr_plugins_out_of_memory(label, err);
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
    for (size_t index = 0; index < entry->module_count; index++) free(entry->modules[index]);
    memset(entry, 0, sizeof *entry);
}

void fr_plugins_free(fr_plugin_entry *entries, size_t count) {
    if (entries == NULL) return;
    for (size_t index = 0; index < count; index++) fr_plugins_free_entry(&entries[index]);
    free(entries);
}
