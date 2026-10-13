#include "plugin/plugin_deps.h"
#include "plugin/plugin_deps_internal.h"

#include "plugin/plugin_modules.h"
#include "util/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Every refusal below names who asked, and the root is a plugin while every
   other requester is one plugin's alias for another. Calling an alias a plugin
   sends a reader looking for a [plugins] entry that does not exist. */
const char *fr_plugin_deps_requester(const fr_plugin_deps *view) {
    return view->label_is_alias ? "alias" : "plugin";
}

int fr_plugin_deps_out_of_memory(const fr_plugin_deps *view, fr_error *err) {
    fr_error_set(err, "%s \"%s\": out of memory acquiring its dependencies", fr_plugin_deps_requester(view),
                 view->label);
    return FR_ERR;
}

/* The "nothing" fallback is on an empty list and not on an empty buffer: a list
   too long for out has already had its truncated head written there, and saying
   "nothing" instead would report the opposite of what is true. */
void fr_plugin_deps_list_names(char *out, size_t size, const char *const *names, size_t count) {
    size_t filled = 0;
    out[0] = '\0';
    if (count == 0) {
        snprintf(out, size, "nothing");
        return;
    }
    for (size_t index = 0; index < count; index++) {
        int written = snprintf(out + filled, size - filled, "%s%s", filled == 0 ? "" : ", ",
                               names[index]);
        if (written < 0 || (size_t) written >= size - filled) break;
        filled += (size_t) written;
    }
}

static void free_bindings(fr_plugin_deps *view) {
    for (size_t index = 0; index < view->binding_count; index++) free(view->bindings[index].alias);
    view->binding_count = 0;
}

void fr_plugin_deps_free_node(deps_node *node) {
    fr_plugin_source_close(node->source);
    fr_plugins_free_declaration(&node->declaration);
    free_bindings(&node->view);
    free(node->text);
    free(node->label);
    free(node->required_by);
    free(node->url);
    memset(node, 0, sizeof *node);
}

static void list_aliases(char *out, size_t size, const fr_plugin_deps *deps) {
    const char *names[FR_PLUGIN_MAX_REQUIRES];
    for (size_t index = 0; index < deps->binding_count; index++) {
        names[index] = deps->bindings[index].alias;
    }
    fr_plugin_deps_list_names(out, size, names, deps->binding_count);
}

static int exports_member(const deps_node *node, const char *member) {
    for (size_t index = 0; index < node->declaration.exports_count; index++) {
        if (strcmp(node->declaration.exports[index], member) == 0) return 1;
    }
    return 0;
}

int fr_plugin_deps_member(fr_plugin_deps *deps, const char *alias, const char *member,
                          const char **out_text, size_t *out_length,
                          fr_plugin_deps **out_owner, const char **out_owner_label,
                          fr_error *err) {
    *out_text = NULL;
    *out_length = 0;
    *out_owner = NULL;
    *out_owner_label = NULL;

    if (deps == NULL) {
        fr_error_set(err, "no dependency is required under the alias \"%s\": this plugin requires"
                          " nothing",
                     alias);
        return FR_ERR;
    }

    const deps_binding *binding = NULL;
    for (size_t index = 0; index < deps->binding_count && binding == NULL; index++) {
        if (strcmp(deps->bindings[index].alias, alias) == 0) binding = &deps->bindings[index];
    }
    if (binding == NULL) {
        char aliases[256];
        list_aliases(aliases, sizeof aliases, deps);
        fr_error_set(err, "%s \"%s\": no dependency is required under the alias \"%s\"; it"
                          " requires %s",
                     fr_plugin_deps_requester(deps), deps->label, alias, aliases);
        return FR_ERR;
    }

    deps_node *node = &deps->graph->nodes[binding->node];
    if (!exports_member(node, member)) {
        char exported[256];
        fr_plugin_deps_list_names(exported, sizeof exported, (const char *const *) node->declaration.exports,
                                  node->declaration.exports_count);
        fr_error_set(err, "%s \"%s\": \"%s\" does not export \"%s\"; it exports %s",
                     fr_plugin_deps_requester(deps), deps->label, alias, member, exported);
        return FR_ERR;
    }
    if (fr_plugin_source_member(node->source, member, out_text, out_length, err) != FR_OK) {
        return FR_ERR;
    }

    *out_owner = &node->view;
    *out_owner_label = node->label;
    return FR_OK;
}

