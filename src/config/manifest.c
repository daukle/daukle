#include "config/manifest.h"

#include "config/jsonx.h"
#include "exec/tool.h"
#include "plugin/plugins.h"
#include "project/region.h"
#include "util/error.h"
#include "util/semver.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>


static char *duplicate(const char *text) {
    if (text == NULL) return NULL;
    size_t length = strlen(text) + 1;
    char *copy = malloc(length);
    if (copy != NULL) memcpy(copy, text, length);
    return copy;
}

static void module_free(fr_module *module) {
    if (module == NULL) return;
    free(module->name);
    fr_string_array_free(module->requires, module->requires_count);
}

static void source_free(fr_source *source) {
    if (source == NULL) return;
    free(source->project);
    free(source->kind);
}

static void dependency_free(fr_dependency *dependency) {
    if (dependency == NULL) return;
    free(dependency->project);
    fr_string_array_free(dependency->modules, dependency->module_count);
}

static void consumer_free(fr_consumer *consumer) {
    if (consumer == NULL) return;
    free(consumer->id);
    free(consumer->language);
    free(consumer->file);
    free(consumer->configuration);
    for (size_t index = 0; index < consumer->dependency_count; index++) {
        dependency_free(&consumer->dependencies[index]);
    }
    free(consumer->dependencies);
}

static int check_schema(const cJSON *root, const char *file_path, fr_error *err) {
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    if (!cJSON_IsNumber(schema) || schema->valueint != FR_SCHEMA) {
        fr_error_set(err, "\"%s\" declares an unsupported schema, expected %d", file_path, FR_SCHEMA);
        return FR_ERR;
    }
    return FR_OK;
}

/* A module entry carries its language blocks beside the keys the core reads
   from that same entry, so a language may not be named after one: the lookup
   would find the core's own value instead of a language block. */
static const char *RESERVED_MODULE_KEYS[] = { "requires" };

static int is_reserved_module_key(const char *name) {
    size_t count = sizeof RESERVED_MODULE_KEYS / sizeof RESERVED_MODULE_KEYS[0];
    for (size_t index = 0; index < count; index++) {
        if (strcmp(RESERVED_MODULE_KEYS[index], name) == 0) return 1;
    }
    return 0;
}

static int read_module(const cJSON *entry, const char *name, fr_module *out, fr_error *err) {
    memset(out, 0, sizeof *out);
    char path[256];
    snprintf(path, sizeof path, "modules.%s", name);

    out->name = duplicate(name);
    if (out->name == NULL) {
        fr_error_set(err, "out of memory reading %s", path);
        return FR_ERR;
    }
    if (fr_json_array_of_strings(entry, "requires", path, &out->requires, &out->requires_count, err) != FR_OK) {
        return FR_ERR;
    }
    out->blocks = entry;
    return FR_OK;
}

/* Every name the manifest vocabulary defines at the root, whether it is a key
   or a section. A name outside this list is refused rather than discarded,
   because a discarded one is a user believing they configured something:
   "[toolchain.java]" for "[toolchains.java]" used to sync clean, with no
   toolchain, no error and no warning. D-49. */
static const char *const MANIFEST_KEYS[] = {
    "schema", "project", "version", "modules", "sources", "consumers",
    "toolchains", "plugins", "publish", "resolvers", "tasks"
};

static int manifest_key_is_known(const char *name) {
    for (size_t index = 0; index < sizeof MANIFEST_KEYS / sizeof MANIFEST_KEYS[0]; index++) {
        if (strcmp(MANIFEST_KEYS[index], name) == 0) return 1;
    }
    return 0;
}

/* Answers only the mistake people actually make, a missing or surplus plural,
   rather than searching by edit distance: every section name here is plural,
   and the defect this was raised for is "toolchain" for "toolchains". A wrong
   suggestion costs more than none, so the rule stays one a reader can check. */
