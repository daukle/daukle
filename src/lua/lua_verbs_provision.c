#include "lua/lua_verbs.h"
#include "lua/lua_verbs_internal.h"

#include "config/config_lua.h"
#include "exec/toolreport.h"
#include "lua/lua_sandbox.h"
#include "lua/luax.h"
#include "net/http.h"
#include "plugin/registry.h"
#include "provision/provision.h"
#include "util/error.h"

#include "lauxlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FR_PROVISION_MAX_HEADERS 8

static const char *const PROVISION_FIELDS[] = { "url", "sha256", "as", "headers" };

static int provision_field_is_known(const char *key) {
    for (size_t index = 0; index < sizeof PROVISION_FIELDS / sizeof PROVISION_FIELDS[0]; index++) {
        if (strcmp(PROVISION_FIELDS[index], key) == 0) return 1;
    }
    return 0;
}

/* Refused by name rather than ignored: a silently dropped version = "21" is a
   user believing they pinned something. The message names whichever key the
   walk reached and never a fixed one, because lua_next's order over a plugin's
   table is unspecified, so with two unknown keys either may be the one seen. */
static int refuse_an_unknown_provision_field(lua_State *state, const char *verb) {
    lua_pushnil(state);
    while (lua_next(state, 1) != 0) {
        if (lua_type(state, -2) != LUA_TSTRING) {
            return luaL_error(state, "%s takes named fields only", verb);
        }
        const char *key = lua_tostring(state, -2);
        if (!provision_field_is_known(key)) {
            return luaL_error(state, "%s does not take \"%s\"; it takes url, sha256"
                                     ", as and headers", verb, key);
        }
        lua_pop(state, 1);
    }
    return 0;
}

/* Raw, never lua_getfield: setmetatable is a base global a plugin keeps, so a
   field answered by an __index could hand one digest to the check here and a
   different one to the fetch, and the digest read IS the whole enforcement of
   the pin. The value stays on the stack, since the answer points into it. */
static const char *provision_field(lua_State *state, const char *key) {
    lua_pushstring(state, key);
    lua_rawget(state, 1);
    return lua_type(state, -1) == LUA_TSTRING ? lua_tostring(state, -1) : NULL;
}

/* One "%s" and no other specifier. The template exists so that "Bearer " is not
   written into core, and it is one specifier precisely so it cannot become a
   format string a plugin controls. */
static int format_takes_one_string(const char *format) {
    int seen = 0;
    for (const char *scan = format; *scan != 0; scan++) {
        if (*scan != 37) continue;
        scan++;
        if (*scan == 37) continue;
        if (*scan != 115) return 0;
        seen++;
    }
    return seen == 1;
}

/* Reads the "headers" table, resolving each value from the environment. A value
   is a table naming a variable, never a string: a literal would make a plugin,
   and the manifest that pins it, places a secret ends up, and both are
   committed. The variable must be one this plugin declared, so one allowlist
   governs reading and fetching rather than two. An unset variable omits the
   header rather than sending it empty, which is what plugins/github.lua already
   does: an empty Bearer is a worse diagnostic than an anonymous request. D-32. */
static size_t read_provision_headers(lua_State *state, fr_http_header *out) {
    lua_pushstring(state, "headers");
    lua_rawget(state, 1);
    if (lua_isnil(state, -1)) return 0;
    if (!lua_istable(state, -1)) {
        luaL_error(state, "daukle.provision field \"headers\" must be a table");
    }

    int headers_index = lua_gettop(state);
    size_t count = 0;
    lua_pushnil(state);
    while (lua_next(state, headers_index) != 0) {
        if (lua_type(state, -2) != LUA_TSTRING) {
            luaL_error(state, "daukle.provision: every header name must be a string");
        }
        const char *name = lua_tostring(state, -2);
        if (lua_type(state, -1) == LUA_TSTRING) {
            luaL_error(state, "daukle.provision: headers.%s must name an environment"
                              " variable, not a value", name);
        }
        if (!lua_istable(state, -1)) {
            luaL_error(state, "daukle.provision: headers.%s must be a table naming env", name);
        }
        if (count == FR_PROVISION_MAX_HEADERS) {
            luaL_error(state, "daukle.provision: more than %d headers",
                       FR_PROVISION_MAX_HEADERS);
        }

        lua_pushstring(state, "env");
        lua_rawget(state, -2);
        const char *variable = lua_type(state, -1) == LUA_TSTRING
                             ? lua_tostring(state, -1) : NULL;
        if (variable == NULL) {
            luaL_error(state, "daukle.provision: headers.%s needs an env string", name);
        }
        if (fr_lua_verbs_is_core_credential(variable)
            && !fr_lua_verbs_env_is_declared(variable)) {
            luaL_error(state, "daukle.provision: headers.%s names %s, which this plugin did"
                              " not declare in env", name, variable);
        }

        lua_pushstring(state, "format");
        lua_rawget(state, -3);
        const char *format = lua_type(state, -1) == LUA_TSTRING
                           ? lua_tostring(state, -1) : NULL;
        if (format != NULL && !format_takes_one_string(format)) {
            luaL_error(state, "daukle.provision: headers.%s format takes exactly one %%s", name);
        }

        const char *value = getenv(variable);
        if (value != NULL && value[0] != 0) {
            char *rendered = NULL;
            if (format != NULL) {
                size_t length = strlen(format) + strlen(value) + 1;
                rendered = malloc(length);
                if (rendered != NULL) snprintf(rendered, length, format, value);
            } else {
                rendered = fr_dup_string(value);
            }
            char *header_name = fr_dup_string(name);
            if (rendered == NULL || header_name == NULL) {
                free(rendered);
                free(header_name);
                luaL_error(state, "out of memory building the %s header", name);
            }
            out[count].name = header_name;
            out[count].value = rendered;
            count++;
        }

        lua_pop(state, 3);
    }
    return count;
}

