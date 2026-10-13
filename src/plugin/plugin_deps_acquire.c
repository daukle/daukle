#include "plugin/plugin_deps.h"
#include "plugin/plugin_deps_internal.h"

#include "cJSON.h"
#include "plugin/plugin_fetch.h"
#include "plugin/plugin_modules.h"
#include "util/error.h"
#include "util/sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    if (copy == NULL) return fr_plugin_deps_out_of_memory(view, err);
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

    fr_error_set(err, "%s \"%s\": requiring \"%s\" is a cycle: %s", fr_plugin_deps_requester(view), view->label,
                 url, trail);
    return FR_ERR;
}

/* @implNote the read below is the pre-pass, and is the whole reason a
   dependency is never loaded: it runs the entry chunk only as far as its
   daukle.plugin{...} call, so requiring another plugin's coordinate parser
   does not also install that plugin's toolchain into the project. Shared by
   the ordinary path (open_node opens node->source first) and the override
   path (fr_plugins_acquire_source has already opened it, for any of the three
   forms an override may take). */
static int read_node_declaration(deps_node *node, fr_error *err) {
    const char *chunk = NULL;
    size_t chunk_length = 0;
    if (fr_plugin_source_entry(node->source, &chunk, &chunk_length, err) != FR_OK) return FR_ERR;

    return fr_plugins_read_declaration(chunk, chunk_length, node->url, "plugin", node->label,
                                       &node->declaration, err);
}

static int open_node(deps_node *node, fr_error *err) {
    if (fr_plugin_source_open_bytes(node->text, node->length, &node->source, err) != FR_OK) {
        return FR_ERR;
    }
    return read_node_declaration(node, err);
}

/* The override replaces what the author named entirely: url, sha256 and even
   the kind (path, url, or resolver coordinate) all come from the manifest's
   own value, turned into bytes the identical way a [plugins] entry is
   (fr_plugins_parse_entry, fr_plugins_acquire_source), never re-parsed here.
   Only ever reached at depth 0 (parent == DEPS_NONE): an override is written
   on the dependent's own [plugins] entry and names one of ITS aliases, so it
   has no meaning for an alias belonging to something the dependent itself
   requires, and acquire_one enforces that before calling this. That is also
   why this carries no depth or node-count guard of its own: depth is always 0
   here, under FR_PLUGIN_DEPS_MAX_DEPTH by construction, and this only ever
   runs from fr_plugin_deps_acquire's first loop, before graph->count can
   exceed FR_PLUGIN_MAX_REQUIRES, itself well under FR_PLUGIN_DEPS_MAX_NODES.
   A runtime check on either would be a second copy of an invariant nothing
   here could ever violate. Node reuse (node_with_url) and cycle detection are
   both skipped too: they exist to make a shared pin meaningless-order-
   independent, and an override's pin (if any) is never compared against
   another requester's, by design (see validate_overrides and the module
   doc). */