static const char *a_near_miss_for(const char *name) {
    size_t length = strlen(name);
    for (size_t index = 0; index < sizeof MANIFEST_KEYS / sizeof MANIFEST_KEYS[0]; index++) {
        const char *known = MANIFEST_KEYS[index];
        size_t known_length = strlen(known);
        if (known_length == length + 1 && known[known_length - 1] == 's'
            && strncmp(known, name, length) == 0) {
            return known;
        }
        if (length == known_length + 1 && name[length - 1] == 's'
            && strncmp(known, name, known_length) == 0) {
            return known;
        }
    }
    return NULL;
}

static int refuse_an_unknown_manifest_key(const cJSON *root, const char *file_path,
                                          fr_error *err) {
    for (const cJSON *member = root->child; member != NULL; member = member->next) {
        if (member->string == NULL || manifest_key_is_known(member->string)) continue;
        const char *near = a_near_miss_for(member->string);
        if (near != NULL) {
            fr_error_set(err, "\"%s\": \"%s\" is not a section daukle defines; did you mean"
                              " \"%s\"?", file_path, member->string, near);
        } else {
            fr_error_set(err, "\"%s\": \"%s\" is not a key or section daukle defines. It holds"
                              " schema, project, version, modules, sources, consumers,"
                              " toolchains, plugins, publish, resolvers and tasks",
                         file_path, member->string);
        }
        return FR_ERR;
    }
    return FR_OK;
}

static int project_from_json(const cJSON *root, const char *file_path, fr_project *out, fr_error *err) {
    memset(out, 0, sizeof *out);
    if (check_schema(root, file_path, err) != FR_OK) return FR_ERR;
    if (refuse_an_unknown_manifest_key(root, file_path, err) != FR_OK) return FR_ERR;

    const char *project_name = NULL;
    if (fr_json_string(root, "project", file_path, &project_name, err) != FR_OK) return FR_ERR;
    out->project = duplicate(project_name);
    if (out->project == NULL) {
        fr_error_set(err, "out of memory reading %s.project", file_path);
        return FR_ERR;
    }

    const char *version_text = NULL;
    if (fr_json_string(root, "version", file_path, &version_text, err) != FR_OK) return FR_ERR;
    if (fr_version_parse(version_text, &out->version, err) != FR_OK) return FR_ERR;

    const cJSON *modules_obj = NULL;
    if (fr_json_object(root, "modules", file_path, &modules_obj, err) != FR_OK) return FR_ERR;

    int size = cJSON_GetArraySize(modules_obj);
    if (size > 0) {
        out->modules = calloc((size_t) size, sizeof *out->modules);
        if (out->modules == NULL) {
            fr_error_set(err, "out of memory reading %s.modules", file_path);
            return FR_ERR;
        }
    }
    size_t index = 0;
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, modules_obj) {
        out->module_count = index + 1;
        if (read_module(entry, entry->string, &out->modules[index], err) != FR_OK) return FR_ERR;
        index++;
    }
    return FR_OK;
}

static int read_source(const cJSON *entry, const char *project_name, fr_source *out, fr_error *err) {
    memset(out, 0, sizeof *out);
    char path[256];
    snprintf(path, sizeof path, "sources.%s", project_name);

    const char *kind = NULL;
    if (fr_json_string(entry, "kind", path, &kind, err) != FR_OK) return FR_ERR;

    out->project = duplicate(project_name);
    out->kind = duplicate(kind);
    if (out->project == NULL || out->kind == NULL) {
        fr_error_set(err, "out of memory reading %s", path);
        return FR_ERR;
    }
    out->block = entry;
    return FR_OK;
}

static int read_dependency(const cJSON *entry, const char *project_name, const char *consumer_path,
                           fr_dependency *out, fr_error *err) {
    memset(out, 0, sizeof *out);
    char path[320];
    snprintf(path, sizeof path, "%s.dependencies.%s", consumer_path, project_name);

    out->project = duplicate(project_name);
    if (out->project == NULL) {
        fr_error_set(err, "out of memory reading %s", path);
        return FR_ERR;
    }
    const char *version_text = NULL;
    if (fr_json_string(entry, "version", path, &version_text, err) != FR_OK) return FR_ERR;
    if (fr_range_parse(version_text, &out->range, err) != FR_OK) return FR_ERR;
    if (fr_json_array_of_strings(entry, "modules", path, &out->modules, &out->module_count, err) != FR_OK) {
        return FR_ERR;
    }
    return FR_OK;
}

