#include "cli/cli_internal.h"

#include "plugin/plugins.h"
#include "project/sync.h"
#include "project/tasks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int fr_cli_run_task(const char *task_name, int use_cache, int verbose) {
    fr_error err;
    char *resolved = NULL;
    if (fr_cli_resolve_manifest_path(NULL, &resolved, &err) != FR_OK) {
        fr_cli_report_error(&err, verbose);
        return 1;
    }

    fr_session session;
    int opened = fr_session_open(resolved, use_cache, &session, &err);
    free(resolved);
    if (opened != FR_OK) {
        fr_cli_report_error(&err, verbose);
        return 1;
    }

    fr_task_set set;
    if (fr_tasks_collect(session.registry, &session.manifest, &set, &err) != FR_OK) {
        fr_session_close(&session);
        fr_cli_report_error(&err, verbose);
        return 1;
    }

    fr_task_plan plan;
    if (fr_tasks_plan(&set, task_name, &plan, &err) != FR_OK) {
        int unknown = fr_tasks_find(&set, task_name) == NULL;
        size_t plugin_count = fr_plugins_report_declared_count(fr_plugins_report());
        fr_tasks_set_free(&set);
        fr_session_close(&session);
        fr_cli_report_error(&err, verbose);
        if (unknown) {
            char message[256];
            fr_tasks_unknown_message(task_name, plugin_count, message, sizeof message);
            fprintf(stderr, "  %s\n", message);
            fprintf(stderr, "  run \"daukle tasks\" to see what they provide\n");
            return 2;
        }
        return 1;
    }

    /* The plan is known good before anything is written: a task that does not
       exist must never trigger the write a real one would have caused. */
    fr_sync_report report;
    if (fr_sync_session(&session, 1, &report, &err) != FR_OK) {
        fr_sync_report_free(&report);
        fr_tasks_plan_free(&plan);
        fr_tasks_set_free(&set);
        fr_session_close(&session);
        fr_cli_report_error(&err, verbose);
        return 1;
    }
    fr_sync_report_free(&report);

    int status = fr_tasks_run(&plan, &session, &err);
    fr_tasks_plan_free(&plan);
    fr_tasks_set_free(&set);
    fr_session_close(&session);
    if (status != FR_OK) {
        fr_cli_report_error(&err, verbose);
        return 1;
    }
    printf("daukle: %s\n", task_name);
    return 0;
}

static void confirm_published(const char *goal, void *state) {
    (void) state;
    printf("daukle: %s\n", goal);
}

int fr_cli_run_publish(const char *only, int use_cache, int verbose) {
    fr_error err;
    char *resolved = NULL;
    if (fr_cli_resolve_manifest_path(NULL, &resolved, &err) != FR_OK) {
        fr_cli_report_error(&err, verbose);
        return 1;
    }

    fr_session session;
    int opened = fr_session_open(resolved, use_cache, &session, &err);
    free(resolved);
    if (opened != FR_OK) {
        fr_cli_report_error(&err, verbose);
        return 1;
    }

    fr_task_set set;
    if (fr_tasks_collect(session.registry, &session.manifest, &set, &err) != FR_OK) {
        fr_session_close(&session);
        fr_cli_report_error(&err, verbose);
        return 1;
    }

    int matched = 0;
    for (size_t index = 0; index < session.manifest.publish_count; index++) {
        if (only == NULL || strcmp(only, session.manifest.publishes[index].name) == 0) matched++;
    }
    if (matched == 0) {
        fr_tasks_set_free(&set);
        if (only == NULL) {
            fprintf(stderr, "daukle: this project declares no publish destinations\n");
        } else {
            fprintf(stderr, "daukle: this project declares no publish destination \"%s\"\n", only);
        }
        fr_session_close(&session);
        return 2;
    }

    if (fr_tasks_check_publish(&set, &session.manifest, only, &err) != FR_OK) {
        fr_tasks_set_free(&set);
        fr_session_close(&session);
        fr_cli_report_error(&err, verbose);
        return 1;
    }

    fr_sync_report report;
    if (fr_sync_session(&session, 1, &report, &err) != FR_OK) {
        fr_sync_report_free(&report);
        fr_tasks_set_free(&set);
        fr_session_close(&session);
        fr_cli_report_error(&err, verbose);
        return 1;
    }
    fr_sync_report_free(&report);

    if (fr_tasks_run_publish(&set, &session, only, confirm_published, NULL, &err) != FR_OK) {
        fr_tasks_set_free(&set);
        fr_session_close(&session);
        fr_cli_report_error(&err, verbose);
        return 1;
    }

    fr_tasks_set_free(&set);
    fr_session_close(&session);
    return 0;
}

