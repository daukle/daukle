#include "migrate/proof_internal.h"

#include "archive/archive.h"
#include "plugin/plugins.h"
#include "util/error.h"
#include "util/sha256.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MEMBER_BYTES (64u * 1024u * 1024u)
#define MAX_MEMBERS 100000u

typedef struct {
    char *name;
    char digest[65];
    int is_symlink;
} proof_member;

typedef struct {
    proof_member *members;
    size_t count;
    size_t capacity;
} proof_members;

static void free_members(proof_members *list) {
    for (size_t index = 0; index < list->count; index++) free(list->members[index].name);
    free(list->members);
    list->members = NULL;
    list->count = 0;
    list->capacity = 0;
}

static int grow_members(proof_members *list, fr_error *err) {
    if (list->count < list->capacity) return FR_OK;
    size_t capacity = list->capacity == 0 ? 16 : list->capacity * 2;
    proof_member *grown = realloc(list->members, capacity * sizeof *grown);
    if (grown == NULL) {
        fr_error_set(err, "out of memory listing the members of an archive");
        return FR_ERR;
    }
    list->members = grown;
    list->capacity = capacity;
    return FR_OK;
}

static int ends_with_json(const char *name) {
    size_t length = strlen(name);
    return length >= 5 && strcmp(name + length - 5, ".json") == 0;
}

static int compare_keys(const void *left, const void *right) {
    const cJSON *first = *(const cJSON *const *) left;
    const cJSON *second = *(const cJSON *const *) right;
    return strcmp(first->string, second->string);
}

/* Relinks every object's members in key order, recursively, so two documents that hold the same
   value print the same. Returns 0 when out of memory. */
static int sort_object_keys(cJSON *item) {
    size_t count = 0;
    for (cJSON *child = item->child; child != NULL; child = child->next) {
        if (!sort_object_keys(child)) return 0;
        count++;
    }
    if (!cJSON_IsObject(item) || count < 2) return 1;
    cJSON **children = malloc(count * sizeof *children);
    if (children == NULL) return 0;
    size_t index = 0;
    for (cJSON *child = item->child; child != NULL; child = child->next) children[index++] = child;
    qsort(children, count, sizeof *children, compare_keys);
    for (index = 0; index < count; index++) {
        children[index]->prev = index == 0 ? children[count - 1] : children[index - 1];
        children[index]->next = index + 1 < count ? children[index + 1] : NULL;
    }
    item->child = children[0];
    free(children);
    return 1;
}

/* The digest of a JSON member's value, independent of whitespace and key order, or 0 when the
   member is not JSON, so it is compared by its bytes instead. */
static int json_value_digest(const fr_archive_member *member, char *digest) {
    cJSON *parsed = cJSON_ParseWithLength((const char *) member->bytes, member->length);
    if (parsed == NULL) return 0;
    char *canonical = sort_object_keys(parsed) ? cJSON_PrintUnformatted(parsed) : NULL;
    cJSON_Delete(parsed);
    if (canonical == NULL) return 0;
    fr_sha256_hex(canonical, strlen(canonical), digest);
    cJSON_free(canonical);
    return 1;
}

static int add_member(proof_members *list, const fr_archive_member *member, const char *path,
                      int json_values, fr_error *err) {
    if (list->count >= MAX_MEMBERS) {
        fr_error_set(err, "comparing \"%s\": it holds more than %u members", path, MAX_MEMBERS);
        return FR_ERR;
    }
    if (grow_members(list, err) != FR_OK) return FR_ERR;
    proof_member *entry = &list->members[list->count];
    entry->name = fr_dup_string(member->name);
    if (entry->name == NULL) {
        fr_error_set(err, "out of memory listing the members of \"%s\"", path);
        return FR_ERR;
    }
    entry->is_symlink = member->kind == FR_MEMBER_SYMLINK;
    entry->digest[0] = '\0';
    int by_value = !entry->is_symlink && json_values && ends_with_json(entry->name)
                   && json_value_digest(member, entry->digest);
    if (!entry->is_symlink && !by_value) fr_sha256_hex(member->bytes, member->length, entry->digest);
    list->count++;
    return FR_OK;
}