static int read_consumer(const cJSON *entry, size_t consumer_index, fr_consumer *out, fr_error *err) {
    memset(out, 0, sizeof *out);
    char path[64];
    snprintf(path, sizeof path, "consumers[%zu]", consumer_index);

    const char *id = NULL;
    if (fr_json_string(entry, "id", path, &id, err) != FR_OK) return FR_ERR;
    out->id = duplicate(id);
    if (out->id == NULL) {
        fr_error_set(err, "out of memory reading %s.id", path);
        return FR_ERR;
    }

    const char *language = NULL;
    if (fr_json_string(entry, "language", path, &language, err) != FR_OK) return FR_ERR;
    if (is_reserved_module_key(language)) {
        fr_error_set(err, "%s.language \"%s\" is a reserved module key", path, language);
        return FR_ERR;
    }
    out->language = duplicate(language);
    if (out->language == NULL) {
        fr_error_set(err, "out of memory reading %s.language", path);
        return FR_ERR;
    }

    const char *file_name = NULL;
    if (fr_json_string(entry, "file", path, &file_name, err) != FR_OK) return FR_ERR;
    out->file = duplicate(file_name);
    if (out->file == NULL) {
        fr_error_set(err, "out of memory reading %s.file", path);
        return FR_ERR;
    }

    const char *configuration = NULL;
    if (fr_json_string(entry, "configuration", path, &configuration, err) != FR_OK) return FR_ERR;
    out->configuration = duplicate(configuration);
    if (out->configuration == NULL) {
        fr_error_set(err, "out of memory reading %s.configuration", path);
        return FR_ERR;
    }

    const cJSON *dependencies_obj = NULL;
    if (fr_json_object(entry, "dependencies", path, &dependencies_obj, err) != FR_OK) return FR_ERR;

    int size = cJSON_GetArraySize(dependencies_obj);
    if (size > 0) {
        out->dependencies = calloc((size_t) size, sizeof *out->dependencies);
        if (out->dependencies == NULL) {
            fr_error_set(err, "out of memory reading %s.dependencies", path);
            return FR_ERR;
        }
    }
    size_t index = 0;
    const cJSON *dependency_entry = NULL;
    cJSON_ArrayForEach(dependency_entry, dependencies_obj) {
        out->dependency_count = index + 1;
        if (read_dependency(dependency_entry, dependency_entry->string, path, &out->dependencies[index], err) != FR_OK) {
            return FR_ERR;
        }
        index++;
    }
    return FR_OK;
}

static int read_toolchain(const cJSON *entry, const char *name, fr_toolchain *out, fr_error *err) {
    memset(out, 0, sizeof *out);
    char path[320];
    snprintf(path, sizeof path, "toolchains.%s", name);

    if (strcmp(name, "publish") == 0) {
        fr_error_set(err, "%s may not be named \"publish\": that prefix is reserved for publish"
                          " destinations, so a \"publish:\" task would never reach this toolchain",
                     path);
        return FR_ERR;
    }

    out->name = duplicate(name);
    if (out->name == NULL) {
        fr_error_set(err, "out of memory reading %s", path);
        return FR_ERR;
    }

    if (cJSON_IsString(entry)) {
        out->version = duplicate(entry->valuestring);
        if (out->version == NULL) {
            fr_error_set(err, "out of memory reading %s", path);
            return FR_ERR;
        }
        return FR_OK;
    }
    if (!cJSON_IsObject(entry)) {
        fr_error_set(err, "%s must be a table or a version string", path);
        return FR_ERR;
    }
    out->block = entry;

    const cJSON *version = cJSON_GetObjectItemCaseSensitive(entry, "version");
    if (version != NULL) {
        if (!cJSON_IsString(version)) {
            fr_error_set(err, "%s.version must be a string", path);
            return FR_ERR;
        }
        out->version = duplicate(version->valuestring);
        if (out->version == NULL) {
            fr_error_set(err, "out of memory reading %s.version", path);
            return FR_ERR;
        }
    }

    const cJSON *dependencies_obj = cJSON_GetObjectItemCaseSensitive(entry, "dependencies");
    if (dependencies_obj == NULL) return FR_OK;
    if (!cJSON_IsObject(dependencies_obj)) {
        fr_error_set(err, "%s.dependencies must be a table", path);
        return FR_ERR;
    }
    int size = cJSON_GetArraySize(dependencies_obj);
    if (size > 0) {
        out->dependencies = calloc((size_t) size, sizeof *out->dependencies);
        if (out->dependencies == NULL) {
            fr_error_set(err, "out of memory reading %s.dependencies", path);
            return FR_ERR;
        }
    }
    size_t index = 0;
    const cJSON *dependency_entry = NULL;
    cJSON_ArrayForEach(dependency_entry, dependencies_obj) {
        out->dependency_count = index + 1;
        if (read_dependency(dependency_entry, dependency_entry->string, path,
                            &out->dependencies[index], err) != FR_OK) {
            return FR_ERR;
        }
        index++;
    }
    return FR_OK;
}

