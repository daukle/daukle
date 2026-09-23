#ifndef DAUKLE_TASKS_H
#define DAUKLE_TASKS_H

#include "registry.h"
#include "sync.h"
#include "types.h"

#include <stddef.h>

/* One task in the collected set. plugin is NULL when the manifest created the
   task, which is the aggregator case; toolchain is NULL unless the name
   carries a "<toolchain>:" prefix. Every string is borrowed from the registry
   or the manifest, both of which outlive the set. */
typedef struct {
    const char *name;
    const char *part_of;
    const char *const *depends_on;
    size_t depends_on_count;
    const char *const *extra_depends_on;  /* edges a manifest block added to a declared task */
    size_t extra_depends_on_count;
    const char *extra_part_of;
    const fr_task_plugin *plugin;
    const fr_toolchain *toolchain;
} fr_task_node;

typedef struct {
    fr_task_node *nodes;
    size_t count;
} fr_task_set;

/* Builds the set every other function here works over: every task a plugin
   declared whose toolchain (if any) the manifest declares, plus every task a
   manifest block created, with a manifest block naming a declared task adding
   edges to it rather than creating a second one. Refuses a reserved name, a
   malformed name, a bodied bare name and a manifest block claiming a
   "<toolchain>:" prefix. */
int fr_tasks_collect(const fr_registry *registry, const fr_manifest *manifest,
                     fr_task_set *out, fr_error *err);
void fr_tasks_set_free(fr_task_set *set);
const fr_task_node *fr_tasks_find(const fr_task_set *set, const char *name);

/* FR_TASK_JOIN_PART_OF: what declared itself part of a name. Those run before
   it and it pulls them in. FR_TASK_JOIN_DEPENDS_ON: what names it in
   dependsOn. Those run after it and it is what pulls THEM in: it is a
   prerequisite of theirs, not the reverse. The two kinds run in opposite
   directions, so a listing that does not tell them apart states one of them
   backwards. */
typedef enum {
    FR_TASK_JOIN_PART_OF,
    FR_TASK_JOIN_DEPENDS_ON
} fr_task_join_kind;

/* Every task pointing at name by the given kind of edge. This is the inbound
   half of the graph, which a plugin's own source cannot show, and it is why
   "daukle tasks" is a complete view rather than another partial one. Writes
   at most capacity names, but always returns the true total, even past
   capacity: the caller compares the return value against capacity to learn
   whether the list it was handed is the whole answer. */
size_t fr_tasks_joiners(const fr_task_set *set, const char *name, fr_task_join_kind kind,
                        const char **out_names, size_t capacity);

/* The built-in command names. A task may not take one, because the command
   wins at dispatch and the task would exist, list, and never run. */
int fr_tasks_name_is_reserved(const char *name);

typedef struct {
    const fr_task_node **nodes;
    size_t count;
} fr_task_plan;

/* Resolves goal's transitive closure and orders it so that every edge runs
   before the task it points at. An edge naming nothing, a cycle, or a goal
   nothing declares is refused here, before anything runs: a graph error must
   never leave half a build behind. */
int fr_tasks_plan(const fr_task_set *set, const char *goal, fr_task_plan *out, fr_error *err);
void fr_tasks_plan_free(fr_task_plan *plan);

/* Runs a plan in order. Before each run-bearing task, its own toolchain's
   derived directory is ensured to exist, every iteration: generation may have
   created nothing for it (a compiler-driven toolchain's ordinary case), and
   ensuring an already-existing directory is a no-op, not an error. A task
   whose run raises stops the run: tasks already run stay run, and nothing is
   undone. */
int fr_tasks_run(const fr_task_plan *plan, const fr_session *session, fr_error *err);

#endif
