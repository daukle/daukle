#include "project/task_command.h"

#include "exec/exec.h"
#include "exec/tool.h"
#include "exec/toolreport.h"
#include "lua/lua_sandbox.h"
#include "lua/lua_verbs.h"
#include "util/error.h"

#include <stdlib.h>
#include <string.h>

static int resolve_working_directory(const fr_task_command *command, const char *project_dir,
                                     char **out, fr_error *err) {
    return fr_lua_sandbox_resolve_dir_in(project_dir, command->cwd != NULL ? command->cwd : ".",
                                         out, err);
}

int fr_task_command_run(const fr_task_command *command, const char *project_dir,
                        const char *task_name, fr_error *err) {
    char *program = NULL;
    if (fr_tool_resolve(command->tool, &program, err) != FR_OK) return FR_ERR;
    fr_toolreport_used_installed(command->tool, NULL, program);

    char *working_directory = NULL;
    if (resolve_working_directory(command, project_dir, &working_directory, err) != FR_OK) {
        free(program);
        return FR_ERR;
    }

    fr_exec_request request;
    memset(&request, 0, sizeof request);
    request.program = program;
    request.argv = (const char *const *) command->args;
    request.argv_count = command->arg_count;
    request.cwd = working_directory;

    /* Both of them, and not fr_lua_verbs_env_to_scrub's answer: that one
       filters by what the running chunk declared, and a manifest task runs
       outside every chunk, where the declared list is whatever the last
       plugin left behind. A manifest declares no environment at all. */
    const char *scrub[FR_LUA_VERBS_MAX_SCRUB];
    request.scrub_count = fr_lua_verbs_core_credentials(scrub, FR_LUA_VERBS_MAX_SCRUB);
    request.scrub = request.scrub_count > 0 ? scrub : NULL;

    fr_exec_result result;
    int started = fr_exec_run(&request, &result, err);
    free(working_directory);
    free(program);
    if (started != FR_OK) return FR_ERR;

    int code = result.code;
    fr_exec_result_free(&result);
    if (code != 0) {
        fr_error_set(err, "task \"%s\": %s exited with code %d", task_name, command->tool, code);
        return FR_ERR;
    }
    return FR_OK;
}
