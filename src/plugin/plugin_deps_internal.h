#ifndef DAUKLE_PLUGIN_DEPS_INTERNAL_H
#define DAUKLE_PLUGIN_DEPS_INTERNAL_H

#include "plugin/plugin_deps.h"

#include <stddef.h>

/* The seam between plugin_deps.c, which answers questions about an acquired
   dependency graph, and plugin_deps_acquire.c, which builds it. The graph's
   shape is shared; everything else stays in the file that owns it. */

#define DEPS_NONE ((size_t) -1)

typedef struct deps_graph deps_graph;
typedef struct deps_node deps_node;

typedef struct {
    char *alias;
    size_t node;
} deps_binding;

/* One dependent's view of the graph: which artifact each of ITS aliases names.
   The root view owns the graph and its label, a node's view owns neither, which
   is what lets fr_plugin_deps_member hand a node's own view out as the set a
   require inside that artifact resolves against. */
struct fr_plugin_deps {
    deps_graph *graph;
    char *label;
    int label_is_alias;       /* a node is known by the alias that named it, not by a plugin label */
    deps_node *artifact;      /* the node this view belongs to, NULL for the acquired root view */
    deps_binding bindings[FR_PLUGIN_MAX_REQUIRES];
    size_t binding_count;
    int owns_graph;
};

struct deps_node {
    char *url;
    char *label;              /* the alias it was first named by, for messages */
    char *required_by;        /* and who named it, for a disagreement over its pin */
    const char *required_by_kind;
    fr_plugin_kind kind;       /* FR_PLUGIN_URL for an ordinary fetch, whatever the override used otherwise */
    char digest[65];
    char *text;
    size_t length;
    fr_plugin_source *source;
    fr_plugin_declaration declaration;
    int overridden;           /* whether an override replaced what the author declared */
    size_t parent;            /* who required it, for naming a cycle's chain */
    size_t depth;
    fr_plugin_deps view;
};

struct deps_graph {
    deps_node nodes[FR_PLUGIN_DEPS_MAX_NODES];
    size_t count;
};

const char *fr_plugin_deps_requester(const fr_plugin_deps *view);
int fr_plugin_deps_out_of_memory(const fr_plugin_deps *view, fr_error *err);
void fr_plugin_deps_list_names(char *out, size_t size, const char *const *names, size_t count);
void fr_plugin_deps_free_node(deps_node *node);

#endif
