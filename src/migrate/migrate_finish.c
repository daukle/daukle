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

static int returned_any_files(const fr_migrate_state *state) {
    for (size_t index = 0; index < state->kept_count; index++) {
        const cJSON *files = cJSON_GetObjectItemCaseSensitive(state->kept[index].result, "files");
        if (files != NULL && cJSON_GetArraySize(files) > 0) return 1;
    }
    return 0;
}

/** @implNote the wording without files is byte for byte what plugins released
    before `files` assert on, so it must not change. */
static void report_unproved(const fr_migrate_state *state, const char *what) {
    const char *kept = returned_any_files(state) ? "daukle.toml and the migrator's files are"
                                                  : "daukle.toml is";
    fprintf(stderr, "daukle migrate: %s; %s kept for inspection and nothing was removed\n", what,
            kept);
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
        report_unproved(state, "the new build differs from the old one");
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

static int move_one(const char *path, fr_error *err) {
    char *target = old_home_of(path);
    if (target == NULL) {
        fr_error_set(err, "out of memory moving \"%s\" aside", path);
        return FR_ERR;
    }
    make_parent_directories(target);
    int status = FR_OK;
    if (rename(path, target) != 0) {
        fr_error_set(err, "could not move \"%s\" to %s (%s); what had already moved is listed"
                          " above", path, target, strerror(errno));
        status = FR_ERR;
    }
    free(target);
    return status;
}

static void print_already_moved(const fr_migrate_state *state, size_t moved) {
    fprintf(stderr, "daukle migrate: moving the old files stopped; already in " FR_MIGRATE_OLD_SHOWN
                    ":%s\n", moved == 0 ? " nothing" : "");
    for (size_t index = 0; moved > 0 && index < state->kept_count; index++) {
        const cJSON *path = NULL;
        cJSON_ArrayForEach(path, cJSON_GetObjectItemCaseSensitive(state->kept[index].result,
                                                                  "owned")) {
            if (moved == 0) break;
            fprintf(stderr, "  %s\n", path->valuestring);
            moved--;
        }
    }
}

/* fr_migrate_check_owned has refused every predictable failure before
   anything was written, so a rename failing here is the filesystem's; every
   path already moved is printed, since the error itself has room for one. */
static int move_aside(const fr_migrate_state *state, fr_error *err) {
    if (fr_derived_check_real(".", FR_DERIVED_MIGRATE_OLD_NAME, err) != FR_OK) return FR_ERR;
    size_t moved = 0;
    for (size_t index = 0; index < state->kept_count; index++) {
        const cJSON *path = NULL;
        cJSON_ArrayForEach(path, cJSON_GetObjectItemCaseSensitive(state->kept[index].result,
                                                                  "owned")) {
            if (move_one(path->valuestring, err) != FR_OK) {
                print_already_moved(state, moved);
                return FR_ERR;
            }
            moved++;
        }
    }
    return FR_OK;
}

static int path_exists(const char *path) {
    return fr_cache_file_exists(path) || fr_cache_directory_exists(path);
}

static int fold(int character) {
    return character >= 'A' && character <= 'Z' ? character - 'A' + 'a' : character;
}

/* Equal, or one a directory holding the other. Case is folded because the
   owned-path refusal folds it: on a case-insensitive disk the two are one. */
static int paths_nest(const char *left, const char *right) {
    size_t index = 0;
    while (left[index] != '\0' && right[index] != '\0'
           && fold((unsigned char) left[index]) == fold((unsigned char) right[index])) {
        index++;
    }
    if (left[index] == '\0' && right[index] == '\0') return 1;
    if (left[index] == '\0') return right[index] == '/';
    if (right[index] == '\0') return left[index] == '/';
    return 0;
}

static int owned_blocker(fr_migrate_state *state, const char *label, const char *path,
                         const char *why, fr_error *err) {
    char source[512];
    snprintf(source, sizeof source, "migrator %s", label);
    return fr_blockers_add(&state->blockers, source, "owned", path, why, NULL, err);
}

static int nests_in_another(const fr_migrate_state *state, size_t kept_index,
                            const cJSON *path) {
    for (size_t index = 0; index <= kept_index; index++) {
        const cJSON *other = NULL;
        cJSON_ArrayForEach(other, cJSON_GetObjectItemCaseSensitive(state->kept[index].result,
                                                                   "owned")) {
            if (other == path) return 0;
            if (paths_nest(other->valuestring, path->valuestring)) return 1;
        }
    }
    return 0;
}

static int check_one_owned(fr_migrate_state *state, size_t kept_index, const cJSON *path,
                           fr_error *err) {
    const char *label = state->kept[kept_index].label;
    const char *text = path->valuestring;
    if (!path_exists(text)) {
        return owned_blocker(state, label, text, "the migrator hands it back to be moved aside,"
                                                 " but it does not exist", err);
    }
    if (nests_in_another(state, kept_index, path)) {
        return owned_blocker(state, label, text, "it is, or lies inside, another owned path, so"
                                                 " moving both aside cannot succeed", err);
    }
    char *target = old_home_of(text);
    if (target == NULL) {
        fr_error_set(err, "out of memory checking where \"%s\" would be moved", text);
        return FR_ERR;
    }
    int taken = path_exists(target);
    free(target);
    if (!taken) return FR_OK;
    return owned_blocker(state, label, text, "an earlier migration's copy already sits at its"
                                             " place in " FR_MIGRATE_OLD_SHOWN, err);
}

int fr_migrate_check_owned(fr_migrate_state *state, fr_error *err) {
    fr_error unsafe;
    if (fr_derived_check_real(".", FR_DERIVED_MIGRATE_OLD_NAME, &unsafe) != FR_OK) {
        return fr_migrate_core_blocker(state, "derived directory", FR_MIGRATE_OLD_SHOWN,
                                       unsafe.message, err);
    }
    for (size_t index = 0; index < state->kept_count; index++) {
        const cJSON *path = NULL;
        cJSON_ArrayForEach(path, cJSON_GetObjectItemCaseSensitive(state->kept[index].result,
                                                                  "owned")) {
            if (check_one_owned(state, index, path, err) != FR_OK) return FR_ERR;
        }
    }
    return FR_OK;
}

static int write_carried_file(const cJSON *entry, fr_error *err) {
    const char *path = cJSON_GetObjectItemCaseSensitive(entry, "path")->valuestring;
    const char *text = cJSON_GetObjectItemCaseSensitive(entry, "text")->valuestring;
    make_parent_directories(path);
    return fr_file_write_text(path, text, err);
}

/** The paths were checked by fr_migrate_check_run_result, which confines each
    to daukle/<instance>/, so nothing here can reach the old tool's files. */
static int write_files(const fr_migrate_state *state, fr_error *err) {
    for (size_t index = 0; index < state->kept_count; index++) {
        const cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(state->kept[index].result,
                                                                   "files")) {
            if (write_carried_file(entry, err) != FR_OK) return FR_ERR;
        }
    }
    return FR_OK;
}

int fr_migrate_finish(fr_migrate_state *state, fr_migrate_outcome *out_outcome, fr_error *err) {
    if (write_manifest(state, err) != FR_OK) return FR_ERR;
    if (write_files(state, err) != FR_OK) return FR_ERR;

    *out_outcome = FR_MIGRATE_UNPROVED;
    char failure[FR_MIGRATE_FAILURE_SIZE];
    if (!run_proofs(state, failure, sizeof failure)) {
        report_unproved(state, failure);
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
    if (fr_derived_check_real(".", FR_DERIVED_MIGRATE_OLD_NAME, err) != FR_OK) return FR_ERR;
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
