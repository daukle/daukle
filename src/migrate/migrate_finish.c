#include "migrate/migrate_internal.h"

#include "cache/cache.h"
#include "config/toml_write.h"
#include "migrate/proof.h"
#include "plugin/plugins.h"
#include "project/derived.h"
#include "project/region.h"
#include "project/sync.h"
#include "project/tasks.h"
#include "util/error.h"
#include "util/strbuf.h"
#include "util/tree.h"

#include "cJSON.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FR_MIGRATE_MANIFEST "daukle.toml"
#define FR_MIGRATE_OLD_SHOWN "build/daukle/" FR_DERIVED_MIGRATE_OLD_NAME
#define FR_MIGRATE_FAILURE_SIZE 1024

static const char *const FIRST_KEYS[] = { "schema", "project", "version", "modules", "plugins" };

static void report_unproved(const char *what) {
    fprintf(stderr, "daukle migrate: %s; daukle.toml is kept for inspection and nothing was"
                    " removed\n", what);
}

static int write_manifest(const fr_migrate_state *state, fr_error *err) {
    char *text = NULL;
    if (fr_toml_write(state->manifest, FIRST_KEYS, sizeof FIRST_KEYS / sizeof *FIRST_KEYS, &text,
                      err) != FR_OK) {
        return FR_ERR;
    }
    int status = fr_file_write_text(FR_MIGRATE_MANIFEST, text, err);
    free(text);
    return status;
}

static const char *proof_task(const fr_migrate_kept *kept) {
    const cJSON *proof = cJSON_GetObjectItemCaseSensitive(kept->result, "proof");
    return cJSON_GetObjectItemCaseSensitive(proof, "task")->valuestring;
}

/* Generation runs once, before the first proof task, the way a task run syncs
   before it runs. */
static int run_proof_task(const fr_session *session, const fr_task_set *set, const char *task,
                          int *synced, fr_error *err) {
    fr_task_plan plan;
    if (fr_tasks_plan(set, task, &plan, err) != FR_OK) return FR_ERR;
    if (!*synced) {
        fr_sync_report report;
        int status = fr_sync_session(session, 1, &report, err);
        fr_sync_report_free(&report);
        if (status != FR_OK) {
            fr_tasks_plan_free(&plan);
            return FR_ERR;
        }
        *synced = 1;
    }
    int status = fr_tasks_run(&plan, session, err);
    fr_tasks_plan_free(&plan);
    return status;
}

/* The new build failing is an answer about the project, not a failure of
   migrate, so it fills failure and returns 0 rather than raising FR_ERR. */
static int run_proofs(const fr_migrate_state *state, char *failure, size_t failure_size) {
    fr_error err;
    fr_session session;
    if (fr_session_open(FR_MIGRATE_MANIFEST, state->options->use_cache, &session, &err) != FR_OK) {
        snprintf(failure, failure_size, "the new manifest did not load: %s", err.message);
        return 0;
    }
    fr_task_set set;
    if (fr_tasks_collect(session.registry, &session.manifest, &set, &err) != FR_OK) {
        fr_session_close(&session);
        snprintf(failure, failure_size, "the new manifest's tasks did not collect: %s", err.message);
        return 0;
    }
    int synced = 0;
    int proved = 1;
    for (size_t index = 0; proved && index < state->kept_count; index++) {
        const char *task = proof_task(&state->kept[index]);
        if (run_proof_task(&session, &set, task, &synced, &err) != FR_OK) {
            snprintf(failure, failure_size, "the proof task \"%s\" failed: %s", task, err.message);
            proved = 0;
        }
    }
    fr_tasks_set_free(&set);
    fr_session_close(&session);
    return proved;
}

static void compare_entry(const cJSON *entry, fr_strbuf *differences, size_t *count) {
    const char *old_path = cJSON_GetObjectItemCaseSensitive(entry, "old")->valuestring;
    const char *new_path = cJSON_GetObjectItemCaseSensitive(entry, "new")->valuestring;
    const char *as = cJSON_GetObjectItemCaseSensitive(entry, "as")->valuestring;
    fr_proof_kind kind;
    if (fr_proof_kind_parse(as, &kind) != FR_OK) {
        fr_strbuf_append_format(differences, "  %s: cannot be compared as \"%s\"\n", old_path, as);
        (*count)++;
        return;
    }
    int same = 0;
    char difference[FR_MIGRATE_FAILURE_SIZE];
    difference[0] = '\0';
    fr_error err;
    if (fr_proof_compare(".", old_path, new_path, kind, &same, difference, sizeof difference, &err)
        != FR_OK) {
        fr_strbuf_append_format(differences, "  %s\n", err.message);
        (*count)++;
    } else if (!same) {
        fr_strbuf_append_format(differences, "  %s\n", difference);
        (*count)++;
    }
}

