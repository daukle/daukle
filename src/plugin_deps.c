#include "plugin_deps.h"

#include "error.h"
#include "plugin_fetch.h"
#include "plugin_modules.h"
#include "sha256.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEPS_NONE ((size_t) -1)

typedef struct deps_graph deps_graph;

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
    deps_binding bindings[FR_PLUGIN_MAX_REQUIRES];
    size_t binding_count;
    int owns_graph;
};

typedef struct {
    char *url;
    char *label;              /* the alias it was first named by, for messages */
    char *required_by;        /* and who named it, for a disagreement over its pin */
    const char *required_by_kind;
    char digest[65];
    char *text;
    size_t length;
    fr_plugin_source *source;
    fr_plugin_declaration declaration;
    int overridden;           /* whether an override replaced what the author declared */
    size_t parent;            /* who required it, for naming a cycle's chain */
    size_t depth;
    fr_plugin_deps view;
} deps_node;

struct deps_graph {
    deps_node nodes[FR_PLUGIN_DEPS_MAX_NODES];
    size_t count;
};

/* Every refusal below names who asked, and the root is a plugin while every
   other requester is one plugin's alias for another. Calling an alias a plugin
   sends a reader looking for a [plugins] entry that does not exist. */
static const char *requester(const fr_plugin_deps *view) {
    return view->label_is_alias ? "alias" : "plugin";
}

static int out_of_memory(const fr_plugin_deps *view, fr_error *err) {
    fr_error_set(err, "%s \"%s\": out of memory acquiring its dependencies", requester(view),
                 view->label);
    return FR_ERR;
}

/* plugins.c's own copy is a file static there, and this module may not reach
   into it: a pin may be written in either case and must still match. */
static int digest_matches(const char *actual, const char *pinned) {
    size_t length = strlen(actual);
    if (length != strlen(pinned) || length >= 65) return 0;
    for (size_t index = 0; index < length; index++) {
        if (tolower((unsigned char) actual[index]) != tolower((unsigned char) pinned[index])) {
            return 0;
        }
    }
    return 1;
}

static void free_bindings(fr_plugin_deps *view) {
    for (size_t index = 0; index < view->binding_count; index++) free(view->bindings[index].alias);
    view->binding_count = 0;
}

static void free_node(deps_node *node) {
    fr_plugin_source_close(node->source);
    fr_plugins_free_declaration(&node->declaration);
    free_bindings(&node->view);
    free(node->text);
    free(node->label);
    free(node->required_by);
    free(node->url);
    memset(node, 0, sizeof *node);
}

static size_t node_with_url(const deps_graph *graph, const char *url) {
    for (size_t index = 0; index < graph->count; index++) {
        if (strcmp(graph->nodes[index].url, url) == 0) return index;
    }
    return DEPS_NONE;
}

/* The bindings array is FR_PLUGIN_MAX_REQUIRES and the requires it is filled
   from is an array of the same bound, so the capacity is a type-level invariant
   and a runtime check on it would be a second copy of one fact. */
static int bind_alias(fr_plugin_deps *view, const char *alias, size_t node, fr_error *err) {
    char *copy = fr_dup_string(alias);
    if (copy == NULL) return out_of_memory(view, err);
    view->bindings[view->binding_count].alias = copy;
    view->bindings[view->binding_count].node = node;
    view->binding_count++;
    return FR_OK;
}

static int url_on_path(const deps_graph *graph, size_t parent, const char *url) {
    for (size_t at = parent; at != DEPS_NONE; at = graph->nodes[at].parent) {
        if (strcmp(graph->nodes[at].url, url) == 0) return 1;
    }
    return 0;
}

/* Names the chain rather than only the artifact that closed it, in the manner
   of config_lua.c's report_module_cycle. The url that closed the cycle is
   written twice: once before the chain, where no length of chain can push it
   out of the message, and once as the chain's last hop, which is what makes the
   chain readable and is the part a long one truncates. */