static void free_provision_headers(fr_http_header *headers, size_t count) {
    for (size_t index = 0; index < count; index++) {
        free((char *) headers[index].name);
        free((char *) headers[index].value);
    }
}

int fr_lua_verb_provision(lua_State *state) {
    if (fr_lua_generation_is_running()) {
        return luaL_error(state, "daukle.provision is not available while generating; "
                                 "generation is a pure function of the manifest");
    }
    /* The per-kind refusals in config_lua fire when daukle.language and its
       siblings are CALLED, by which time a chunk that provisioned at its top
       level has already put a tree on disk during `daukle check`. The refusal
       has to reach the call, exactly as daukle.exec's does. */
    if (fr_lua_plugin_exec_is_refused()) {
        return luaL_error(state, "daukle.provision is not available while a plugin chunk is"
                                 " loading; call it from a task or publish callback");
    }
    luaL_checktype(state, 1, LUA_TTABLE);
    refuse_an_unknown_provision_field(state, "daukle.provision");

    const char *url = provision_field(state, "url");
    if (url == NULL) return luaL_error(state, "daukle.provision needs a url string");

    const char *digest = provision_field(state, "sha256");
    if (digest == NULL) {
        return luaL_error(state, "daukle.provision needs a sha256 string: there is no unpinned"
                                 " form of it and no flag that relaxes one");
    }

    const char *label = provision_field(state, "as");
    if (label == NULL && !lua_isnil(state, -1)) {
        return luaL_error(state, "daukle.provision field \"as\" must be a string");
    }
    if (label != NULL && !fr_toolreport_label_is_safe(label)) {
        return luaL_error(state, "daukle.provision field \"as\" may not hold a control character");
    }

    fr_http_header headers[FR_PROVISION_MAX_HEADERS];
    memset(headers, 0, sizeof headers);
    size_t header_count = read_provision_headers(state, headers);

    fr_provision_result result;
    fr_error err;
    int provisioned = fr_provision(url, digest, header_count > 0 ? headers : NULL,
                                   header_count, &result, &err);
    free_provision_headers(headers, header_count);
    if (provisioned != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }
    fr_toolreport_provisioned(label, url, digest, result.was_cached);
    fr_toolreport_links(digest, result.unpack.symlinks_copied, result.unpack.symlinks_unresolved,
                        result.unpack.first_symlink_unresolved);

    fr_lua_root *handle = lua_newuserdatauv(state, sizeof *handle, 0);
    snprintf(handle->root, sizeof handle->root, "%s", result.root);
    luaL_getmetatable(state, FR_PROVISION_HANDLE);
    lua_setmetatable(state, -2);
    return 1;
}

/* Returns a path string and not a handle, which is the opposite of root:tool and
   is deliberate: a tool handle is opaque so that daukle.exec can refuse an
   arbitrary string, and an artifact is never executed, only named to a tool as
   an argument. This widens what a plugin may NAME, exactly as root:path did,
   and not what it may RUN. */
