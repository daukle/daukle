#include "migrate/detect_internal.h"

#include "project/region.h"
#include "util/error.h"
#include "util/strbuf.h"
#include "util/tree.h"

#include "lauxlib.h"
#include "lualib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CONTENT_BYTES (1024L * 1024L)

typedef struct {
    const fr_catalog_plugin *plugin;
    char *file;
    int specific;
} claim;

typedef struct {
    claim *items;
    size_t count;
} claim_list;

static void free_claims(claim_list *claims) {
    for (size_t index = 0; index < claims->count; index++) free(claims->items[index].file);
    free(claims->items);
}

static int add_claim(claim_list *claims, const fr_catalog_plugin *plugin, const char *file,
                     int specific) {
    for (size_t index = 0; index < claims->count; index++) {
        claim *existing = &claims->items[index];
        if (existing->plugin == plugin && strcmp(existing->file, file) == 0) {
            if (specific) existing->specific = 1;
            return FR_OK;
        }
    }
    claim *grown = realloc(claims->items, (claims->count + 1) * sizeof *grown);
    if (grown == NULL) return FR_ERR;
    claims->items = grown;
    char *copy = fr_detect_copy(file, strlen(file));
    if (copy == NULL) return FR_ERR;
    grown[claims->count].plugin = plugin;
    grown[claims->count].file = copy;
    grown[claims->count].specific = specific;
    claims->count++;
    return FR_OK;
}

typedef struct {
    const char *name;
    int found_regular;
} probe;

static void probe_entry(const char *name, int is_directory, int is_link, void *context) {
    probe *state = context;
    if (!is_directory && !is_link && strcmp(name, state->name) == 0) state->found_regular = 1;
}

static int is_regular_file(const char *path) {
    const char *separator = strrchr(path, '/');
    if (separator == NULL) return 0;
    char *parent = fr_detect_copy(path, (size_t) (separator - path));
    if (parent == NULL) return 0;
    probe state = { separator + 1, 0 };
    fr_list_directory(parent, probe_entry, &state);
    free(parent);
    return state.found_regular;
}

static int is_small_enough(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) return 0;
    int fits = fseek(file, 0, SEEK_END) == 0 && ftell(file) <= MAX_CONTENT_BYTES;
    fclose(file);
    return fits;
}

/* 1 on a match, 0 on none, -1 with err set on a malformed pattern. string.find
   with plain = false is Lua's own matcher, which is what a catalog author tested
   against; the call is protected so a bad pattern cannot abort the run. */
static int pattern_finds(lua_State *state, const char *text, size_t length, const char *pattern,
                         fr_error *err) {
    lua_getglobal(state, LUA_STRLIBNAME);
    lua_getfield(state, -1, "find");
    lua_remove(state, -2);
    lua_pushlstring(state, text, length);
    lua_pushstring(state, pattern);
    if (lua_pcall(state, 2, 1, 0) != LUA_OK) {
        fr_error_set(err, "%s", lua_tostring(state, -1));
        lua_pop(state, 1);
        return -1;
    }
    int found = !lua_isnil(state, -1);
    lua_pop(state, 1);
    return found;
}

static int any_pattern_finds(fr_detect_run *run, const fr_catalog_plugin *plugin,
                             const fr_detect_rule *rule, const char *text, size_t length,
                             fr_error *err) {
    for (size_t index = 0; index < rule->contains_count; index++) {
        fr_error inner;
        int found = pattern_finds(run->patterns, text, length, rule->contains[index], &inner);
        if (found < 0) {
            fr_error_set(err, "plugin %s has a malformed detect pattern \"%s\": %s", plugin->id,
                         rule->contains[index], inner.message);
            return -1;
        }
        if (found) return 1;
    }
    return 0;
}

/* 1 when the file satisfies the rule's contains patterns, 0 when not, -1 on
   error. A file over 1 MiB or one that cannot be read does not match. */