static int read_publish(const cJSON *entry, const char *name, fr_publish_target *out,
                        fr_error *err) {
    memset(out, 0, sizeof *out);
    char path[320];
    snprintf(path, sizeof path, "publish.%s", name);

    out->name = duplicate(name);
    if (out->name == NULL) {
        fr_error_set(err, "out of memory reading %s", path);
        return FR_ERR;
    }
    if (!cJSON_IsObject(entry)) {
        fr_error_set(err, "%s must be a table", path);
        return FR_ERR;
    }
    out->block = entry;

    const cJSON *from = cJSON_GetObjectItemCaseSensitive(entry, "from");
    if (from == NULL || !cJSON_IsString(from)) {
        fr_error_set(err, "%s needs a \"from\" string naming the toolchain whose output it"
                          " publishes", path);
        return FR_ERR;
    }
    out->from = duplicate(from->valuestring);
    if (out->from == NULL) {
        fr_error_set(err, "out of memory reading %s.from", path);
        return FR_ERR;
    }
    return FR_OK;
}

static int check_publish_targets(const fr_manifest *out, const char *file_path, fr_error *err) {
    for (size_t index = 0; index < out->publish_count; index++) {
        const fr_publish_target *target = &out->publishes[index];
        int declared = 0;
        for (size_t other = 0; other < out->toolchain_count; other++) {
            if (strcmp(out->toolchains[other].name, target->from) == 0) {
                declared = 1;
                break;
            }
        }
        if (!declared) {
            fr_error_set(err, "%s.publish.%s.from names \"%s\", but this manifest declares no"
                              " toolchain \"%s\"", file_path, target->name, target->from,
                              target->from);
            return FR_ERR;
        }
    }
    return FR_OK;
}

static void task_command_free(fr_task_command *command) {
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
        out->args[out->arg_count] = duplicate(item->valuestring);
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
    out->tool = duplicate(tool->valuestring);
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
        out->cwd = duplicate(cwd->valuestring);
        if (out->cwd == NULL) {
            fr_error_set(err, "out of memory reading %s.run.cwd", path);
            return FR_ERR;
        }
    }

    const cJSON *args = cJSON_GetObjectItemCaseSensitive(entry, "args");
    if (args == NULL) return FR_OK;
    return read_run_args(args, path, out, err);
}

static int read_task(const cJSON *entry, const char *name, fr_task *out, fr_error *err) {
    memset(out, 0, sizeof *out);
    char path[320];
    snprintf(path, sizeof path, "tasks.%s", name);

    out->name = duplicate(name);
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
        out->part_of = duplicate(part_of->valuestring);
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
        out->depends_on[index] = duplicate(item->valuestring);
        if (out->depends_on[index] == NULL) {
            fr_error_set(err, "out of memory reading %s.dependsOn", path);
            return FR_ERR;
        }
        out->depends_on_count = ++index;
    }
    return FR_OK;
}

