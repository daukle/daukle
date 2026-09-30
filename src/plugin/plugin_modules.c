#include "plugin/plugin_modules.h"

#include "archive/tar.h"
#include "lua/lua_sandbox.h"
#include "project/region.h"
#include "util/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENTRY_MEMBER "plugin.lua"
#define MAX_CACHED FR_TAR_MAX_MEMBERS

typedef enum { SOURCE_CHUNK, SOURCE_ARCHIVE, SOURCE_DIRECTORY } source_kind;

typedef struct {
    char name[FR_TAR_MAX_NAME + 1];
    char *text;
    size_t length;
} cached_member;

struct fr_plugin_source {
    source_kind kind;
    const char *bytes;
    size_t length;
    fr_tar archive;
    lua_State *state;
    char *directory;
    cached_member cached[MAX_CACHED];
    size_t cached_count;
};

static int ends_with_lua(const char *name) {
    size_t length = strlen(name);
    return length > 4 && strcmp(name + length - 4, ".lua") == 0;
}

static fr_plugin_source *allocate(source_kind kind) {
    fr_plugin_source *source = calloc(1, sizeof *source);
    if (source != NULL) source->kind = kind;
    return source;
}

int fr_plugin_source_open_bytes(const char *bytes, size_t length, fr_plugin_source **out,
                                fr_error *err) {
    *out = NULL;

    int is_archive = fr_tar_looks_like_archive(bytes, length);
    fr_plugin_source *source = allocate(is_archive ? SOURCE_ARCHIVE : SOURCE_CHUNK);
    if (source == NULL) {
        fr_error_set(err, "out of memory reading a plugin");
        return FR_ERR;
    }
    source->bytes = bytes;
    source->length = length;

    if (is_archive) {
        if (fr_tar_read(bytes, length, &source->archive, err) != FR_OK) {
            free(source);
            return FR_ERR;
        }
        for (size_t index = 0; index < source->archive.count; index++) {
            const char *name = source->archive.members[index].name;
            if (!ends_with_lua(name)) {
                fr_error_set(err, "the archive member \"%s\" is not a .lua file", name);
                free(source);
                return FR_ERR;
            }
            if (fr_lua_sandbox_climbs_out(name)) {
                fr_error_set(err, "the archive member \"%s\" would leave the plugin", name);
                free(source);
                return FR_ERR;
            }
        }
    }

    *out = source;
    return FR_OK;
}

int fr_plugin_source_open_directory(lua_State *state, const char *relative_directory,
                                    fr_plugin_source **out, fr_error *err) {
    *out = NULL;

    fr_plugin_source *source = allocate(SOURCE_DIRECTORY);
    if (source == NULL) {
        fr_error_set(err, "out of memory reading a plugin");
        return FR_ERR;
    }

    size_t length = strlen(relative_directory);
    source->directory = malloc(length + 1);
    if (source->directory == NULL) {
        free(source);
        fr_error_set(err, "out of memory reading a plugin");
        return FR_ERR;
    }
    memcpy(source->directory, relative_directory, length + 1);
    source->state = state;

    *out = source;
    return FR_OK;
}

static const cached_member *find_cached(const fr_plugin_source *source, const char *name) {
    for (size_t index = 0; index < source->cached_count; index++) {
        if (strcmp(source->cached[index].name, name) == 0) return &source->cached[index];
    }
    return NULL;
}

/* Read once and kept, so every form hands back borrowed bytes that live as long
   as the source does and a caller never has to know which form it asked. */