static int compare_members_by_name(const void *left, const void *right) {
    return strcmp(((const proof_member *) left)->name, ((const proof_member *) right)->name);
}

static int list_archive(const char *path, int json_values, proof_members *list, fr_error *err) {
    fr_archive *archive = NULL;
    if (fr_archive_open(path, MAX_MEMBER_BYTES, &archive, err) != FR_OK) return FR_ERR;
    const fr_archive_member *member = NULL;
    for (;;) {
        if (fr_archive_next(archive, &member, err) != FR_OK) break;
        if (member == NULL) {
            fr_archive_close(archive);
            qsort(list->members, list->count, sizeof *list->members, compare_members_by_name);
            return FR_OK;
        }
        if (member->kind == FR_MEMBER_DIRECTORY) continue;
        if (add_member(list, member, path, json_values, err) != FR_OK) break;
    }
    fr_archive_close(archive);
    free_members(list);
    return FR_ERR;
}

int fr_proof_differences_add(fr_proof_differences *list, const char *member,
                             fr_proof_difference_kind kind, fr_error *err) {
    if (list->count >= FR_PROOF_MAX_DIFFERENCES) {
        list->truncated = 1;
        return FR_OK;
    }
    if (list->count == list->capacity) {
        size_t capacity = list->capacity == 0 ? 16 : list->capacity * 2;
        fr_proof_difference *grown = realloc(list->items, capacity * sizeof *grown);
        if (grown == NULL) {
            fr_error_set(err, "out of memory listing the differences between two build outputs");
            return FR_ERR;
        }
        list->items = grown;
        list->capacity = capacity;
    }
    char *copy = NULL;
    if (member != NULL) {
        copy = fr_dup_string(member);
        if (copy == NULL) {
            fr_error_set(err, "out of memory listing the differences between two build outputs");
            return FR_ERR;
        }
    }
    list->items[list->count].member = copy;
    list->items[list->count].kind = kind;
    list->count++;
    return FR_OK;
}

void fr_proof_differences_free(fr_proof_differences *list) {
    for (size_t index = 0; index < list->count; index++) free(list->items[index].member);
    free(list->items);
    memset(list, 0, sizeof *list);
}

const char *fr_proof_difference_kind_name(fr_proof_difference_kind kind) {
    switch (kind) {
        case FR_PROOF_ONLY_OLD:
            return "only-old";
        case FR_PROOF_ONLY_NEW:
            return "only-new";
        case FR_PROOF_DIFFERENT_BYTES:
            return "bytes";
        case FR_PROOF_SYMLINK_OLD:
            return "symlink-old";
        case FR_PROOF_SYMLINK_NEW:
            return "symlink-new";
    }
    return "unknown";
}

void fr_proof_describe_archive_difference(const fr_proof_difference *difference, char *out,
                                          size_t size) {
    const char *member = difference->member;
    switch (difference->kind) {
        case FR_PROOF_ONLY_OLD:
            snprintf(out, size, "member \"%s\" is in the old archive but not the new archive", member);
            return;
        case FR_PROOF_ONLY_NEW:
            snprintf(out, size, "member \"%s\" is in the new archive but not the old archive", member);
            return;
        case FR_PROOF_DIFFERENT_BYTES:
            snprintf(out, size, "member \"%s\" has different bytes in the old and the new archive",
                     member);
            return;
        case FR_PROOF_SYMLINK_OLD:
        case FR_PROOF_SYMLINK_NEW:
            snprintf(out, size, "member \"%s\" is a symlink in the %s archive", member,
                     difference->kind == FR_PROOF_SYMLINK_OLD ? "old" : "new");
            return;
    }
}

