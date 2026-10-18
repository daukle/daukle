#include "migrate/proof.h"

#include "archive/archive.h"
#include "plugin/plugins.h"
#include "project/region.h"
#include "util/error.h"
#include "util/sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MEMBER_BYTES (64u * 1024u * 1024u)
#define MAX_MEMBERS 100000u
#define PATH_SIZE 1024

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

static int join_path(const char *root, const char *relative, char *out, fr_error *err) {
    int written = snprintf(out, PATH_SIZE, "%s/%s", root, relative);
    if (written < 0 || written >= PATH_SIZE) {
        fr_error_set(err, "comparing build outputs: the path \"%s\" below \"%s\" is too long",
                     relative, root);
        return FR_ERR;
    }
    return FR_OK;
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

static int add_member(proof_members *list, const fr_archive_member *member, const char *path,
                      fr_error *err) {
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
    if (!entry->is_symlink) fr_sha256_hex(member->bytes, member->length, entry->digest);
    list->count++;
    return FR_OK;
}

static int compare_members_by_name(const void *left, const void *right) {
    return strcmp(((const proof_member *) left)->name, ((const proof_member *) right)->name);
}

static int list_archive(const char *path, proof_members *list, fr_error *err) {
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
        if (add_member(list, member, path, err) != FR_OK) break;
    }
    fr_archive_close(archive);
    free_members(list);
    return FR_ERR;
}

static int members_equal(const proof_member *old_member, const proof_member *new_member,
                         char *difference, size_t difference_size) {
    if (old_member->is_symlink || new_member->is_symlink) {
        snprintf(difference, difference_size, "member \"%s\" is a symlink in the %s archive",
                 old_member->name, old_member->is_symlink ? "old" : "new");
        return 0;
    }
    if (strcmp(old_member->digest, new_member->digest) != 0) {
        snprintf(difference, difference_size,
                 "member \"%s\" has different bytes in the old and the new archive",
                 old_member->name);
        return 0;
    }
    return 1;
}

static int walk_sorted_members(const proof_members *old_list, const proof_members *new_list,
                               char *difference, size_t difference_size) {
    size_t old_index = 0;
    size_t new_index = 0;
    while (old_index < old_list->count || new_index < new_list->count) {
        int order = 0;
        if (old_index >= old_list->count) order = 1;
        else if (new_index >= new_list->count) order = -1;
        else order = strcmp(old_list->members[old_index].name, new_list->members[new_index].name);

        if (order < 0) {
            snprintf(difference, difference_size,
                     "member \"%s\" is in the old archive but not the new archive",
                     old_list->members[old_index].name);
            return 0;
        }
        if (order > 0) {
            snprintf(difference, difference_size,
                     "member \"%s\" is in the new archive but not the old archive",
                     new_list->members[new_index].name);
            return 0;
        }
        if (!members_equal(&old_list->members[old_index], &new_list->members[new_index],
                           difference, difference_size)) {
            return 0;
        }
        old_index++;
        new_index++;
    }
    return 1;
}

static int compare_archives(const char *old_path, const char *new_path, int *out_same,
                            char *difference, size_t difference_size, fr_error *err) {
    proof_members old_list = { 0 };
    proof_members new_list = { 0 };
    if (list_archive(old_path, &old_list, err) != FR_OK) return FR_ERR;
    if (list_archive(new_path, &new_list, err) != FR_OK) {
        free_members(&old_list);
        return FR_ERR;
    }
    *out_same = walk_sorted_members(&old_list, &new_list, difference, difference_size);
    free_members(&old_list);
    free_members(&new_list);
    return FR_OK;
}

static int compare_bytes(const char *old_path, const char *new_path, int *out_same,
                         char *difference, size_t difference_size, fr_error *err) {
    char *old_bytes = NULL;
    char *new_bytes = NULL;
    size_t old_length = 0;
    size_t new_length = 0;
    if (fr_file_read_bytes(old_path, &old_bytes, &old_length, err) != FR_OK) return FR_ERR;
    if (fr_file_read_bytes(new_path, &new_bytes, &new_length, err) != FR_OK) {
        free(old_bytes);
        return FR_ERR;
    }
    char old_digest[65];
    char new_digest[65];
    fr_sha256_hex(old_bytes, old_length, old_digest);
    fr_sha256_hex(new_bytes, new_length, new_digest);
    free(old_bytes);
    free(new_bytes);
    *out_same = fr_sha256_hex_equal(old_digest, new_digest);
    if (!*out_same) {
        snprintf(difference, difference_size,
                 "the old build's sha256 is %s and the new build's is %s", old_digest, new_digest);
    }
    return FR_OK;
}

int fr_proof_compare(const char *root, const char *old_path, const char *new_path,
                     fr_proof_kind kind, int *out_same, char *difference,
                     size_t difference_size, fr_error *err) {
    *out_same = 0;
    if (difference_size > 0) difference[0] = '\0';
    char old_full[PATH_SIZE];
    char new_full[PATH_SIZE];
    if (join_path(root, old_path, old_full, err) != FR_OK
        || join_path(root, new_path, new_full, err) != FR_OK) {
        return FR_ERR;
    }
    if (kind == FR_PROOF_ARCHIVE) {
        return compare_archives(old_full, new_full, out_same, difference, difference_size, err);
    }
    return compare_bytes(old_full, new_full, out_same, difference, difference_size, err);
}

int fr_proof_kind_parse(const char *text, fr_proof_kind *out) {
    if (strcmp(text, "bytes") == 0) {
        *out = FR_PROOF_BYTES;
        return FR_OK;
    }
    if (strcmp(text, "archive") == 0) {
        *out = FR_PROOF_ARCHIVE;
        return FR_OK;
    }
    return FR_ERR;
}
