#include "project/tasks_internal.h"

#include "plugin/plugins.h"
#include "util/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int own_string(fr_task_set *set, char *text, fr_error *err) {
    char **grown = realloc(set->owned, (set->owned_count + 1) * sizeof *grown);
    if (grown == NULL) {
        free(text);
        fr_error_set(err, "out of memory collecting task names");
        return FR_ERR;
    }
    set->owned = grown;
    set->owned[set->owned_count++] = text;
    return FR_OK;
}

static int own_list(fr_task_set *set, char **list, fr_error *err) {
    char ***grown = realloc(set->owned_lists, (set->owned_list_count + 1) * sizeof *grown);
    if (grown == NULL) {
        free(list);
        fr_error_set(err, "out of memory collecting task edges");
        return FR_ERR;
    }
    set->owned_lists = grown;
    set->owned_lists[set->owned_list_count++] = list;
    return FR_OK;
}

/* "node:install" becomes "web:install" for the instance web of the toolchain
   node; a bare edge and an edge naming another toolchain are returned as they
   are, because an aggregator is shared and another toolchain is not this
   instance's to rename. The result is registered with the set. */
static char *renamed_for_instance(fr_task_set *set, const char *edge, const fr_toolchain *instance,
                                  fr_error *err) {
    size_t prefix = strlen(instance->plugin);
    char *renamed = NULL;
    if (strncmp(edge, instance->plugin, prefix) != 0 || edge[prefix] != ':') {
        renamed = fr_dup_string(edge);
    } else {
        size_t length = strlen(instance->name) + strlen(edge + prefix) + 1;
        renamed = malloc(length);
        if (renamed != NULL) snprintf(renamed, length, "%s%s", instance->name, edge + prefix);
    }
    if (renamed == NULL) {
        fr_error_set(err, "out of memory renaming task \"%s\" for toolchain \"%s\"", edge,
                     instance->name);
        return NULL;
    }
    return own_string(set, renamed, err) == FR_OK ? renamed : NULL;
}

static int rename_edges(fr_task_set *set, const fr_task_plugin *plugin,
                        const fr_toolchain *instance, const char *const **out_edges,
                        fr_error *err) {
    *out_edges = NULL;
    if (plugin->depends_on_count == 0) return FR_OK;
    char **edges = calloc(plugin->depends_on_count, sizeof *edges);
    if (edges == NULL) {
        fr_error_set(err, "out of memory renaming the edges of task \"%s\"", plugin->capability);
        return FR_ERR;
    }
    if (own_list(set, edges, err) != FR_OK) return FR_ERR;
    for (size_t index = 0; index < plugin->depends_on_count; index++) {
        edges[index] = renamed_for_instance(set, plugin->depends_on[index], instance, err);
        if (edges[index] == NULL) return FR_ERR;
    }
    *out_edges = (const char *const *) edges;
    return FR_OK;
}

static int append_instance_node(fr_task_set *set, const fr_task_plugin *plugin, const char *name,
                                const fr_toolchain *instance, fr_error *err) {
    fr_task_node node;
    memset(&node, 0, sizeof node);
    node.name = renamed_for_instance(set, name, instance, err);
    if (node.name == NULL || fr_tasks_check_name(node.name, err) != FR_OK) return FR_ERR;
    if (fr_tasks_find(set, node.name) != NULL) {
        fr_error_set(err, "toolchain \"%s\" would declare task \"%s\" a second time",
                     instance->name, node.name);
        return FR_ERR;
    }
    if (plugin->part_of != NULL) {
        node.part_of = renamed_for_instance(set, plugin->part_of, instance, err);
        if (node.part_of == NULL) return FR_ERR;
    }
    if (rename_edges(set, plugin, instance, &node.depends_on, err) != FR_OK) return FR_ERR;
    node.depends_on_count = plugin->depends_on_count;
    node.plugin = plugin;
    node.toolchain = instance;
    return fr_tasks_append(set, &node, err);
}

int fr_tasks_append_toolchain_nodes(fr_task_set *set, const fr_manifest *manifest,
                                  const fr_task_plugin *plugin, const char *name, size_t prefix,
                                  fr_error *err) {
    for (size_t index = 0; index < manifest->toolchain_count; index++) {
        const fr_toolchain *toolchain = &manifest->toolchains[index];
        if (!fr_tasks_text_is_prefix(toolchain->plugin, name, prefix)) continue;
        int own_name = fr_tasks_text_is_prefix(toolchain->name, name, prefix);
        if (!own_name) {
            if (append_instance_node(set, plugin, name, toolchain, err) != FR_OK) return FR_ERR;
            continue;
        }
        fr_task_node node;
        memset(&node, 0, sizeof node);
        node.name = name;
        node.part_of = plugin->part_of;
        node.depends_on = plugin->depends_on;
        node.depends_on_count = plugin->depends_on_count;
        node.plugin = plugin;
        node.toolchain = toolchain;
        if (fr_tasks_append(set, &node, err) != FR_OK) return FR_ERR;
    }
    return FR_OK;
}