int fr_lua_verb_artifact(lua_State *state) {
    if (fr_lua_generation_is_running()) {
        return luaL_error(state, "daukle.artifact is not available while generating; "
                                 "generation is a pure function of the manifest");
    }
    if (fr_lua_plugin_exec_is_refused()) {
        return luaL_error(state, "daukle.artifact is not available while a plugin chunk is"
                                 " loading; call it from a task or publish callback");
    }
    luaL_checktype(state, 1, LUA_TTABLE);
    refuse_an_unknown_provision_field(state, "daukle.artifact");

    const char *url = provision_field(state, "url");
    if (url == NULL) return luaL_error(state, "daukle.artifact needs a url string");

    const char *digest = provision_field(state, "sha256");
    if (digest == NULL) {
        return luaL_error(state, "daukle.artifact needs a sha256 string: there is no unpinned"
                                 " form of it and no flag that relaxes one");
    }

    const char *label = provision_field(state, "as");
    if (label == NULL && !lua_isnil(state, -1)) {
        return luaL_error(state, "daukle.artifact field \"as\" must be a string");
    }
    if (label != NULL && !fr_toolreport_label_is_safe(label)) {
        return luaL_error(state, "daukle.artifact field \"as\" may not hold a control character");
    }

    fr_http_header headers[FR_PROVISION_MAX_HEADERS];
    memset(headers, 0, sizeof headers);
    size_t header_count = read_provision_headers(state, headers);

    fr_artifact_result result;
    fr_error err;
    int fetched = fr_artifact(url, digest, header_count > 0 ? headers : NULL, header_count,
                              &result, &err);
    free_provision_headers(headers, header_count);
    if (fetched != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }
    fr_toolreport_artifact(label, url, digest, result.was_cached);

    lua_pushstring(state, result.path);
    return 1;
}

/* daukle.pin takes no sha256 and the message says so rather than listing the
   fields, because offering one is a misunderstanding of what the verb is for
   and the list alone would not correct it. */
static int refuse_an_unknown_pin_field(lua_State *state) {
    lua_pushnil(state);
    while (lua_next(state, 1) != 0) {
        if (lua_type(state, -2) != LUA_TSTRING) {
            return luaL_error(state, "daukle.pin takes named fields only");
        }
        const char *key = lua_tostring(state, -2);
        if (strcmp(key, "sha256") == 0) {
            return luaL_error(state, "daukle.pin does not take \"sha256\": the digest is"
                                     " what it returns. Use daukle.artifact for bytes you"
                                     " already have a pin for");
        }
        if (strcmp(key, "url") != 0 && strcmp(key, "as") != 0 && strcmp(key, "headers") != 0) {
            return luaL_error(state, "daukle.pin does not take \"%s\"; it takes url, as"
                                     " and headers", key);
        }
        lua_pop(state, 1);
    }
    return 0;
}

int fr_lua_verb_pin(lua_State *state) {
    if (!fr_lua_verbs_is_resolving()) {
        return luaL_error(state, "daukle.pin is available only in a resolve run: pass --resolve."
                                 " An ordinary build fetches nothing it has no sha256 for");
    }
    if (fr_lua_generation_is_running()) {
        return luaL_error(state, "daukle.pin is not available while generating; "
                                 "generation is a pure function of the manifest");
    }
    if (fr_lua_plugin_exec_is_refused()) {
        return luaL_error(state, "daukle.pin is not available while a plugin chunk is"
                                 " loading; call it from a task or publish callback");
    }
    luaL_checktype(state, 1, LUA_TTABLE);
    refuse_an_unknown_pin_field(state);

    const char *url = provision_field(state, "url");
    if (url == NULL) return luaL_error(state, "daukle.pin needs a url string");

    const char *label = provision_field(state, "as");
    if (label == NULL && !lua_isnil(state, -1)) {
        return luaL_error(state, "daukle.pin field \"as\" must be a string");
    }
    if (label != NULL && !fr_toolreport_label_is_safe(label)) {
        return luaL_error(state, "daukle.pin field \"as\" may not hold a control"
                                 " character");
    }

    fr_http_header headers[FR_PROVISION_MAX_HEADERS];
    memset(headers, 0, sizeof headers);
    size_t header_count = read_provision_headers(state, headers);

    fr_pin_result result;
    fr_error err;
    int fetched = fr_artifact_pin(url, header_count > 0 ? headers : NULL, header_count, &result,
                                  &err);
    free_provision_headers(headers, header_count);
    if (fetched != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }
    fr_toolreport_artifact(label, url, result.sha256, 0);

    lua_newtable(state);
    lua_pushstring(state, result.path);
    lua_setfield(state, -2, "path");
    lua_pushstring(state, result.sha256);
    lua_setfield(state, -2, "sha256");
    return 1;
}
