#include "tasks.h"

#include "error.h"

#include <stdlib.h>
#include <string.h>

#define FR_TASK_CAPABILITY_PREFIX "daukle.task/"

static const char *RESERVED_COMMANDS[] = {
    "sync", "check", "add", "config", "plugin", "clean", "tasks"
};

int fr_tasks_name_is_reserved(const char *name) {
    for (size_t index = 0; index < sizeof RESERVED_COMMANDS / sizeof *RESERVED_COMMANDS; index++) {
        if (strcmp(RESERVED_COMMANDS[index], name) == 0) return 1;
    }
    return 0;
}

/* The leaf grammar of spec section 3, applied to a bare name and to each half
   of a prefixed one. Narrow on purpose: widening it later cannot break a name
   that already parses, and the reverse is not true. */
static int leaf_is_usable(const char *text) {
    if (text[0] == '\0') return 0;
    if (!((text[0] >= 'a' && text[0] <= 'z') || (text[0] >= '0' && text[0] <= '9'))) return 0;
    for (const char *cursor = text; *cursor != '\0'; cursor++) {
        char c = *cursor;
        int ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
                 || c == '.' || c == '_' || c == '-';
        if (!ok) return 0;
    }
    return 1;
}

/* Returns the length of the "<toolchain>" half, or 0 when the name is bare.
   A name with more than one colon is not prefixed, it is malformed, and
   leaf_is_usable rejects the remainder. */
static size_t prefix_length(const char *name) {
    const char *colon = strchr(name, ':');
    return colon == NULL ? 0 : (size_t) (colon - name);
}

static int name_is_usable(const char *name) {
    size_t prefix = prefix_length(name);
    if (prefix == 0) return leaf_is_usable(name);
    char owner[128];
    if (prefix >= sizeof owner) return 0;
    memcpy(owner, name, prefix);
    owner[prefix] = '\0';
    return leaf_is_usable(owner) && leaf_is_usable(name + prefix + 1);
}

static const fr_toolchain *toolchain_for(const fr_manifest *manifest, const char *name,
                                         size_t prefix) {
    for (size_t index = 0; index < manifest->toolchain_count; index++) {
        const char *declared = manifest->toolchains[index].name;
        if (strlen(declared) == prefix && strncmp(declared, name, prefix) == 0) {
            return &manifest->toolchains[index];
        }
    }
    return NULL;
}

const fr_task_node *fr_tasks_find(const fr_task_set *set, const char *name) {
    for (size_t index = 0; index < set->count; index++) {
        if (strcmp(set->nodes[index].name, name) == 0) return &set->nodes[index];
    }
    return NULL;
}

/* The same search as fr_tasks_find, written out rather than casting its
   const result away: collection needs to add edges to a node it already
   placed, and a cast that launders const is a cast the next reader has to
   re-derive the safety of. */
static fr_task_node *find_mutable(fr_task_set *set, const char *name) {
    for (size_t index = 0; index < set->count; index++) {
        if (strcmp(set->nodes[index].name, name) == 0) return &set->nodes[index];
    }
    return NULL;
}

static int check_name(const char *name, fr_error *err) {
    if (!name_is_usable(name)) {
        fr_error_set(err, "\"%s\" is not a usable task name: a name is lowercase letters, digits,"
                          " \".\", \"_\" and \"-\", optionally prefixed by a toolchain and a colon", name);
        return FR_ERR;
    }
    if (fr_tasks_name_is_reserved(name)) {
        fr_error_set(err, "a task may not be called \"%s\": that is also a daukle command,"
                          " and the command wins", name);
        return FR_ERR;
    }
    return FR_OK;
}

static int append(fr_task_set *set, const fr_task_node *node, fr_error *err) {
    fr_task_node *nodes = realloc(set->nodes, (set->count + 1) * sizeof *nodes);
    if (nodes == NULL) {
        fr_error_set(err, "out of memory collecting task \"%s\"", node->name);
        return FR_ERR;
    }
    set->nodes = nodes;
    set->nodes[set->count++] = *node;
    return FR_OK;
}

int fr_tasks_collect(const fr_registry *registry, const fr_manifest *manifest,
                     fr_task_set *out, fr_error *err) {
    memset(out, 0, sizeof *out);

    for (size_t index = 0; index < fr_registry_task_count(registry); index++) {
        const fr_task_plugin *plugin = fr_registry_task_at(registry, index);
        if (strncmp(plugin->capability, FR_TASK_CAPABILITY_PREFIX,
                    strlen(FR_TASK_CAPABILITY_PREFIX)) != 0) {
            fr_error_set(err, "\"%s\" is not a task capability: it does not start with \"%s\"",
                        plugin->capability, FR_TASK_CAPABILITY_PREFIX);
            fr_tasks_set_free(out);
            return FR_ERR;
        }
        const char *name = plugin->capability + strlen(FR_TASK_CAPABILITY_PREFIX);
        if (check_name(name, err) != FR_OK) {
            fr_tasks_set_free(out);
            return FR_ERR;
        }

        size_t prefix = prefix_length(name);
        if (prefix == 0 && plugin->run != NULL) {
            fr_error_set(err, "task \"%s\" may not run anything: a task with no toolchain in its"
                              " name drives no tool", name);
            fr_tasks_set_free(out);
            return FR_ERR;
        }

        const fr_toolchain *toolchain = NULL;
        if (prefix > 0) {
            toolchain = toolchain_for(manifest, name, prefix);
            if (toolchain == NULL) continue;
        }

        fr_task_node node;
        memset(&node, 0, sizeof node);
        node.name = name;
        node.part_of = plugin->part_of;
        node.depends_on = plugin->depends_on;
        node.depends_on_count = plugin->depends_on_count;
        node.plugin = plugin;
        node.toolchain = toolchain;
        if (append(out, &node, err) != FR_OK) {
            fr_tasks_set_free(out);
            return FR_ERR;
        }
    }

    for (size_t index = 0; index < manifest->task_count; index++) {
        const fr_task *task = &manifest->tasks[index];
        if (check_name(task->name, err) != FR_OK) {
            fr_tasks_set_free(out);
            return FR_ERR;
        }

        fr_task_node *existing = find_mutable(out, task->name);
        if (existing != NULL) {
            existing->extra_depends_on = (const char *const *) task->depends_on;
            existing->extra_depends_on_count = task->depends_on_count;
            existing->extra_part_of = task->part_of;
            continue;
        }

        if (prefix_length(task->name) > 0) {
            fr_error_set(err, "task \"%s\" cannot be declared here: that name belongs to the plugin"
                              " providing its toolchain", task->name);
            fr_tasks_set_free(out);
            return FR_ERR;
        }

        fr_task_node node;
        memset(&node, 0, sizeof node);
        node.name = task->name;
        node.part_of = task->part_of;
        node.depends_on = (const char *const *) task->depends_on;
        node.depends_on_count = task->depends_on_count;
        if (append(out, &node, err) != FR_OK) {
            fr_tasks_set_free(out);
            return FR_ERR;
        }
    }
    return FR_OK;
}

void fr_tasks_set_free(fr_task_set *set) {
    if (set == NULL) return;
    free(set->nodes);
    set->nodes = NULL;
    set->count = 0;
}