static int report_cycle(const deps_graph *graph, const fr_plugin_deps *view, size_t parent,
                        const char *url, fr_error *err) {
    size_t chain[FR_PLUGIN_DEPS_MAX_DEPTH + 1];
    size_t chain_length = 0;
    for (size_t at = parent; at != DEPS_NONE && chain_length < sizeof chain / sizeof *chain;
         at = graph->nodes[at].parent) {
        chain[chain_length] = at;
        chain_length++;
    }

    char trail[400];
    size_t filled = 0;
    trail[0] = '\0';
    while (chain_length > 0) {
        chain_length--;
        int written = snprintf(trail + filled, sizeof trail - filled, "%s%s",
                               filled == 0 ? "" : " -> ", graph->nodes[chain[chain_length]].url);
        if (written < 0 || (size_t) written >= sizeof trail - filled) break;
        filled += (size_t) written;
    }
    snprintf(trail + filled, sizeof trail - filled, "%s%s", filled == 0 ? "" : " -> ", url);

    fr_error_set(err, "%s \"%s\": requiring \"%s\" is a cycle: %s", requester(view), view->label,
                 url, trail);
    return FR_ERR;
}

/* @implNote the read below is the pre-pass, and is the whole reason a
   dependency is never loaded: it runs the entry chunk only as far as its
   daukle.plugin{...} call, so requiring another plugin's coordinate parser
   does not also install that plugin's toolchain into the project. */
static int open_node(deps_node *node, fr_error *err) {
    if (fr_plugin_source_open_bytes(node->text, node->length, &node->source, err) != FR_OK) {
        return FR_ERR;
    }
    const char *chunk = NULL;
    size_t chunk_length = 0;
    if (fr_plugin_source_entry(node->source, &chunk, &chunk_length, err) != FR_OK) return FR_ERR;

    return fr_plugins_read_declaration(chunk, chunk_length, node->url, "plugin", node->label,
                                       &node->declaration, err);
}

/* @implNote the cycle check must stay ahead of the digest check. An artifact's
   bytes carry the digest of what it requires, so no two artifacts requiring
   each other can both carry a correct pin: verify first and every real cycle
   reports as a mismatched pin instead, and the cycle refusal becomes
   unreachable. */
static int acquire_one(deps_graph *graph, fr_plugin_deps *view, size_t parent, size_t depth,
                       const fr_plugin_requirement *requirement, const struct cJSON *overrides,
                       fr_error *err) {
    /* An override replaces url and sha256 here, and sets the node's
       overridden below; none is applied yet. */
    (void) overrides;
    const char *url = requirement->url;
    const char *sha256 = requirement->sha256;

    if (depth >= FR_PLUGIN_DEPS_MAX_DEPTH) {
        fr_error_set(err, "%s \"%s\": the dependency graph is deeper than %d, reaching \"%s\"",
                     requester(view), view->label, FR_PLUGIN_DEPS_MAX_DEPTH, url);
        return FR_ERR;
    }
    if (url_on_path(graph, parent, url)) return report_cycle(graph, view, parent, url, err);

    size_t existing = node_with_url(graph, url);
    if (existing != DEPS_NONE) {
        const deps_node *held = &graph->nodes[existing];
        /* A second requester's pin is checked against what was already
           fetched, not skipped: whoever asked first would otherwise decide
           which bytes everyone gets, and the attestation the later author
           wrote would never be compared to anything. */
        if (!digest_matches(held->digest, sha256)) {
            fr_error_set(err, "%s \"%s\": requires[\"%s\"] expected sha256 %s but \"%s\" is"
                              " already pinned to %s by %s \"%s\"",
                         requester(view), view->label, requirement->alias, sha256, url,
                         held->digest, held->required_by_kind, held->required_by);
            return FR_ERR;
        }
        return bind_alias(view, requirement->alias, existing, err);
    }

    if (graph->count == FR_PLUGIN_DEPS_MAX_NODES) {
        fr_error_set(err, "%s \"%s\": the dependency graph names more than %d artifacts",
                     requester(view), view->label, FR_PLUGIN_DEPS_MAX_NODES);
        return FR_ERR;
    }

    char *text = NULL;
    size_t length = 0;
    if (fr_plugin_fetch(url, NULL, 0, &text, &length, err) != FR_OK) return FR_ERR;

    /* Checked before the source is opened and the declaration read, because
       reading the declaration already runs part of the chunk: verifying after
       would mean the unpinned bytes had already executed. */
    char digest[65];
    fr_sha256_hex(text, length, digest);
    if (!digest_matches(digest, sha256)) {
        fr_error_set(err, "%s \"%s\": requires[\"%s\"] expected sha256 %s but the file is %s",
                     requester(view), view->label, requirement->alias, sha256, digest);
        fr_plugin_fetch_discard(url);
        free(text);
        return FR_ERR;
    }

    deps_node *node = &graph->nodes[graph->count];
    memset(node, 0, sizeof *node);
    node->text = text;
    node->length = length;
    node->parent = parent;
    node->depth = depth;
    snprintf(node->digest, sizeof node->digest, "%s", digest);
    node->url = fr_dup_string(url);
    node->label = fr_dup_string(requirement->alias);
    node->required_by = fr_dup_string(view->label);
    node->required_by_kind = requester(view);
    node->view.graph = graph;
    node->view.label_is_alias = 1;
    if (node->url == NULL || node->label == NULL || node->required_by == NULL) {
        free_node(node);
        return out_of_memory(view, err);
    }
    node->view.label = node->label;

    if (open_node(node, err) != FR_OK) {
        free_node(node);
        return FR_ERR;
    }

    graph->count++;
    return bind_alias(view, requirement->alias, graph->count - 1, err);
}