#define FR_TASKS_JOINER_CAPACITY 32

/* label reads in the direction that kind actually runs: FR_TASK_JOIN_PART_OF
   is what name pulls in (they run before name), FR_TASK_JOIN_DEPENDS_ON is
   what pulls name in (name runs before them). Printing the true total past
   FR_TASKS_JOINER_CAPACITY, rather than staying silent about it, is what
   keeps a truncated list from being mistaken for a complete one. */
static void print_task_joiners(const fr_task_set *set, const char *name,
                               fr_task_join_kind kind, const char *label) {
    const char *joiners[FR_TASKS_JOINER_CAPACITY];
    size_t total = fr_tasks_joiners(set, name, kind, joiners, FR_TASKS_JOINER_CAPACITY);
    size_t shown = total < FR_TASKS_JOINER_CAPACITY ? total : FR_TASKS_JOINER_CAPACITY;
    for (size_t index = 0; index < shown; index++) {
        printf("  %s: %s\n", label, joiners[index]);
    }
    if (total > shown) {
        printf("  ... and %zu more not shown\n", total - shown);
    }
}

int fr_cli_list_tasks(int use_cache, int verbose) {
    fr_error err;
    char *resolved = NULL;
    if (fr_cli_resolve_manifest_path(NULL, &resolved, &err) != FR_OK) {
        fr_cli_report_error(&err, verbose);
        return 1;
    }
    fr_session session;
    int opened = fr_session_open(resolved, use_cache, &session, &err);
    free(resolved);
    if (opened != FR_OK) {
        fr_cli_report_error(&err, verbose);
        return 1;
    }

    fr_task_set set;
    if (fr_tasks_collect(session.registry, &session.manifest, &set, &err) != FR_OK) {
        fr_session_close(&session);
        fr_cli_report_error(&err, verbose);
        return 1;
    }

    if (set.count == 0) {
        printf("daukle: this project has no tasks\n");
    } else {
        printf("daukle: %zu task%s\n", set.count, set.count == 1 ? "" : "s");
    }
    for (size_t index = 0; index < set.count; index++) {
        if (index > 0) printf("\n");
        const fr_task_node *node = &set.nodes[index];
        printf("%s%s\n", node->name, node->plugin == NULL ? " (from the manifest)" : "");
        int runs_something = node->run != NULL
                             || (node->plugin != NULL && node->plugin->run != NULL);
        printf("  runs: %s\n", runs_something ? "a program" : "nothing of its own");
        if (node->run != NULL) printf("  program: %s\n", node->run->tool);
        for (size_t edge = 0; edge < node->depends_on_count; edge++) {
            printf("  after: %s\n", node->depends_on[edge]);
        }
        for (size_t edge = 0; edge < node->extra_depends_on_count; edge++) {
            printf("  after: %s (from the manifest)\n", node->extra_depends_on[edge]);
        }
        if (node->part_of != NULL) printf("  part of: %s\n", node->part_of);
        if (node->extra_part_of != NULL) printf("  part of: %s (from the manifest)\n", node->extra_part_of);

        print_task_joiners(&set, node->name, FR_TASK_JOIN_PART_OF, "pulls in");
        print_task_joiners(&set, node->name, FR_TASK_JOIN_DEPENDS_ON, "needed by");
    }

    fr_tasks_set_free(&set);
    fr_session_close(&session);
    return 0;
}
