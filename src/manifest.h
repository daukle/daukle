#ifndef DAUKLE_MANIFEST_H
#define DAUKLE_MANIFEST_H

#include "types.h"

int fr_project_read(const char *file_path, fr_project *out, fr_error *err);
int fr_project_parse(const char *text, const char *origin, fr_project *out, fr_error *err);
int fr_manifest_from_document(struct cJSON *root, const char *origin, fr_manifest *out, fr_error *err);
int fr_manifest_read(const char *file_path, fr_manifest *out, fr_error *err);
void fr_project_free(fr_project *project);
void fr_manifest_free(fr_manifest *manifest);
const fr_module *fr_project_module(const fr_project *project, const char *name);
const fr_source *fr_manifest_source(const fr_manifest *manifest, const char *project);

#endif
