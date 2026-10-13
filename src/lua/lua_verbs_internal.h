#ifndef DAUKLE_LUA_VERBS_INTERNAL_H
#define DAUKLE_LUA_VERBS_INTERNAL_H

/* The seam between lua_verbs.c, which builds the per-plugin environment and
   installs the verbs, and the files that each implement a family of them.
   Everything that crosses between those files does so through this one. */

#define FR_TOOL_HANDLE "daukle.tool"

#define FR_TOOL_NAME_MAX 128

typedef struct {
    char path[1024];
    char name[FR_TOOL_NAME_MAX];
} fr_lua_tool;

#define FR_PROVISION_HANDLE "daukle.provision.root"

/* Sized like fr_provision_result.root, so taking that root cannot truncate. */
#define FR_PROVISION_ROOT_MAX 1024

typedef struct {
    char root[FR_PROVISION_ROOT_MAX];
} fr_lua_root;

int fr_lua_verbs_is_verbose(void);
int fr_lua_verbs_is_resolving(void);

#endif
