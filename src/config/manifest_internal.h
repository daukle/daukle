#ifndef DAUKLE_MANIFEST_INTERNAL_H
#define DAUKLE_MANIFEST_INTERNAL_H

#include "config/manifest.h"

#include "cJSON.h"

char *fr_manifest_duplicate(const char *text);
void fr_manifest_task_command_free(fr_task_command *command);
int fr_manifest_read_task(const cJSON *entry, const char *name, fr_task *out, fr_error *err);

int fr_manifest_read_source(const cJSON *entry, const char *project_name, fr_source *out, fr_error *err);
int fr_manifest_read_consumer(const cJSON *entry, size_t consumer_index, fr_consumer *out, fr_error *err);
int fr_manifest_read_toolchain(const cJSON *entry, const char *name, fr_toolchain *out, fr_error *err);
int fr_manifest_read_publish(const cJSON *entry, const char *name, fr_publish_target *out,
                             fr_error *err);
int fr_manifest_check_publish_targets(const fr_manifest *out, const char *file_path, fr_error *err);

#endif