static int file_satisfies_contains(fr_detect_run *run, const fr_catalog_plugin *plugin,
                                   const fr_detect_rule *rule, const char *path, fr_error *err) {
    if (rule->contains_count == 0) return 1;
    if (!is_small_enough(path)) return 0;
    char *text = NULL;
    size_t length = 0;
    fr_error ignored;
    if (fr_file_read_bytes(path, &text, &length, &ignored) != FR_OK) return 0;
    int found = any_pattern_finds(run, plugin, rule, text, length, err);
    free(text);
    return found;
}

static int claim_file(fr_detect_run *run, const char *directory, const fr_catalog_plugin *plugin,
                      const fr_detect_rule *rule, const char *file, claim_list *claims,
                      fr_error *err) {
    char *path = fr_detect_join(directory, file);
    if (path == NULL) {
        fr_error_set(err, "out of memory while reading %s for detection", file);
        return FR_ERR;
    }
    int satisfied = file_satisfies_contains(run, plugin, rule, path, err);
    free(path);
    if (satisfied < 0) return FR_ERR;
    if (satisfied == 0) return FR_OK;
    if (add_claim(claims, plugin, file, rule->contains_count > 0) != FR_OK) {
        fr_error_set(err, "out of memory while recording a detection claim on %s", file);
        return FR_ERR;
    }
    return FR_OK;
}

static int claim_named_path(fr_detect_run *run, const char *directory,
                            const fr_catalog_plugin *plugin, const fr_detect_rule *rule,
                            const char *entry, claim_list *claims, fr_error *err) {
    char *path = fr_detect_join(directory, entry);
    if (path == NULL) {
        fr_error_set(err, "out of memory while reading %s for detection", entry);
        return FR_ERR;
    }
    int regular = is_regular_file(path);
    free(path);
    return regular ? claim_file(run, directory, plugin, rule, entry, claims, err) : FR_OK;
}

static int claim_matching_names(fr_detect_run *run, const char *directory,
                                const fr_catalog_plugin *plugin, const fr_detect_rule *rule,
                                const char *entry, const fr_detect_listing *listing,
                                claim_list *claims, fr_error *err) {
    for (size_t index = 0; index < listing->file_count; index++) {
        if (!fr_detect_name_matches(entry, listing->files[index])) continue;
        if (claim_file(run, directory, plugin, rule, listing->files[index], claims, err) != FR_OK) {
            return FR_ERR;
        }
    }
    return FR_OK;
}

static int claim_rule(fr_detect_run *run, const char *directory, const fr_catalog_plugin *plugin,
                      const fr_detect_rule *rule, const fr_detect_listing *listing,
                      claim_list *claims, fr_error *err) {
    for (size_t index = 0; index < rule->file_count; index++) {
        const char *entry = rule->files[index];
        int rc = strchr(entry, '/') != NULL
                     ? claim_named_path(run, directory, plugin, rule, entry, claims, err)
                     : claim_matching_names(run, directory, plugin, rule, entry, listing, claims,
                                            err);
        if (rc != FR_OK) return rc;
    }
    return FR_OK;
}

static int rule_admits(const fr_detect_rule *rule, const char *relative_dir) {
    return rule->scope == FR_DETECT_TREE || relative_dir[0] == '\0';
}

static int collect_claims(fr_detect_run *run, const char *directory, const char *relative_dir,
                          const fr_detect_listing *listing, claim_list *claims, fr_error *err) {
    for (size_t plugin_index = 0; plugin_index < run->catalog->count; plugin_index++) {
        const fr_catalog_plugin *plugin = &run->catalog->plugins[plugin_index];
        for (size_t rule_index = 0; rule_index < plugin->rule_count; rule_index++) {
            const fr_detect_rule *rule = &plugin->rules[rule_index];
            if (!rule_admits(rule, relative_dir)) continue;
            if (claim_rule(run, directory, plugin, rule, listing, claims, err) != FR_OK) {
                return FR_ERR;
            }
        }
    }
    return FR_OK;
}

