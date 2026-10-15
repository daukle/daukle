#include "plugin/plugin_modules.h"

#include "archive/tar.h"
#include "lua/lua_sandbox.h"
#include "project/region.h"
#include "util/error.h"
#include "util/tree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENTRY_MEMBER "plugin.lua"
#define MODULES_DIRECTORY "modules"
#define MAIN_MODULE "main"
#define MAX_CACHED FR_TAR_MAX_MEMBERS

/* "main" names plugin.lua on a [plugins] line, so a modules/main.lua is a file
   no line can ever reach, and saying so at load is the only time its author
   finds out. */
#define UNREACHABLE_MAIN_REFUSAL "the plugin carries " MODULES_DIRECTORY "/" MAIN_MODULE ".lua," \
    " which can never load: the module \"" MAIN_MODULE "\" is " ENTRY_MEMBER ", so rename it"

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
            if (strcmp(name, MODULES_DIRECTORY "/" MAIN_MODULE ".lua") == 0) {
                fr_error_set(err, UNREACHABLE_MAIN_REFUSAL);
                free(source);
                return FR_ERR;
            }
        }
    }

    *out = source;
    return FR_OK;
}

/* Resolving canonicalises, and canonicalising a path that does not exist
   fails, so a resolve that succeeds is the existence check. */
static int directory_carries_unreachable_main(const fr_plugin_source *source) {
    char relative[1024];
    int written = snprintf(relative, sizeof relative, "%s/" MODULES_DIRECTORY "/" MAIN_MODULE ".lua",
                           source->directory);
    if (written < 0 || (size_t) written >= sizeof relative) return 0;
    char *resolved = NULL;
    fr_error ignored;
    if (fr_lua_sandbox_resolve(source->state, relative, &resolved, &ignored) != FR_OK) return 0;
    free(resolved);
    return 1;
}

typedef struct {
    const char *wanted;
    int found;
    char names[256];
    size_t filled;
} module_listing;

static void note_module(module_listing *listing, const char *file_name) {
    if (!ends_with_lua(file_name)) return;
    size_t name_length = strlen(file_name) - 4;
    if (strlen(listing->wanted) == name_length
        && strncmp(file_name, listing->wanted, name_length) == 0) {
        listing->found = 1;
    }
    size_t room = sizeof listing->names - listing->filled;
    int written = snprintf(listing->names + listing->filled, room, "%s%.*s",
                           listing->filled == 0 ? "" : ", ", (int) name_length, file_name);
    if (written < 0 || (size_t) written >= room) {
        listing->names[listing->filled] = '\0';
        return;
    }
    listing->filled += (size_t) written;
}

static void note_directory_module(const char *name, int is_directory, int is_link, void *context) {
    (void) is_link;
    if (!is_directory) note_module(context, name);
}

static void list_modules(const fr_plugin_source *source, module_listing *listing) {
    if (source->kind == SOURCE_ARCHIVE) {
        const size_t prefix_length = sizeof MODULES_DIRECTORY;
        for (size_t index = 0; index < source->archive.count; index++) {
            const char *name = source->archive.members[index].name;
            if (strncmp(name, MODULES_DIRECTORY "/", prefix_length) != 0) continue;
            if (strchr(name + prefix_length, '/') != NULL) continue;
            note_module(listing, name + prefix_length);
        }
        return;
    }
    char relative[1024];
    int written = snprintf(relative, sizeof relative, "%s/" MODULES_DIRECTORY, source->directory);
    if (written < 0 || (size_t) written >= sizeof relative) return;
    char *resolved = NULL;
    fr_error ignored;
    if (fr_lua_sandbox_resolve_dir(source->state, relative, &resolved, &ignored) != FR_OK) return;
    fr_list_directory(resolved, note_directory_module, listing);
    free(resolved);
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

    if (directory_carries_unreachable_main(source)) {
        fr_error_set(err, UNREACHABLE_MAIN_REFUSAL);
        fr_plugin_source_close(source);
        return FR_ERR;
    }

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

int fr_plugin_source_module_entry(fr_plugin_source *source, const char *module,
                                  const char **out_text, size_t *out_length, fr_error *err) {
    if (strcmp(module, "main") == 0) {
        return fr_plugin_source_entry(source, out_text, out_length, err);
    }
    if (source->kind == SOURCE_CHUNK) {
        fr_error_set(err, "this plugin is a single file, so it has no module \"%s\" to load",
                     module);
        return FR_ERR;
    }
    module_listing listing = { module, 0, "", 0 };
    list_modules(source, &listing);
    if (!listing.found) {
        if (listing.filled == 0) {
            fr_error_set(err, "there is no module \"%s\": the plugin has no modules, only its "
                              ENTRY_MEMBER, module);
        } else {
            fr_error_set(err, "there is no module \"%s\": the plugin's modules are %s", module,
                         listing.names);
        }
        return FR_ERR;
    }
    char member_name[FR_TAR_MAX_NAME + 1];
    snprintf(member_name, sizeof member_name, MODULES_DIRECTORY "/%s", module);
    return fr_plugin_source_member(source, member_name, out_text, out_length, err);
}

void fr_plugin_source_close(fr_plugin_source *source) {
    if (source == NULL) return;
    for (size_t index = 0; index < source->cached_count; index++) free(source->cached[index].text);
    free(source->directory);
    free(source);
}
