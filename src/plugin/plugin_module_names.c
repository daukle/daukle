#include "plugin/plugin_modules.h"

#include "archive/tar.h"
#include "lua/lua_sandbox.h"
#include "util/error.h"

#include <string.h>

static int ends_with_lua(const char *name) {
    size_t length = strlen(name);
    return length > 4 && strcmp(name + length - 4, ".lua") == 0;
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