static int first_claim_of_file(const claim_list *claims, size_t index) {
    for (size_t earlier = 0; earlier < index; earlier++) {
        if (strcmp(claims->items[earlier].file, claims->items[index].file) == 0) return 0;
    }
    return 1;
}

static int outranks(const claim *left, const claim *right) {
    if (left->specific != right->specific) return left->specific > right->specific;
    return left->plugin->catalog_index < right->plugin->catalog_index;
}

static int ties_with(const claim *left, const claim *right) {
    return left->specific == right->specific &&
           left->plugin->catalog_index == right->plugin->catalog_index;
}

static const claim *best_claim_of_file(const claim_list *claims, const char *file) {
    const claim *best = NULL;
    for (size_t index = 0; index < claims->count; index++) {
        const claim *candidate = &claims->items[index];
        if (strcmp(candidate->file, file) != 0) continue;
        if (best == NULL || outranks(candidate, best)) best = candidate;
    }
    return best;
}

static void describe_tie(const claim_list *claims, const claim *best, const char *relative_dir,
                         fr_error *err) {
    fr_strbuf names;
    fr_strbuf_init(&names);
    for (size_t index = 0; index < claims->count; index++) {
        const claim *candidate = &claims->items[index];
        if (strcmp(candidate->file, best->file) != 0 || !ties_with(candidate, best)) continue;
        if (names.length > 0) fr_strbuf_append(&names, ", ");
        fr_strbuf_append(&names, candidate->plugin->id);
    }
    char *joined = fr_strbuf_release(&names);
    fr_error_set(err, "%s/%s is claimed equally by %s; pass --with <id> to choose",
                 relative_dir[0] == '\0' ? "the project root" : relative_dir, best->file,
                 joined != NULL ? joined : "several plugins");
    free(joined);
}

static size_t count_tied(const claim_list *claims, const claim *best) {
    size_t tied = 0;
    for (size_t index = 0; index < claims->count; index++) {
        const claim *candidate = &claims->items[index];
        if (strcmp(candidate->file, best->file) == 0 && ties_with(candidate, best)) tied++;
    }
    return tied;
}

static int record_winners(fr_detect_run *run, const claim_list *claims, const claim *best,
                          const char *relative_dir, fr_error *err) {
    int is_tie = count_tied(claims, best) > 1;
    size_t recorded = 0;
    for (size_t index = 0; index < claims->count; index++) {
        const claim *candidate = &claims->items[index];
        if (strcmp(candidate->file, best->file) != 0 || !ties_with(candidate, best)) continue;
        if (is_tie && !fr_detect_is_with(run, candidate->plugin)) continue;
        if (fr_detections_add(run->found, candidate->plugin, relative_dir, err) != FR_OK) {
            return FR_ERR;
        }
        recorded++;
    }
    if (recorded > 0) return FR_OK;
    describe_tie(claims, best, relative_dir, err);
    return FR_ERR;
}

static int settle_claims(fr_detect_run *run, const claim_list *claims, const char *relative_dir,
                         fr_error *err) {
    for (size_t index = 0; index < claims->count; index++) {
        if (!first_claim_of_file(claims, index)) continue;
        const claim *best = best_claim_of_file(claims, claims->items[index].file);
        if (record_winners(run, claims, best, relative_dir, err) != FR_OK) return FR_ERR;
    }
    return FR_OK;
}

int fr_detect_directory(fr_detect_run *run, const char *directory, const char *relative_dir,
                        const fr_detect_listing *listing, fr_error *err) {
    claim_list claims = { 0 };
    int rc = collect_claims(run, directory, relative_dir, listing, &claims, err);
    if (rc == FR_OK) rc = settle_claims(run, &claims, relative_dir, err);
    free_claims(&claims);
    return rc;
}