static int read_from_directory(fr_plugin_source *source, const char *file_name,
                               const char **out_text, size_t *out_length, fr_error *err) {
    const cached_member *existing = find_cached(source, file_name);
    if (existing != NULL) {
        *out_text = existing->text;
        *out_length = existing->length;
        return FR_OK;
    }
    if (source->cached_count == MAX_CACHED) {
        fr_error_set(err, "the plugin reads more than %d modules", MAX_CACHED);
        return FR_ERR;
    }

    char relative[1024];
    int written = snprintf(relative, sizeof relative, "%s/%s", source->directory, file_name);
    if (written < 0 || (size_t) written >= sizeof relative) {
        fr_error_set(err, "the path to \"%s\" is too long", file_name);
        return FR_ERR;
    }

    char *resolved = NULL;
    if (fr_lua_sandbox_resolve(source->state, relative, &resolved, err) != FR_OK) return FR_ERR;

    char *text = NULL;
    size_t length = 0;
    int status = fr_file_read_bytes(resolved, &text, &length, err);
    free(resolved);
    if (status != FR_OK) return FR_ERR;

    cached_member *slot = &source->cached[source->cached_count++];
    snprintf(slot->name, sizeof slot->name, "%s", file_name);
    slot->text = text;
    slot->length = length;

    *out_text = text;
    *out_length = length;
    return FR_OK;
}

int fr_plugin_source_entry(fr_plugin_source *source, const char **out_text, size_t *out_length,
                           fr_error *err) {
    if (source->kind == SOURCE_CHUNK) {
        *out_text = source->bytes;
        *out_length = source->length;
        return FR_OK;
    }
    if (source->kind == SOURCE_DIRECTORY) {
        return read_from_directory(source, ENTRY_MEMBER, out_text, out_length, err);
    }

    const fr_tar_member *member = fr_tar_find(&source->archive, ENTRY_MEMBER);
    if (member == NULL) {
        fr_error_set(err, "the archive has no " ENTRY_MEMBER " to run");
        return FR_ERR;
    }
    *out_text = member->bytes;
    *out_length = member->length;
    return FR_OK;
}

typedef enum {
    MODULE_NAME_OK,
    MODULE_NAME_EMPTY,
    MODULE_NAME_ESCAPES,
    MODULE_NAME_HAS_COLON,
    MODULE_NAME_HAS_LUA_SUFFIX,
    MODULE_NAME_HAS_BACKSLASH,
    MODULE_NAME_TRAILING_SLASH,
    MODULE_NAME_TOO_LONG
} module_name_problem;

/* One rule set, read once: fr_plugin_module_name_check and fr_plugin_export_name_check word the
   same problem for two different readers (an author who wrote a require call, and one who wrote an
   exports entry) rather than checking it twice. */
static module_name_problem module_name_problem_of(const char *name) {
    size_t length = strlen(name);
    if (length == 0) return MODULE_NAME_EMPTY;
    /* Before the colon rule below, because a windows drive letter carries one
       too and "C:/x" is an escape rather than a coordinate. */
    if (fr_lua_sandbox_climbs_out(name)) return MODULE_NAME_ESCAPES;
    if (strchr(name, ':') != NULL) return MODULE_NAME_HAS_COLON;
    if (ends_with_lua(name)) return MODULE_NAME_HAS_LUA_SUFFIX;
    if (strchr(name, '\\') != NULL) return MODULE_NAME_HAS_BACKSLASH;
    if (name[length - 1] == '/') return MODULE_NAME_TRAILING_SLASH;
    if (length + 4 > FR_TAR_MAX_NAME) return MODULE_NAME_TOO_LONG;
    return MODULE_NAME_OK;
}

/* Every refusal says what to write instead, because the caller is a plugin
   author reading one line of output. */
int fr_plugin_module_name_check(const char *name, fr_error *err) {
    size_t length = strlen(name);
    switch (module_name_problem_of(name)) {
    case MODULE_NAME_OK:
        return FR_OK;
    case MODULE_NAME_EMPTY:
        fr_error_set(err, "daukle.require was given an empty module name");
        return FR_ERR;
    case MODULE_NAME_ESCAPES:
        fr_error_set(err, "daukle.require(\"%s\"): a module name may not leave the plugin", name);
        return FR_ERR;
    case MODULE_NAME_HAS_COLON:
        fr_error_set(err, "daukle.require(\"%s\"): a module name may contain at most one \":\","
                          " which separates a plugin from its module", name);
        return FR_ERR;
    case MODULE_NAME_HAS_LUA_SUFFIX:
        fr_error_set(err, "daukle.require takes a module name: write \"%.*s\" without the \".lua\"",
                     (int) (length - 4), name);
        return FR_ERR;
    case MODULE_NAME_HAS_BACKSLASH:
        fr_error_set(err, "daukle.require(\"%s\"): a module name separates with \"/\"", name);
        return FR_ERR;
    case MODULE_NAME_TRAILING_SLASH:
        fr_error_set(err, "daukle.require(\"%s\"): a module name does not end in \"/\"", name);
        return FR_ERR;
    case MODULE_NAME_TOO_LONG:
        fr_error_set(err, "daukle.require was given a module name longer than %d bytes",
                     FR_TAR_MAX_NAME - 4);
        return FR_ERR;
    }
    return FR_OK;
}