int fr_plugin_deps_acquire(const fr_plugin_declaration *declaration,
                           const struct cJSON *overrides, const fr_resolver_entry *resolvers,
                           size_t resolver_count, fr_plugin_deps **out, fr_error *err) {
    (void) resolvers;
    (void) resolver_count;
    *out = NULL;

    fr_plugin_deps *deps = calloc(1, sizeof *deps);
    deps_graph *graph = calloc(1, sizeof *graph);
    char *label = fr_dup_string(declaration->label != NULL ? declaration->label : "plugin");
    if (deps == NULL || graph == NULL || label == NULL) {
        free(deps);
        free(graph);
        free(label);
        fr_error_set(err, "out of memory acquiring dependencies");
        return FR_ERR;
    }
    deps->graph = graph;
    deps->label = label;
    deps->owns_graph = 1;

    for (size_t index = 0; index < declaration->requires_count; index++) {
        if (acquire_one(graph, deps, DEPS_NONE, 0, &declaration->requires[index], overrides, err)
            != FR_OK) {
            fr_plugin_deps_close(deps);
            return FR_ERR;
        }
    }

    /* Breadth first over the nodes themselves: a node appended below is
       reached by this same loop, so the queue is the graph. */
    for (size_t index = 0; index < graph->count; index++) {
        deps_node *node = &graph->nodes[index];
        for (size_t at = 0; at < node->declaration.requires_count; at++) {
            if (acquire_one(graph, &node->view, index, node->depth + 1,
                            &node->declaration.requires[at], overrides, err)
                != FR_OK) {
                fr_plugin_deps_close(deps);
                return FR_ERR;
            }
        }
    }

    *out = deps;
    return FR_OK;
}

/* The "nothing" fallback is on an empty list and not on an empty buffer: a list
   too long for out has already had its truncated head written there, and saying
   "nothing" instead would report the opposite of what is true. */
static void list_names(char *out, size_t size, const char *const *names, size_t count) {
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

static void list_aliases(char *out, size_t size, const fr_plugin_deps *deps) {
    const char *names[FR_PLUGIN_MAX_REQUIRES];
    for (size_t index = 0; index < deps->binding_count; index++) {
        names[index] = deps->bindings[index].alias;
    }
    list_names(out, size, names, deps->binding_count);
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
                     requester(deps), deps->label, alias, aliases);
        return FR_ERR;
    }

    deps_node *node = &deps->graph->nodes[binding->node];
    if (!exports_member(node, member)) {
        char exported[256];
        list_names(exported, sizeof exported, (const char *const *) node->declaration.exports,
                   node->declaration.exports_count);
        fr_error_set(err, "%s \"%s\": \"%s\" does not export \"%s\"; it exports %s",
                     requester(deps), deps->label, alias, member, exported);
        return FR_ERR;
    }
    if (fr_plugin_source_member(node->source, member, out_text, out_length, err) != FR_OK) {
        return FR_ERR;
    }

    *out_owner = &node->view;
    *out_owner_label = node->label;
    return FR_OK;
}

/* A node's own view is owned by the graph, so closing the one
   fr_plugin_deps_member handed back must not take the acquisition down with
   it: only what fr_plugin_deps_acquire produced closes anything. */
void fr_plugin_deps_close(fr_plugin_deps *deps) {
    if (deps == NULL || !deps->owns_graph) return;
    deps_graph *graph = deps->graph;
    for (size_t index = 0; index < graph->count; index++) free_node(&graph->nodes[index]);
    free(graph);
    free_bindings(deps);
    free(deps->label);
    free(deps);
}
