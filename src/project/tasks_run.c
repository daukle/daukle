#include "project/tasks.h"

#include "config/config_lua.h"
#include "plugin/resolve.h"
#include "project/derived.h"
#include "project/task_command.h"
#include "project/toolchain_root.h"
#include "util/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int run_node(const fr_task_node *node, const fr_session *session, fr_error *err) {
    /* A manifest task has no toolchain, so there is no derived directory
       to ensure before it and the step below is skipped rather than given
       a default. D-53 section 4. */
    if (node->run != NULL) return fr_task_command_run(node->run, session->manifest_dir, node->name, err);
    if (node->plugin == NULL || node->plugin->run == NULL) return FR_OK;

    if (node->publish != NULL) {
        fprintf(stderr, "daukle publish: %s (from %s)\n", node->publish->name,
                node->publish->from);
    }

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

    char *derived_dir_relative = NULL;
    if (fr_derived_dir_relative(node->toolchain->name, &derived_dir_relative, err) != FR_OK) {
        free(derived_dir);
        return FR_ERR;
    }

    char *generated_dir_relative = NULL;
    if (fr_generated_dir_relative(node->toolchain->name, &generated_dir_relative, err)
        != FR_OK) {
        free(derived_dir_relative);
        free(derived_dir);
        return FR_ERR;
    }

    fr_resolved *resolved = NULL;
    size_t resolved_count = 0;
    if (fr_resolve_toolchain(node->toolchain, &session->manifest, session->manifest_dir,
                             session->registry, &resolved, &resolved_count, err) != FR_OK) {
        free(generated_dir_relative);
        free(derived_dir_relative);
        free(derived_dir);
        return FR_ERR;
    }

    char version[64];
    snprintf(version, sizeof version, "%d.%d.%d", session->manifest.self.version.major,
             session->manifest.self.version.minor, session->manifest.self.version.patch);

    char *root = NULL;
    if (fr_toolchain_root(node->toolchain, &root, err) != FR_OK) {
        fr_resolved_free(resolved, resolved_count);
        free(generated_dir_relative);
        free(derived_dir_relative);
        free(derived_dir);
        return FR_ERR;
    }

    fr_task_run_context context;
    context.name = node->name;
    context.toolchain = node->toolchain;
    context.publish = node->publish;
    context.project = session->manifest.self.project;
    context.version = version;
    context.root = root;
    context.derived_dir_relative = derived_dir_relative;
    context.generated_dir_relative = generated_dir_relative;
    context.resolved = resolved;
    context.resolved_count = resolved_count;
    context.modules = session->manifest.self.modules;
    context.module_count = session->manifest.self.module_count;

    int status = node->plugin->run(node->plugin->state, &context, err);
    free(root);
    fr_resolved_free(resolved, resolved_count);
    free(generated_dir_relative);
    free(derived_dir_relative);
    free(derived_dir);
    if (status != FR_OK) {
        char original[sizeof err->message];
        memcpy(original, err->message, sizeof original);
        fr_error_set(err, "task \"%s\": %s", node->name, original);
        return FR_ERR;
    }
    return FR_OK;
}

typedef enum { NODE_NOT_STARTED, NODE_RUNNING, NODE_COMPLETED } node_progress;

/* One invocation's record of every node of the set, shared by its plan and every plan a
   running task starts through daukle.run_task, so a task runs at most once per invocation. */
typedef struct {
    const fr_task_set *set;
    const fr_session *session;
    unsigned char *progress;
} task_run;

static task_run *active_run;

static int run_plan(task_run *run, const fr_task_plan *plan, fr_error *err) {
    for (size_t index = 0; index < plan->count; index++) {
        const fr_task_node *node = plan->nodes[index];
        size_t slot = (size_t) (node - run->set->nodes);
        if (run->progress[slot] == NODE_COMPLETED) continue;
        if (run->progress[slot] == NODE_RUNNING) {
            fr_error_set(err, "task \"%s\" is already running, so running it again from inside it"
                              " would never end", node->name);
            return FR_ERR;
        }
        run->progress[slot] = NODE_RUNNING;
        int status = run_node(node, run->session, err);
        run->progress[slot] = status == FR_OK ? NODE_COMPLETED : NODE_NOT_STARTED;
        if (status != FR_OK) return FR_ERR;
    }
    return FR_OK;
}

static int run_nested(const char *goal, fr_error *err) {
    if (active_run == NULL) {
        fr_error_set(err, "no task run is in progress to run \"%s\" in", goal);
        return FR_ERR;
    }
    fr_task_plan plan;
    if (fr_tasks_plan(active_run->set, goal, &plan, err) != FR_OK) return FR_ERR;
    int status = run_plan(active_run, &plan, err);
    fr_tasks_plan_free(&plan);
    return status;
}

int fr_tasks_run(const fr_task_set *set, const fr_task_plan *plan, const fr_session *session,
                 fr_error *err) {
    task_run run = { set, session, calloc(set->count > 0 ? set->count : 1, 1) };
    if (run.progress == NULL) {
        fr_error_set(err, "out of memory starting a task run");
        return FR_ERR;
    }
    task_run *outer = active_run;
    active_run = &run;
    fr_lua_set_task_runner(run_nested);
    int status = run_plan(&run, plan, err);
    active_run = outer;
    if (outer == NULL) fr_lua_set_task_runner(NULL);
    free(run.progress);
    return status;
}

int fr_tasks_check_publish(const fr_task_set *set, const fr_manifest *manifest,
                           const char *only, fr_error *err) {
    for (size_t index = 0; index < manifest->publish_count; index++) {
        const fr_publish_target *target = &manifest->publishes[index];
        if (only != NULL && strcmp(only, target->name) != 0) continue;
        char goal[160];
        if (!fr_tasks_publish_goal(target->name, goal, sizeof goal)) {
            fr_error_set(err, "the publish destination \"%s\" is too long", target->name);
            return FR_ERR;
        }
        if (fr_tasks_find(set, goal) == NULL) {
            fr_error_set(err, "no plugin declares a publisher named \"%s\", which [publish.%s]"
                              " needs", target->name, target->name);
            return FR_ERR;
        }
        fr_task_plan plan;
        if (fr_tasks_plan(set, goal, &plan, err) != FR_OK) return FR_ERR;
        fr_tasks_plan_free(&plan);
    }
    return FR_OK;
}

int fr_tasks_run_publish(const fr_task_set *set, const fr_session *session, const char *only,
                         fr_publish_done_fn done, void *done_state, fr_error *err) {
    for (size_t index = 0; index < session->manifest.publish_count; index++) {
        const fr_publish_target *target = &session->manifest.publishes[index];
        if (only != NULL && strcmp(only, target->name) != 0) continue;

        char goal[160];
        if (!fr_tasks_publish_goal(target->name, goal, sizeof goal)) {
            fr_error_set(err, "the publish destination \"%s\" is too long", target->name);
            return FR_ERR;
        }

        fr_task_plan plan;
        if (fr_tasks_plan(set, goal, &plan, err) != FR_OK) return FR_ERR;
        int status = fr_tasks_run(set, &plan, session, err);
        fr_tasks_plan_free(&plan);
        if (status != FR_OK) return FR_ERR;

        if (done != NULL) done(goal, done_state);
    }
    return FR_OK;
}
