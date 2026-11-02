#include "cli/cli_internal.h"

#include "migrate/proof.h"
#include "util/error.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>

#define FR_CLI_COMPARE_DIFFERENT 3

static void print_text(const fr_proof_differences *list, fr_proof_kind kind) {
    for (size_t index = 0; index < list->count; index++) {
        const fr_proof_difference *difference = &list->items[index];
        if (kind == FR_PROOF_BYTES) {
            printf("bytes: the old sha256 is %s and the new one is %s\n", list->old_digest,
                   list->new_digest);
        } else {
            printf("%s: %s\n", fr_proof_difference_kind_name(difference->kind), difference->member);
        }
    }
    if (list->truncated) {
        printf("daukle: listing stopped after %u differences\n", FR_PROOF_MAX_DIFFERENCES);
    }
}

static int print_json(const fr_proof_differences *list, const char *as) {
    cJSON *root = cJSON_CreateObject();
    cJSON *differences = fr_proof_differences_json(list);
    if (root == NULL || differences == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(differences);
        return FR_ERR;
    }
    cJSON_AddBoolToObject(root, "same", list->count == 0);
    cJSON_AddStringToObject(root, "as", as);
    cJSON_AddItemToObject(root, "differences", differences);
    cJSON_AddBoolToObject(root, "truncated", list->truncated);
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL) return FR_ERR;
    printf("%s\n", text);
    cJSON_free(text);
    return FR_OK;
}

int fr_cli_compare(const fr_cli_options *options) {
    const char *as = options->compare_as != NULL ? options->compare_as : "bytes";
    fr_proof_kind kind;
    if (fr_proof_kind_parse(as, &kind) != FR_OK) {
        fprintf(stderr, "daukle: --as takes \"bytes\" or \"archive\", not \"%s\"\n", as);
        return 2;
    }
    fr_proof_differences list;
    fr_error err;
    if (fr_proof_list_differences(NULL, options->compare_old, options->compare_new, kind, &list,
                                  &err) != FR_OK) {
        fr_cli_report_error(&err, options->verbose);
        return 1;
    }
    int status = FR_OK;
    if (options->compare_json) status = print_json(&list, as);
    else print_text(&list, kind);
    int same = list.count == 0;
    fr_proof_differences_free(&list);
    if (status != FR_OK) {
        fprintf(stderr, "daukle: out of memory writing the comparison as JSON\n");
        return 1;
    }
    return same ? 0 : FR_CLI_COMPARE_DIFFERENT;
}
