#include "tasks.h"

#include "derived.h"
#include "error.h"
#include "resolve.h"

#include <stdio.h>
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

/* Depth-first post-order, which yields dependencies before their dependents
   and, because the set is walked in declaration order, yields the same plan
   for the same input every time. state: 0 unvisited, 1 on the stack, 2 done.
   A 1 met again is the cycle. */
typedef struct {
    const fr_task_set *set;
    char *state;
    fr_task_plan *plan;
    const char **stack;
    size_t depth;
} plan_walk;

/* check_every_edge refuses every dangling edge before the walk begins, which
   makes the NULL branch below unreachable today; it stays so a later refactor
   reaching this function by another route meets a refusal, not a NULL deref. */
static int edge_target(const plan_walk *walk, const fr_task_node *node, const char *name,
                       const char *relation, const fr_task_node **out, fr_error *err) {
    const fr_task_node *found = fr_tasks_find(walk->set, name);
    if (found == NULL) {
        fr_error_set(err, "task \"%s\" %s \"%s\", which nothing declares", node->name, relation, name);
        return FR_ERR;
    }
    *out = found;
    return FR_OK;
}

static int visit(plan_walk *walk, const fr_task_node *node, fr_error *err);

static int visit_dependency(plan_walk *walk, const fr_task_node *node, const char *name,
                            const char *relation, fr_error *err) {
    const fr_task_node *target = NULL;
    if (edge_target(walk, node, name, relation, &target, err) != FR_OK) return FR_ERR;
    return visit(walk, target, err);
}

/* Anything that declared itself part of this task is a dependency of it. This
   is the reverse edge of spec section 4, joined here rather than stored, so
   that no plugin ever writes into another's declaration. */
static int visit_joiners(plan_walk *walk, const fr_task_node *node, fr_error *err) {
    for (size_t index = 0; index < walk->set->count; index++) {
        const fr_task_node *other = &walk->set->nodes[index];
        int joins = (other->part_of != NULL && strcmp(other->part_of, node->name) == 0)
                    || (other->extra_part_of != NULL && strcmp(other->extra_part_of, node->name) == 0);
        if (joins && visit(walk, other, err) != FR_OK) return FR_ERR;
    }
    return FR_OK;
}

static int report_cycle(plan_walk *walk, const fr_task_node *node, fr_error *err) {
    char trail[400];
    size_t used = 0;
    size_t start = 0;
    while (start < walk->depth && strcmp(walk->stack[start], node->name) != 0) start++;
    for (size_t index = start; index < walk->depth; index++) {
        int written = snprintf(trail + used, sizeof trail - used, "%s -> ", walk->stack[index]);
        if (written < 0 || (size_t) written >= sizeof trail - used) break;
        used += (size_t) written;
    }
    snprintf(trail + used, sizeof trail - used, "%s", node->name);
    fr_error_set(err, "these tasks depend on each other in a cycle: %s", trail);
    return FR_ERR;
}

static int visit(plan_walk *walk, const fr_task_node *node, fr_error *err) {
    size_t position = (size_t) (node - walk->set->nodes);
    if (walk->state[position] == 2) return FR_OK;
    if (walk->state[position] == 1) return report_cycle(walk, node, err);

    walk->state[position] = 1;
    walk->stack[walk->depth++] = node->name;

    for (size_t index = 0; index < node->depends_on_count; index++) {
        if (visit_dependency(walk, node, node->depends_on[index], "depends on", err) != FR_OK) return FR_ERR;
    }
    for (size_t index = 0; index < node->extra_depends_on_count; index++) {
        if (visit_dependency(walk, node, node->extra_depends_on[index], "depends on", err) != FR_OK) return FR_ERR;
    }
    if (visit_joiners(walk, node, err) != FR_OK) return FR_ERR;

    walk->depth--;
    walk->state[position] = 2;

    const fr_task_node **nodes = realloc(walk->plan->nodes, (walk->plan->count + 1) * sizeof *nodes);
    if (nodes == NULL) {
        fr_error_set(err, "out of memory planning task \"%s\"", node->name);
        return FR_ERR;
    }
    walk->plan->nodes = nodes;
    walk->plan->nodes[walk->plan->count++] = node;
    return FR_OK;
}

/* Every part_of and every dependsOn in the whole set is checked here, not
   only the ones the goal reaches, so a typo in a task nobody ran is still a
   refusal rather than a surprise on the day it is first asked for. */
