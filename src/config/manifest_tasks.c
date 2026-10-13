#include "config/manifest_internal.h"

#include "config/jsonx.h"
#include "exec/tool.h"
#include "util/error.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void fr_manifest_task_command_free(fr_task_command *command) {
    if (command == NULL) return;
    free(command->tool);
    for (size_t index = 0; index < command->arg_count; index++) free(command->args[index]);
    free(command->args);
    free(command->cwd);
    free(command);
}

static const char *const RUN_KEYS[] = { "tool", "args", "cwd" };

static int run_key_is_known(const char *key) {
    for (size_t index = 0; index < sizeof RUN_KEYS / sizeof RUN_KEYS[0]; index++) {
        if (strcmp(RUN_KEYS[index], key) == 0) return 1;
    }
    return 0;
}

static int read_run_args(const cJSON *args, const char *path, fr_task_command *out,
                         fr_error *err) {
    if (!fr_json_is_array_or_empty_table(args)) {
        fr_error_set(err, "%s.run.args must be an array", path);
        return FR_ERR;
    }
    int size = cJSON_GetArraySize(args);
    if (size > 0) {
        out->args = calloc((size_t) size, sizeof *out->args);
        if (out->args == NULL) {
            fr_error_set(err, "out of memory reading %s.run.args", path);
            return FR_ERR;
        }
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, args) {
        if (!cJSON_IsString(item)) {
            fr_error_set(err, "%s.run.args must hold strings", path);
            return FR_ERR;
        }
        out->args[out->arg_count] = fr_manifest_duplicate(item->valuestring);
        if (out->args[out->arg_count] == NULL) {
            fr_error_set(err, "out of memory reading %s.run.args", path);
            return FR_ERR;
        }
        out->arg_count++;
    }
    return FR_OK;
}

/* The escape hatch D-53 specifies. Every key is refused by name, which the
   task block itself already does and which is what makes adding one here
   safe: a key accepted and ignored is the defect D-44 and D-49 were raised
   for. */
static int read_run(const cJSON *entry, const char *path, fr_task_command *out, fr_error *err) {
    if (!cJSON_IsObject(entry)) {
        fr_error_set(err, "%s.run must be a table", path);
        return FR_ERR;
    }
    for (const cJSON *member = entry->child; member != NULL; member = member->next) {
        if (!run_key_is_known(member->string)) {
            fr_error_set(err, "%s.run: \"%s\" is not a key run defines; only tool, args and cwd"
                              " are", path, member->string);
            return FR_ERR;
        }
    }

    const cJSON *tool = cJSON_GetObjectItemCaseSensitive(entry, "tool");
    if (tool == NULL || !cJSON_IsString(tool)) {
        fr_error_set(err, "%s.run.tool must be a string naming a program", path);
        return FR_ERR;
    }
    if (!fr_tool_name_is_valid(tool->valuestring)) {
        fr_error_set(err, FR_TOOL_NOT_A_NAME_REFUSAL, tool->valuestring);
        return FR_ERR;
    }
    out->tool = fr_manifest_duplicate(tool->valuestring);
    if (out->tool == NULL) {
        fr_error_set(err, "out of memory reading %s.run.tool", path);
        return FR_ERR;
    }

    const cJSON *cwd = cJSON_GetObjectItemCaseSensitive(entry, "cwd");
    if (cwd != NULL) {
        if (!cJSON_IsString(cwd)) {
            fr_error_set(err, "%s.run.cwd must be a string", path);
            return FR_ERR;
        }
        out->cwd = fr_manifest_duplicate(cwd->valuestring);
        if (out->cwd == NULL) {
            fr_error_set(err, "out of memory reading %s.run.cwd", path);
            return FR_ERR;
        }
    }

    const cJSON *args = cJSON_GetObjectItemCaseSensitive(entry, "args");
    if (args == NULL) return FR_OK;
    return read_run_args(args, path, out, err);
}

int fr_manifest_read_task(const cJSON *entry, const char *name, fr_task *out, fr_error *err) {
    memset(out, 0, sizeof *out);
    char path[320];
    snprintf(path, sizeof path, "tasks.%s", name);

    out->name = fr_manifest_duplicate(name);
    if (out->name == NULL) {
        fr_error_set(err, "out of memory reading %s", path);
        return FR_ERR;
    }
    if (!cJSON_IsObject(entry)) {
        fr_error_set(err, "%s must be a table", path);
        return FR_ERR;
    }

    const cJSON *member = entry->child;
    while (member != NULL) {
        if (strcmp(member->string, "dependsOn") != 0 && strcmp(member->string, "partOf") != 0
            && strcmp(member->string, "run") != 0) {
            fr_error_set(err, "%s: \"%s\" is not a key a task block defines; only dependsOn,"
                              " partOf and run are", path, member->string);
            return FR_ERR;
        }
        member = member->next;
    }

    const cJSON *run = cJSON_GetObjectItemCaseSensitive(entry, "run");
    if (run != NULL) {
        out->run = calloc(1, sizeof *out->run);
        if (out->run == NULL) {
            fr_error_set(err, "out of memory reading %s.run", path);
            return FR_ERR;
        }
        if (read_run(run, path, out->run, err) != FR_OK) return FR_ERR;
    }

    const cJSON *part_of = cJSON_GetObjectItemCaseSensitive(entry, "partOf");
    if (part_of != NULL) {
        if (!cJSON_IsString(part_of)) {
            fr_error_set(err, "%s.partOf must be a string", path);
            return FR_ERR;
        }
        out->part_of = fr_manifest_duplicate(part_of->valuestring);
        if (out->part_of == NULL) {
            fr_error_set(err, "out of memory reading %s.partOf", path);
            return FR_ERR;
        }
    }

    const cJSON *depends = cJSON_GetObjectItemCaseSensitive(entry, "dependsOn");
    if (depends == NULL) return FR_OK;
    if (!fr_json_is_array_or_empty_table(depends)) {
        fr_error_set(err, "%s.dependsOn must be an array", path);
        return FR_ERR;
    }
    int size = cJSON_GetArraySize(depends);
    if (size > 0) {
        out->depends_on = calloc((size_t) size, sizeof *out->depends_on);
        if (out->depends_on == NULL) {
            fr_error_set(err, "out of memory reading %s.dependsOn", path);
            return FR_ERR;
        }
    }
    size_t index = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, depends) {
        if (!cJSON_IsString(item)) {
            fr_error_set(err, "%s.dependsOn must hold strings", path);
            return FR_ERR;
        }
        out->depends_on[index] = fr_manifest_duplicate(item->valuestring);
        if (out->depends_on[index] == NULL) {
            fr_error_set(err, "out of memory reading %s.dependsOn", path);
            return FR_ERR;
        }
        out->depends_on_count = ++index;
    }
    return FR_OK;
}
