#include "project/tasks.h"

#include "util/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