static int acquire_override(deps_graph *graph, fr_plugin_deps *view, size_t depth,
                            const fr_plugin_requirement *requirement, const struct cJSON *value,
                            const fr_resolver_entry *resolvers, size_t resolver_count,
                            fr_error *err) {
    /* Labelled as the override it is, not as the bare alias: plugins.c's
       shared messages (fr_plugins_parse_entry, fr_plugins_acquire_source) all
       read "plugin \"%s\": ...", and a bare alias there would send the reader
       looking for a [plugins.<alias>] entry that does not exist, the exact
       trap requester() exists to avoid everywhere else in this module. */
    char override_label[FR_PLUGIN_MAX_ALIAS + 96];
    snprintf(override_label, sizeof override_label, "%s's override for %s", view->label,
            requirement->alias);

    fr_plugin_entry entry;
    memset(&entry, 0, sizeof entry);
    entry.label = fr_dup_string(override_label);
    if (entry.label == NULL) return fr_plugin_deps_out_of_memory(view, err);
    if (fr_plugins_parse_entry(override_label, value, resolvers, resolver_count, &entry, err)
        != FR_OK) {
        fr_plugins_free_entry(&entry);
        return FR_ERR;
    }
    fr_plugin_kind kind = entry.kind;

    char *text = NULL;
    char *origin = NULL;
    char *resolved = NULL;
    size_t length = 0;
    char digest[65];
    fr_plugin_source *source = NULL;
    int status = fr_plugins_acquire_source(&entry, resolvers, resolver_count, &text, &length,
                                           &origin, &resolved, digest, &source, err);
    fr_plugins_free_entry(&entry);
    free(resolved);
    if (status != FR_OK) return FR_ERR;

    deps_node *node = &graph->nodes[graph->count];
    memset(node, 0, sizeof *node);
    node->text = text;
    node->length = length;
    node->source = source;
    node->parent = DEPS_NONE;
    node->depth = depth;
    node->overridden = 1;
    node->kind = kind;
    snprintf(node->digest, sizeof node->digest, "%s", digest);
    node->url = origin;
    node->label = fr_dup_string(requirement->alias);
    node->required_by = fr_dup_string(view->label);
    node->required_by_kind = fr_plugin_deps_requester(view);
    node->view.graph = graph;
    node->view.label_is_alias = 1;
    node->view.artifact = node;
    if (node->url == NULL || node->label == NULL || node->required_by == NULL) {
        fr_plugin_deps_free_node(node);
        return fr_plugin_deps_out_of_memory(view, err);
    }
    node->view.label = node->label;

    if (read_node_declaration(node, err) != FR_OK) {
        fr_plugin_deps_free_node(node);
        return FR_ERR;
    }

    graph->count++;
    return bind_alias(view, requirement->alias, graph->count - 1, err);
}

/* @implNote the cycle check must stay ahead of the digest check. An artifact's
   bytes carry the digest of what it requires, so no two artifacts requiring
   each other can both carry a correct pin: verify first and every real cycle
   reports as a mismatched pin instead, and the cycle refusal becomes
   unreachable. */