static int add_member_difference(const proof_member *old_member, const proof_member *new_member,
                                 fr_proof_differences *out, fr_error *err) {
    if (old_member->is_symlink || new_member->is_symlink) {
        return fr_proof_differences_add(out, old_member->name,
                                        old_member->is_symlink ? FR_PROOF_SYMLINK_OLD
                                                               : FR_PROOF_SYMLINK_NEW,
                                        err);
    }
    if (strcmp(old_member->digest, new_member->digest) != 0) {
        return fr_proof_differences_add(out, old_member->name, FR_PROOF_DIFFERENT_BYTES, err);
    }
    return FR_OK;
}

static int walk_sorted_members(const proof_members *old_list, const proof_members *new_list,
                               fr_proof_differences *out, fr_error *err) {
    size_t old_index = 0;
    size_t new_index = 0;
    int status = FR_OK;
    while (status == FR_OK && !out->truncated
           && (old_index < old_list->count || new_index < new_list->count)) {
        int order = 0;
        if (old_index >= old_list->count) order = 1;
        else if (new_index >= new_list->count) order = -1;
        else order = strcmp(old_list->members[old_index].name, new_list->members[new_index].name);

        if (order < 0) {
            status = fr_proof_differences_add(out, old_list->members[old_index++].name,
                                              FR_PROOF_ONLY_OLD, err);
        } else if (order > 0) {
            status = fr_proof_differences_add(out, new_list->members[new_index++].name,
                                              FR_PROOF_ONLY_NEW, err);
        } else {
            status = add_member_difference(&old_list->members[old_index++],
                                           &new_list->members[new_index++], out, err);
        }
    }
    return status;
}

int fr_proof_list_archive_differences(const char *old_path, const char *new_path, int json_values,
                                      fr_proof_differences *out, fr_error *err) {
    proof_members old_list = { 0 };
    proof_members new_list = { 0 };
    if (list_archive(old_path, json_values, &old_list, err) != FR_OK) return FR_ERR;
    if (list_archive(new_path, json_values, &new_list, err) != FR_OK) {
        free_members(&old_list);
        return FR_ERR;
    }
    int status = walk_sorted_members(&old_list, &new_list, out, err);
    free_members(&old_list);
    free_members(&new_list);
    if (status != FR_OK) fr_proof_differences_free(out);
    return status;
}

int fr_proof_compare_archives(const char *old_path, const char *new_path, int json_values, int *out_same,
                              char *difference, size_t difference_size, fr_error *err) {
    fr_proof_differences list = { 0 };
    if (fr_proof_list_archive_differences(old_path, new_path, json_values, &list, err) != FR_OK) return FR_ERR;
    *out_same = list.count == 0;
    if (list.count > 0 && difference_size > 0) {
        fr_proof_describe_archive_difference(&list.items[0], difference, difference_size);
    }
    fr_proof_differences_free(&list);
    return FR_OK;
}

static cJSON *difference_json(const fr_proof_difference *difference,
                              const fr_proof_differences *list) {
    cJSON *entry = cJSON_CreateObject();
    if (entry == NULL) return NULL;
    if (difference->member != NULL) cJSON_AddStringToObject(entry, "member", difference->member);
    cJSON_AddStringToObject(entry, "kind", fr_proof_difference_kind_name(difference->kind));
    if (difference->member == NULL) {
        cJSON_AddStringToObject(entry, "oldSha256", list->old_digest);
        cJSON_AddStringToObject(entry, "newSha256", list->new_digest);
    }
    return entry;
}

cJSON *fr_proof_differences_json(const fr_proof_differences *list) {
    cJSON *array = cJSON_CreateArray();
    if (array == NULL) return NULL;
    for (size_t index = 0; index < list->count; index++) {
        cJSON *entry = difference_json(&list->items[index], list);
        if (entry == NULL) {
            cJSON_Delete(array);
            return NULL;
        }
        cJSON_AddItemToArray(array, entry);
    }
    return array;
}