int fr_plugin_deps_own_member(fr_plugin_deps *deps, const char *member, const char **out_text,
                              size_t *out_length, fr_error *err) {
    *out_text = NULL;
    *out_length = 0;

    /* deps->artifact is never NULL here: the only caller passes an owner
       frame's view, which is always an acquired node's own view, never the
       root view fr_plugin_deps_acquire returns. */
    if (deps == NULL) {
        fr_error_set(err, "daukle.require(\"%s\"): no required artifact owns this module", member);
        return FR_ERR;
    }
    return fr_plugin_source_member(deps->artifact->source, member, out_text, out_length, err);
}

const char *fr_plugin_deps_label(const fr_plugin_deps *deps) {
    return deps != NULL ? deps->label : NULL;
}

size_t fr_plugin_deps_count(const fr_plugin_deps *deps) {
    return deps != NULL ? deps->graph->count : 0;
}

size_t fr_plugin_deps_row_count(const fr_plugin_deps *deps) {
    if (deps == NULL) return 0;
    size_t count = deps->binding_count;
    const deps_graph *graph = deps->graph;
    for (size_t index = 0; index < graph->count; index++) {
        count += graph->nodes[index].view.binding_count;
    }
    return count;
}

/* Finds the view (the root's own, or one acquired node's) that holds the binding at index, walking
   the same views fr_plugin_deps_row_count sums so the two always agree on how many rows there are. */
static const fr_plugin_deps *view_for_row(const fr_plugin_deps *deps, size_t *index) {
    if (*index < deps->binding_count) return deps;
    *index -= deps->binding_count;

    const deps_graph *graph = deps->graph;
    for (size_t node_index = 0; node_index < graph->count; node_index++) {
        const fr_plugin_deps *view = &graph->nodes[node_index].view;
        if (*index < view->binding_count) return view;
        *index -= view->binding_count;
    }
    return NULL;
}

/* One row per binding rather than one per node: two dependents naming the same url are two
   bindings, and reporting them as one would keep whichever dependent's alias a Lua table walk
   reached first, silently dropping the other's attribution. The artifact fields (url, digest, uses,
   kind, overridden) come from the node the binding names; alias and required_by come from the
   binding's own view, so every row is attributed to whoever actually named it. */
void fr_plugin_deps_row_at(const fr_plugin_deps *deps, size_t index, fr_plugin_deps_row *out) {
    if (deps == NULL) {
        memset(out, 0, sizeof *out);
        return;
    }
    const fr_plugin_deps *view = view_for_row(deps, &index);
    const deps_binding *binding = &view->bindings[index];
    const deps_node *node = &deps->graph->nodes[binding->node];

    out->url = node->url;
    out->alias = binding->alias;
    out->required_by = view->label;
    out->digest = node->digest;
    out->uses = (const char *const *) node->declaration.uses;
    out->uses_count = node->declaration.uses_count;
    out->kind = node->kind;
    out->overridden = node->overridden;
}

/* A node's own view is owned by the graph, so closing the one
   fr_plugin_deps_member handed back must not take the acquisition down with
   it: only what fr_plugin_deps_acquire produced closes anything. */
void fr_plugin_deps_close(fr_plugin_deps *deps) {
    if (deps == NULL || !deps->owns_graph) return;
    deps_graph *graph = deps->graph;
    for (size_t index = 0; index < graph->count; index++) fr_plugin_deps_free_node(&graph->nodes[index]);
    free(graph);
    free_bindings(deps);
    free(deps->label);
    free(deps);
}