static int compare_artifacts(const fr_migrate_state *state, int *out_same, fr_error *err) {
    fr_strbuf differences;
    fr_strbuf_init(&differences);
    size_t count = 0;
    for (size_t index = 0; index < state->kept_count; index++) {
        const cJSON *proof = cJSON_GetObjectItemCaseSensitive(state->kept[index].result, "proof");
        const cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(proof, "compare")) {
            compare_entry(entry, &differences, &count);
        }
    }
    char *text = fr_strbuf_release(&differences);
    if (text == NULL) {
        fr_error_set(err, "out of memory comparing the old build with the new one");
        return FR_ERR;
    }
    *out_same = count == 0;
    if (count > 0) {
        report_unproved("the new build differs from the old one");
        fputs(text, stderr);
    }
    free(text);
    return FR_OK;
}

static char *old_home_of(const char *path) {
    size_t length = strlen(FR_MIGRATE_OLD_SHOWN) + 1 + strlen(path) + 1;
    char *target = malloc(length);
    if (target != NULL) snprintf(target, length, "%s/%s", FR_MIGRATE_OLD_SHOWN, path);
    return target;
}

static void make_parent_directories(const char *target) {
    char *parent = fr_dup_string(target);
    if (parent == NULL) return;
    char *last = strrchr(parent, '/');
    if (last != NULL) {
        *last = '\0';
        fr_cache_make_directories(parent);
    }
    free(parent);
}

static int move_one(const char *path, fr_strbuf *moved, fr_error *err) {
    char *target = old_home_of(path);
    if (target == NULL) {
        fr_error_set(err, "out of memory moving \"%s\" aside", path);
        return FR_ERR;
    }
    make_parent_directories(target);
    int status = FR_OK;
    if (rename(path, target) != 0) {
        char *already = fr_strbuf_release(moved);
        fr_strbuf_init(moved);
        fr_error_set(err, "could not move \"%s\" to %s (%s); already moved there: %s", path,
                     target, strerror(errno),
                     already != NULL && already[0] != '\0' ? already : "nothing");
        free(already);
        status = FR_ERR;
    } else {
        fr_strbuf_append_format(moved, "%s\"%s\"", moved->length == 0 ? "" : ", ", path);
    }
    free(target);
    return status;
}

/* A failed rename stops the move and names what had already gone, so the user
   can tell which files are in build/daukle/migrate/old and which are not. */
static int move_aside(const fr_migrate_state *state, fr_error *err) {
    fr_strbuf moved;
    fr_strbuf_init(&moved);
    for (size_t index = 0; index < state->kept_count; index++) {
        const cJSON *path = NULL;
        cJSON_ArrayForEach(path, cJSON_GetObjectItemCaseSensitive(state->kept[index].result,
                                                                  "owned")) {
            if (move_one(path->valuestring, &moved, err) != FR_OK) {
                fr_strbuf_free(&moved);
                return FR_ERR;
            }
        }
    }
    fr_strbuf_free(&moved);
    return FR_OK;
}

int fr_migrate_finish(fr_migrate_state *state, fr_migrate_outcome *out_outcome, fr_error *err) {
    if (write_manifest(state, err) != FR_OK) return FR_ERR;

    *out_outcome = FR_MIGRATE_UNPROVED;
    char failure[FR_MIGRATE_FAILURE_SIZE];
    if (!run_proofs(state, failure, sizeof failure)) {
        report_unproved(failure);
        return FR_OK;
    }
    int same = 0;
    if (compare_artifacts(state, &same, err) != FR_OK) return FR_ERR;
    if (!same) return FR_OK;

    if (move_aside(state, err) != FR_OK) return FR_ERR;
    *out_outcome = FR_MIGRATE_DONE;
    printf("daukle: migrated; the replaced files are in " FR_MIGRATE_OLD_SHOWN " until"
           " \"daukle migrate --done\"\n");
    return FR_OK;
}

int fr_migrate_done(fr_error *err) {
    if (!fr_cache_directory_exists(FR_MIGRATE_OLD_SHOWN)) {
        fr_error_set(err, "there is no unfinished migration here: " FR_MIGRATE_OLD_SHOWN
                          " does not exist");
        return FR_ERR;
    }
    fr_remove_tree(FR_MIGRATE_OLD_SHOWN);
    if (fr_cache_directory_exists(FR_MIGRATE_OLD_SHOWN)) {
        fr_error_set(err, "could not fully remove " FR_MIGRATE_OLD_SHOWN "; what is left there is"
                          " still the old tool's files");
        return FR_ERR;
    }
    return FR_OK;
}