static int manifest_from_json(const cJSON *root, const char *file_path, fr_manifest *out, fr_error *err) {
    if (project_from_json(root, file_path, &out->self, err) != FR_OK) return FR_ERR;

    const cJSON *sources_obj = cJSON_GetObjectItemCaseSensitive(root, "sources");
    if (sources_obj != NULL) {
        if (!cJSON_IsObject(sources_obj)) {
            fr_error_set(err, "%s.sources must be an object", file_path);
            return FR_ERR;
        }
        int size = cJSON_GetArraySize(sources_obj);
        if (size > 0) {
            out->sources = calloc((size_t) size, sizeof *out->sources);
            if (out->sources == NULL) {
                fr_error_set(err, "out of memory reading %s.sources", file_path);
                return FR_ERR;
            }
        }
        size_t index = 0;
        const cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, sources_obj) {
            out->source_count = index + 1;
            if (read_source(entry, entry->string, &out->sources[index], err) != FR_OK) return FR_ERR;
            index++;
        }
    }

    const cJSON *consumers_arr = cJSON_GetObjectItemCaseSensitive(root, "consumers");
    if (consumers_arr != NULL) {
        if (!fr_json_is_array_or_empty_table(consumers_arr)) {
            fr_error_set(err, "%s.consumers must be an array", file_path);
            return FR_ERR;
        }
        int size = cJSON_GetArraySize(consumers_arr);
        if (size > 0) {
            out->consumers = calloc((size_t) size, sizeof *out->consumers);
            if (out->consumers == NULL) {
                fr_error_set(err, "out of memory reading %s.consumers", file_path);
                return FR_ERR;
            }
        }
        size_t index = 0;
        const cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, consumers_arr) {
            out->consumer_count = index + 1;
            if (read_consumer(entry, index, &out->consumers[index], err) != FR_OK) return FR_ERR;
            index++;
        }
    }

    const cJSON *toolchains_obj = cJSON_GetObjectItemCaseSensitive(root, "toolchains");
    if (toolchains_obj != NULL) {
        if (!cJSON_IsObject(toolchains_obj)) {
            fr_error_set(err, "%s.toolchains must be a table", file_path);
            return FR_ERR;
        }
        int size = cJSON_GetArraySize(toolchains_obj);
        if (size > 0) {
            out->toolchains = calloc((size_t) size, sizeof *out->toolchains);
            if (out->toolchains == NULL) {
                fr_error_set(err, "out of memory reading %s.toolchains", file_path);
                return FR_ERR;
            }
        }
        size_t index = 0;
        const cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, toolchains_obj) {
            out->toolchain_count = index + 1;
            if (read_toolchain(entry, entry->string, &out->toolchains[index], err) != FR_OK) {
                return FR_ERR;
            }
            index++;
        }
    }

    const cJSON *publish_obj = cJSON_GetObjectItemCaseSensitive(root, "publish");
    if (publish_obj != NULL) {
        if (!cJSON_IsObject(publish_obj)) {
            fr_error_set(err, "%s.publish must be a table", file_path);
            return FR_ERR;
        }
        int size = cJSON_GetArraySize(publish_obj);
        if (size > 0) {
            out->publishes = calloc((size_t) size, sizeof *out->publishes);
            if (out->publishes == NULL) {
                fr_error_set(err, "out of memory reading %s.publish", file_path);
                return FR_ERR;
            }
        }
        size_t index = 0;
        const cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, publish_obj) {
            out->publish_count = index + 1;
            if (read_publish(entry, entry->string, &out->publishes[index], err) != FR_OK) {
                return FR_ERR;
            }
            index++;
        }
        if (check_publish_targets(out, file_path, err) != FR_OK) return FR_ERR;
    }

    const cJSON *tasks_obj = cJSON_GetObjectItemCaseSensitive(root, "tasks");
    if (tasks_obj != NULL) {
        if (!cJSON_IsObject(tasks_obj)) {
            fr_error_set(err, "%s.tasks must be a table", file_path);
            return FR_ERR;
        }
        int size = cJSON_GetArraySize(tasks_obj);
        if (size > 0) {
            out->tasks = calloc((size_t) size, sizeof *out->tasks);
            if (out->tasks == NULL) {
                fr_error_set(err, "out of memory reading %s.tasks", file_path);
                return FR_ERR;
            }
        }
        size_t index = 0;
        const cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, tasks_obj) {
            out->task_count = index + 1;
            if (read_task(entry, entry->string, &out->tasks[index], err) != FR_OK) return FR_ERR;
            index++;
        }
    }

    return FR_OK;
}