static int check_every_edge(const fr_task_set *set, fr_error *err) {
    for (size_t index = 0; index < set->count; index++) {
        const fr_task_node *node = &set->nodes[index];
        if (node->part_of != NULL && fr_tasks_find(set, node->part_of) == NULL) {
            fr_error_set(err, "task \"%s\" is part of \"%s\", which nothing declares;"
                              " the plugin providing it is missing", node->name, node->part_of);
            return FR_ERR;
        }
        if (node->extra_part_of != NULL && fr_tasks_find(set, node->extra_part_of) == NULL) {
            fr_error_set(err, "task \"%s\" is part of \"%s\", which nothing declares;"
                              " the plugin providing it is missing", node->name, node->extra_part_of);
            return FR_ERR;
        }
        for (size_t edge = 0; edge < node->depends_on_count; edge++) {
            if (fr_tasks_find(set, node->depends_on[edge]) == NULL) {
                fr_error_set(err, "task \"%s\" depends on \"%s\", which nothing declares",
                             node->name, node->depends_on[edge]);
                return FR_ERR;
            }
        }
        for (size_t edge = 0; edge < node->extra_depends_on_count; edge++) {
            if (fr_tasks_find(set, node->extra_depends_on[edge]) == NULL) {
                fr_error_set(err, "task \"%s\" depends on \"%s\", which nothing declares",
                             node->name, node->extra_depends_on[edge]);
                return FR_ERR;
            }
        }
    }
    return FR_OK;
}

int fr_tasks_plan(const fr_task_set *set, const char *goal, fr_task_plan *out, fr_error *err) {
    memset(out, 0, sizeof *out);
    if (check_every_edge(set, err) != FR_OK) return FR_ERR;

    const fr_task_node *start = fr_tasks_find(set, goal);
    if (start == NULL) {
        fr_error_set(err, "no task \"%s\" is declared", goal);
        return FR_ERR;
    }

    plan_walk walk;
    walk.set = set;
    walk.plan = out;
    walk.depth = 0;
    walk.state = calloc(set->count, 1);
    walk.stack = calloc(set->count, sizeof *walk.stack);
    if (walk.state == NULL || walk.stack == NULL) {
        free(walk.state);
        free(walk.stack);
        fr_error_set(err, "out of memory planning \"%s\"", goal);
        return FR_ERR;
    }

    int status = visit(&walk, start, err);
    free(walk.state);
    free(walk.stack);
    if (status != FR_OK) fr_tasks_plan_free(out);
    return status;
}

void fr_tasks_plan_free(fr_task_plan *plan) {
    if (plan == NULL) return;
    free(plan->nodes);
    plan->nodes = NULL;
    plan->count = 0;
}

int fr_tasks_run(const fr_task_plan *plan, const fr_session *session, fr_error *err) {
    for (size_t index = 0; index < plan->count; index++) {
        const fr_task_node *node = plan->nodes[index];
        if (node->plugin == NULL || node->plugin->run == NULL) continue;

        char *derived_dir = NULL;
        if (fr_derived_dir(session->manifest_dir, node->toolchain->name, &derived_dir, err) != FR_OK) {
            return FR_ERR;
        }
        char *derived_root = NULL;
        if (fr_derived_root(session->manifest_dir, &derived_root, err) != FR_OK) {
            free(derived_dir);
            return FR_ERR;
        }
        int prepared = fr_derived_ensure_root(derived_root, err);
        free(derived_root);
        if (prepared != FR_OK || fr_derived_ensure_dir(derived_dir, err) != FR_OK) {
            free(derived_dir);
            return FR_ERR;
        }

        fr_resolved *resolved = NULL;
        size_t resolved_count = 0;
        if (fr_resolve_toolchain(node->toolchain, &session->manifest, session->manifest_dir,
                                 session->registry, &resolved, &resolved_count, err) != FR_OK) {
            free(derived_dir);
            return FR_ERR;
        }

        char version[64];
        snprintf(version, sizeof version, "%d.%d.%d", session->manifest.self.version.major,
                 session->manifest.self.version.minor, session->manifest.self.version.patch);

        fr_task_run_context context;
        context.name = node->name;
        context.toolchain = node->toolchain;
        context.project = session->manifest.self.project;
        context.version = version;
        context.root = FR_DERIVED_ROOT_RELATIVE;
        context.derived_dir = derived_dir;
        context.resolved = resolved;
        context.resolved_count = resolved_count;

        int status = node->plugin->run(node->plugin->state, &context, err);
        fr_resolved_free(resolved, resolved_count);
        free(derived_dir);
        if (status != FR_OK) {
            char original[sizeof err->message];
            memcpy(original, err->message, sizeof original);
            fr_error_set(err, "task \"%s\": %s", node->name, original);
            return FR_ERR;
        }
    }
    return FR_OK;
}