int fr_plugin_export_name_check(const char *name, fr_error *err) {
    size_t length = strlen(name);
    switch (module_name_problem_of(name)) {
    case MODULE_NAME_OK:
        return FR_OK;
    case MODULE_NAME_EMPTY:
        fr_error_set(err, "exports names an empty module name");
        return FR_ERR;
    case MODULE_NAME_ESCAPES:
        fr_error_set(err, "the export \"%s\" may not leave the plugin", name);
        return FR_ERR;
    case MODULE_NAME_HAS_COLON:
        fr_error_set(err, "the export \"%s\" may contain at most one \":\", which separates a"
                          " plugin from its module", name);
        return FR_ERR;
    case MODULE_NAME_HAS_LUA_SUFFIX:
        fr_error_set(err, "an export name is written without the \".lua\" suffix: write \"%.*s\"",
                     (int) (length - 4), name);
        return FR_ERR;
    case MODULE_NAME_HAS_BACKSLASH:
        fr_error_set(err, "the export \"%s\" separates with \"/\"", name);
        return FR_ERR;
    case MODULE_NAME_TRAILING_SLASH:
        fr_error_set(err, "the export \"%s\" does not end in \"/\"", name);
        return FR_ERR;
    case MODULE_NAME_TOO_LONG:
        fr_error_set(err, "an export name may not be longer than %d bytes", FR_TAR_MAX_NAME - 4);
        return FR_ERR;
    }
    return FR_OK;
}

static void report_missing(const fr_plugin_source *source, const char *file_name, fr_error *err) {
    char listed[256];
    size_t filled = 0;
    listed[0] = '\0';
    for (size_t index = 0; index < source->archive.count; index++) {
        int written = snprintf(listed + filled, sizeof listed - filled, "%s%s",
                               filled == 0 ? "" : ", ", source->archive.members[index].name);
        if (written < 0 || (size_t) written >= sizeof listed - filled) break;
        filled += (size_t) written;
    }
    fr_error_set(err, "the plugin has no \"%s\"; it carries %s", file_name, listed);
}

int fr_plugin_source_member(fr_plugin_source *source, const char *module_name,
                            const char **out_text, size_t *out_length, fr_error *err) {
    if (fr_plugin_module_name_check(module_name, err) != FR_OK) return FR_ERR;

    char file_name[FR_TAR_MAX_NAME + 1];
    snprintf(file_name, sizeof file_name, "%s.lua", module_name);

    if (source->kind == SOURCE_CHUNK) {
        fr_error_set(err, "daukle.require(\"%s\"): this plugin is a single file and has no modules",
                     module_name);
        return FR_ERR;
    }
    if (source->kind == SOURCE_DIRECTORY) {
        return read_from_directory(source, file_name, out_text, out_length, err);
    }

    const fr_tar_member *member = fr_tar_find(&source->archive, file_name);
    if (member == NULL) {
        report_missing(source, file_name, err);
        return FR_ERR;
    }
    *out_text = member->bytes;
    *out_length = member->length;
    return FR_OK;
}

void fr_plugin_source_close(fr_plugin_source *source) {
    if (source == NULL) return;
    for (size_t index = 0; index < source->cached_count; index++) free(source->cached[index].text);
    free(source->directory);
    free(source);
}
