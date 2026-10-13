#include "project/tasks.h"

#include "plugin/resolve.h"
#include "project/derived.h"
#include "project/task_command.h"
#include "util/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int fr_tasks_run(const fr_task_plan *plan, const fr_session *session, fr_error *err) {
    for (size_t index = 0; index < plan->count; index++) {
        const fr_task_node *node = plan->nodes[index];
        /* A manifest task has no toolchain, so there is no derived directory
           to ensure before it and the step below is skipped rather than given
           a default. D-53 section 4. */
        if (node->run != NULL) {
            if (fr_task_command_run(node->run, session->manifest_dir, node->name, err) != FR_OK) {
                return FR_ERR;
            }
            continue;
        }
        if (node->plugin == NULL || node->plugin->run == NULL) continue;

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

        fr_task_run_context context;
        context.name = node->name;
        context.toolchain = node->toolchain;
        context.publish = node->publish;
        context.project = session->manifest.self.project;
        context.version = version;
        context.root = FR_DERIVED_ROOT_RELATIVE;
        context.derived_dir_relative = derived_dir_relative;
        context.generated_dir_relative = generated_dir_relative;
        context.resolved = resolved;
        context.resolved_count = resolved_count;
        context.modules = session->manifest.self.modules;
        context.module_count = session->manifest.self.module_count;

        int status = node->plugin->run(node->plugin->state, &context, err);
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
    }
    return FR_OK;
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
        int status = fr_tasks_run(&plan, session, err);
        fr_tasks_plan_free(&plan);
        if (status != FR_OK) return FR_ERR;

        if (done != NULL) done(goal, done_state);
    }
    return FR_OK;
}