static int acquire_one(deps_graph *graph, fr_plugin_deps *view, size_t parent, size_t depth,
                       const fr_plugin_requirement *requirement, const struct cJSON *overrides,
                       const fr_resolver_entry *resolvers, size_t resolver_count, fr_error *err) {
    /* Scoped to depth 0 only; acquire_override's own doc says why. */
    if (parent == DEPS_NONE && overrides != NULL) {
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(overrides, requirement->alias);
        if (value != NULL) {
            return acquire_override(graph, view, depth, requirement, value, resolvers,
                                    resolver_count, err);
        }
    }

    const char *url = requirement->url;
    const char *sha256 = requirement->sha256;

    if (depth >= FR_PLUGIN_DEPS_MAX_DEPTH) {
        fr_error_set(err, "%s \"%s\": the dependency graph is deeper than %d, reaching \"%s\"",
                     fr_plugin_deps_requester(view), view->label, FR_PLUGIN_DEPS_MAX_DEPTH, url);
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
        if (!fr_sha256_hex_equal(held->digest, sha256)) {
            fr_error_set(err, "%s \"%s\": requires[\"%s\"] expected sha256 %s but \"%s\" is"
                              " already pinned to %s by %s \"%s\"",
                         fr_plugin_deps_requester(view), view->label, requirement->alias, sha256, url,
                         held->digest, held->required_by_kind, held->required_by);
            return FR_ERR;
        }
        return bind_alias(view, requirement->alias, existing, err);
    }

    if (graph->count == FR_PLUGIN_DEPS_MAX_NODES) {
        fr_error_set(err, "%s \"%s\": the dependency graph names more than %d artifacts",
                     fr_plugin_deps_requester(view), view->label, FR_PLUGIN_DEPS_MAX_NODES);
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
    if (!fr_sha256_hex_equal(digest, sha256)) {
        fr_error_set(err, "%s \"%s\": requires[\"%s\"] expected sha256 %s but the file is %s",
                     fr_plugin_deps_requester(view), view->label, requirement->alias, sha256, digest);
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
    node->kind = FR_PLUGIN_URL;
    snprintf(node->digest, sizeof node->digest, "%s", digest);
    node->url = fr_dup_string(url);
    node->label = fr_dup_string(requirement->alias);
    node->required_by = fr_dup_string(view->label);
    node->required_by_kind = fr_plugin_deps_requester(view);
    node->view.graph = graph;
    node->view.label_is_alias = 1;
    node->view.artifact = node;
    if (node->url == NULL || node->label == NULL || node->required_by == NULL) {
        fr_plugin_deps_free_node(node);
        return fr_plugin_deps_out_of_memory(view, err);
    }
    node->view.label = node->label;

    if (open_node(node, err) != FR_OK) {
        fr_plugin_deps_free_node(node);
        return FR_ERR;
    }

    graph->count++;
    return bind_alias(view, requirement->alias, graph->count - 1, err);
}

/* An override key must name an alias declaration->requires actually declares:
   an alias is author-chosen and a [plugins] label is user-chosen, and a typo
   that silently matched neither would leave the author's original target in
   place with no signal that anything was misspelled. An override value
   carrying its own "requires" key is refused too, as reserved: nesting an
   override inside an override is the same feature reached by a second
   spelling, and two spellings for one feature is the defect shape this
   project keeps producing. */
static int validate_overrides(const fr_plugin_declaration *declaration, const cJSON *overrides,
                              fr_error *err) {
    if (overrides == NULL) return FR_OK;
    const char *label = declaration->label != NULL ? declaration->label : "plugin";
    if (!cJSON_IsObject(overrides)) {
        fr_error_set(err, "plugin \"%s\": requires must be a table of alias to artifact", label);
        return FR_ERR;
    }

    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, overrides) {
        const char *alias = entry->string;
        int declared = 0;
        for (size_t index = 0; index < declaration->requires_count && !declared; index++) {
            declared = strcmp(declaration->requires[index].alias, alias) == 0;
        }
        if (!declared) {
            const char *names[FR_PLUGIN_MAX_REQUIRES];
            for (size_t index = 0; index < declaration->requires_count; index++) {
                names[index] = declaration->requires[index].alias;
            }
            char declared_aliases[256];
            fr_plugin_deps_list_names(declared_aliases, sizeof declared_aliases, names,
                      declaration->requires_count);
            fr_error_set(err, "plugin \"%s\": requires[\"%s\"] overrides an alias this plugin does"
                              " not declare; it declares %s",
                         label, alias, declared_aliases);
            return FR_ERR;
        }
        if (cJSON_IsObject(entry) && cJSON_GetObjectItemCaseSensitive(entry, "requires") != NULL) {
            fr_error_set(err, "plugin \"%s\": requires[\"%s\"] names its own requires, which is"
                              " reserved: [plugins.%s.requires.%s.requires] is not a supported"
                              " form, and an override may not nest another override inside it",
                         label, alias, label, alias);
            return FR_ERR;
        }
    }
    return FR_OK;
}

int fr_plugin_deps_acquire(const fr_plugin_declaration *declaration,
                           const struct cJSON *overrides, const fr_resolver_entry *resolvers,
                           size_t resolver_count, fr_plugin_deps **out, fr_error *err) {
    *out = NULL;

    if (validate_overrides(declaration, overrides, err) != FR_OK) return FR_ERR;

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
        if (acquire_one(graph, deps, DEPS_NONE, 0, &declaration->requires[index], overrides,
                        resolvers, resolver_count, err)
            != FR_OK) {
            fr_plugin_deps_close(deps);
            return FR_ERR;
        }
    }

    /* Breadth first over the nodes themselves: a node appended below is
       reached by this same loop, so the queue is the graph. Every call here
       has parent != DEPS_NONE, so acquire_one never applies overrides to it:
       depth 0 is the only place they are scoped to. */
    for (size_t index = 0; index < graph->count; index++) {
        deps_node *node = &graph->nodes[index];
        for (size_t at = 0; at < node->declaration.requires_count; at++) {
            if (acquire_one(graph, &node->view, index, node->depth + 1,
                            &node->declaration.requires[at], overrides, resolvers, resolver_count,
                            err)
                != FR_OK) {
                fr_plugin_deps_close(deps);
                return FR_ERR;
            }
        }
    }

    *out = deps;
    return FR_OK;
}
