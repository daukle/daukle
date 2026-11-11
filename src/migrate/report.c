#include "migrate/report.h"

#include "migrate/proof.h"
#include "project/region.h"
#include "util/error.h"

#include "cJSON.h"

#include <stdlib.h>

#define FR_MIGRATE_REPORT_VERSION 1

static int out_of_memory(fr_error *err) {
    fr_error_set(err, "out of memory writing the migrate report");
    return FR_ERR;
}

static const char *string_field(const cJSON *entry, const char *name) {
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(entry, name);
    return cJSON_IsString(field) ? field->valuestring : "";
}

static void add_differences(cJSON *record, const cJSON *entry) {
    fr_proof_kind kind;
    if (fr_proof_kind_parse(string_field(entry, "as"), &kind) != FR_OK) {
        cJSON_AddStringToObject(record, "error", "not a comparison kind");
        return;
    }
    fr_proof_differences list;
    fr_error compare_err;
    if (fr_proof_list_differences(".", string_field(entry, "old"), string_field(entry, "new"),
                                  kind, &list, &compare_err) != FR_OK) {
        cJSON_AddStringToObject(record, "error", compare_err.message);
        return;
    }
    cJSON *differences = fr_proof_differences_json(&list);
    if (differences == NULL) cJSON_AddStringToObject(record, "error", "out of memory listing the differences");
    cJSON_AddBoolToObject(record, "same", list.count == 0);
    cJSON_AddItemToObject(record, "differences", differences);
    cJSON_AddBoolToObject(record, "truncated", list.truncated);
    fr_proof_differences_free(&list);
}

int fr_migrate_report_comparison(fr_migrate_state *state, const char *instance,
                                 const cJSON *entry, fr_error *err) {
    if (state->compared == NULL) state->compared = cJSON_CreateArray();
    cJSON *record = cJSON_CreateObject();
    if (state->compared == NULL || record == NULL) {
        cJSON_Delete(record);
        return out_of_memory(err);
    }
    cJSON_AddStringToObject(record, "instance", instance);
    cJSON_AddStringToObject(record, "old", string_field(entry, "old"));
    cJSON_AddStringToObject(record, "new", string_field(entry, "new"));
    cJSON_AddStringToObject(record, "as", string_field(entry, "as"));
    add_differences(record, entry);
    cJSON_AddItemToArray(state->compared, record);
    return FR_OK;
}

static const char *outcome_name(const fr_migrate_state *state, int status,
                                fr_migrate_outcome outcome) {
    if (status != FR_OK) return state->undetected ? "undetected" : "error";
    switch (outcome) {
        case FR_MIGRATE_DONE:
            return "migrated";
        case FR_MIGRATE_BLOCKED:
            return "blocked";
        case FR_MIGRATE_UNPROVED:
            return "unproved";
    }
    return "error";
}

static cJSON *detected_json(const fr_migrate_state *state) {
    cJSON *array = cJSON_CreateArray();
    for (size_t index = 0; array != NULL && index < state->detections.count; index++) {
        const fr_detection *detection = &state->detections.items[index];
        cJSON *entry = cJSON_CreateObject();
        if (entry == NULL) break;
        cJSON_AddStringToObject(entry, "plugin", detection->plugin->id);
        cJSON_AddStringToObject(entry, "dir", detection->dir);
        cJSON_AddItemToArray(array, entry);
    }
    return array;
}

static void add_optional_string(cJSON *object, const char *name, const char *value) {
    if (value != NULL) cJSON_AddStringToObject(object, name, value);
    else cJSON_AddNullToObject(object, name);
}

static cJSON *blockers_json(const fr_blockers *blockers) {
    cJSON *array = cJSON_CreateArray();
    for (size_t index = 0; array != NULL && index < blockers->count; index++) {
        const fr_blocker *blocker = &blockers->items[index];
        cJSON *entry = cJSON_CreateObject();
        if (entry == NULL) break;
        add_optional_string(entry, "source", blocker->source);
        add_optional_string(entry, "construct", blocker->construct);
        add_optional_string(entry, "where", blocker->where);
        add_optional_string(entry, "why", blocker->why);
        add_optional_string(entry, "surface", blocker->surface);
        cJSON_AddItemToArray(array, entry);
    }
    return array;
}

static cJSON *proof_json(const fr_migrate_state *state) {
    cJSON *proof = cJSON_CreateObject();
    if (proof == NULL) return NULL;
    if (state->proof_failure != NULL) {
        cJSON_AddStringToObject(proof, "failure", state->proof_failure);
    }
    cJSON *compared = state->compared != NULL ? cJSON_Duplicate(state->compared, 1)
                                              : cJSON_CreateArray();
    cJSON_AddItemToObject(proof, "compared", compared);
    return proof;
}

static const cJSON *notes_of(const fr_migrate_kept *kept) {
    return cJSON_GetObjectItemCaseSensitive(kept->result, "notes");
}

void fr_migrate_notes_print(const fr_migrate_state *state, FILE *out) {
    for (size_t index = 0; index < state->kept_count; index++) {
        const cJSON *note = NULL;
        cJSON_ArrayForEach(note, notes_of(&state->kept[index])) {
            if (cJSON_IsString(note)) {
                fprintf(out, "daukle migrate: migrator %s: %s\n", state->kept[index].label,
                        note->valuestring);
            }
        }
    }
}

static cJSON *notes_json(const fr_migrate_state *state) {
    cJSON *array = cJSON_CreateArray();
    if (array == NULL) return NULL;
    char source[256];
    for (size_t index = 0; index < state->kept_count; index++) {
        snprintf(source, sizeof source, "migrator %s", state->kept[index].label);
        const cJSON *note = NULL;
        cJSON_ArrayForEach(note, notes_of(&state->kept[index])) {
            if (!cJSON_IsString(note)) continue;
            cJSON *entry = cJSON_CreateObject();
            if (entry == NULL) continue;
            cJSON_AddStringToObject(entry, "source", source);
            cJSON_AddStringToObject(entry, "note", note->valuestring);
            cJSON_AddItemToArray(array, entry);
        }
    }
    return array;
}

int fr_migrate_report_write(const char *path, const fr_migrate_state *state, int status,
                            fr_migrate_outcome outcome, const fr_error *run_err, fr_error *err) {
    cJSON *report = cJSON_CreateObject();
    if (report == NULL) return out_of_memory(err);
    cJSON_AddNumberToObject(report, "report", FR_MIGRATE_REPORT_VERSION);
    cJSON_AddStringToObject(report, "outcome", outcome_name(state, status, outcome));
    if (status != FR_OK) cJSON_AddStringToObject(report, "error", run_err->message);
    cJSON_AddItemToObject(report, "detected", detected_json(state));
    cJSON_AddItemToObject(report, "blockers", blockers_json(&state->blockers));
    cJSON_AddItemToObject(report, "notes", notes_json(state));
    cJSON_AddItemToObject(report, "proof", proof_json(state));
    char *text = cJSON_PrintUnformatted(report);
    cJSON_Delete(report);
    if (text == NULL) return out_of_memory(err);
    int written = fr_file_write_text(path, text, err);
    cJSON_free(text);
    if (written != FR_OK) return FR_ERR;
    return FR_OK;
}
