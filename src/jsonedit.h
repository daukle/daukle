#ifndef TIESTONE_JSONEDIT_H
#define TIESTONE_JSONEDIT_H

#include "types.h"

#include <stddef.h>

/* Edits a json document by splicing spans of its text, so every byte outside
   the edited span survives: a generated file whose whole body reformats on
   first write buries the one line that actually drifted.

   path addresses the object to edit, "" for the document root and dotted for a
   nested one ("tiestone.managed"). A missing object along the path is created by
   a set and is left alone by a remove. Keys are compared raw, so a key written
   with a json escape does not match one written without it. */

int fr_json_edit_set_string(const char *text, const char *path, const char *key,
                            const char *value, char **out, fr_error *err);
int fr_json_edit_set_string_array(const char *text, const char *path, const char *key,
                                  const char *const *values, size_t count,
                                  char **out, fr_error *err);
int fr_json_edit_remove(const char *text, const char *path, const char *key,
                        char **out, fr_error *err);

#endif
