#ifndef DAUKLE_MANIFEST_H
#define DAUKLE_MANIFEST_H

#include "util/types.h"

/* The only manifest schema this daukle reads, and the one "daukle init"
   writes. Shared so that init cannot scaffold a file the parser refuses. */
#define FR_SCHEMA 1

int fr_project_parse(const char *text, const char *origin, fr_project *out, fr_error *err);
/* Takes ownership of root on both paths: the manifest holds it on success and
   it is deleted here on failure, so no caller ever frees root itself. */
int fr_manifest_from_document(struct cJSON *root, const char *origin, fr_manifest *out, fr_error *err);
void fr_project_free(fr_project *project);
void fr_manifest_free(fr_manifest *manifest);
const fr_module *fr_project_module(const fr_project *project, const char *name);
const fr_source *fr_manifest_source(const fr_manifest *manifest, const char *project);

#endif
