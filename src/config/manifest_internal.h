#ifndef DAUKLE_MANIFEST_INTERNAL_H
#define DAUKLE_MANIFEST_INTERNAL_H

#include "config/manifest.h"

#include "cJSON.h"

char *fr_manifest_duplicate(const char *text);
void fr_manifest_task_command_free(fr_task_command *command);
int fr_manifest_read_task(const cJSON *entry, const char *name, fr_task *out, fr_error *err);

#endif