int fr_project_parse(const char *text, const char *origin, fr_project *out, fr_error *err) {
    memset(out, 0, sizeof *out);
    cJSON *root = cJSON_Parse(text);
    if (root == NULL) {
        fr_error_set(err, "\"%s\" is not valid json", origin);
        return FR_ERR;
    }

    const char *project_name = NULL;
    if (fr_json_string(root, "project", origin, &project_name, err) != FR_OK) {
        cJSON_Delete(root);
        return FR_ERR;
    }
    if (fr_plugins_reject_in_fetched(root, project_name, err) != FR_OK) {
        cJSON_Delete(root);
        return FR_ERR;
    }

    if (project_from_json(root, origin, out, err) != FR_OK) {
        fr_project_free(out);
        cJSON_Delete(root);
        return FR_ERR;
    }
    out->document = root;
    return FR_OK;
}

int fr_manifest_from_document(cJSON *root, const char *origin, fr_manifest *out, fr_error *err) {
    memset(out, 0, sizeof *out);
    if (manifest_from_json(root, origin, out, err) != FR_OK) {
        fr_manifest_free(out);
        cJSON_Delete(root);
        return FR_ERR;
    }
    out->document = root;
    return FR_OK;
}

void fr_project_free(fr_project *project) {
    if (project == NULL) return;
    for (size_t index = 0; index < project->module_count; index++) {
        module_free(&project->modules[index]);
    }
    free(project->modules);
    free(project->project);
    cJSON_Delete(project->document);
    project->modules = NULL;
    project->module_count = 0;
    project->project = NULL;
    project->document = NULL;
}

void fr_manifest_free(fr_manifest *manifest) {
    if (manifest == NULL) return;
    fr_project_free(&manifest->self);
    for (size_t index = 0; index < manifest->source_count; index++) {
        source_free(&manifest->sources[index]);
    }
    free(manifest->sources);
    for (size_t index = 0; index < manifest->consumer_count; index++) {
        consumer_free(&manifest->consumers[index]);
    }
    free(manifest->consumers);
    for (size_t index = 0; index < manifest->toolchain_count; index++) {
        fr_toolchain *toolchain = &manifest->toolchains[index];
        free(toolchain->name);
        free(toolchain->version);
        for (size_t d = 0; d < toolchain->dependency_count; d++) {
            dependency_free(&toolchain->dependencies[d]);
        }
        free(toolchain->dependencies);
    }
    free(manifest->toolchains);
    for (size_t index = 0; index < manifest->publish_count; index++) {
        free(manifest->publishes[index].name);
        free(manifest->publishes[index].from);
    }
    free(manifest->publishes);
    for (size_t index = 0; index < manifest->task_count; index++) {
        fr_task *task = &manifest->tasks[index];
        free(task->name);
        free(task->part_of);
        for (size_t d = 0; d < task->depends_on_count; d++) free(task->depends_on[d]);
        free(task->depends_on);
        task_command_free(task->run);
    }
    free(manifest->tasks);
    manifest->sources = NULL;
    manifest->source_count = 0;
    manifest->consumers = NULL;
    manifest->consumer_count = 0;
    manifest->toolchains = NULL;
    manifest->toolchain_count = 0;
    manifest->publishes = NULL;
    manifest->publish_count = 0;
    manifest->tasks = NULL;
    manifest->task_count = 0;
    cJSON_Delete(manifest->document);
    manifest->document = NULL;
}

const fr_module *fr_project_module(const fr_project *project, const char *name) {
    if (project == NULL || name == NULL) return NULL;
    for (size_t index = 0; index < project->module_count; index++) {
        if (strcmp(project->modules[index].name, name) == 0) return &project->modules[index];
    }
    return NULL;
}

const fr_source *fr_manifest_source(const fr_manifest *manifest, const char *project) {
    if (manifest == NULL || project == NULL) return NULL;
    for (size_t index = 0; index < manifest->source_count; index++) {
        if (strcmp(manifest->sources[index].project, project) == 0) return &manifest->sources[index];
    }
    return NULL;
}
