#ifndef DAUKLE_TREE_H
#define DAUKLE_TREE_H

#include "util/types.h"

/* Removes path and everything beneath it, best effort: a child that cannot be
   removed leaves its parent behind rather than aborting the sweep, because both
   callers re-probe the result afterwards and have different things to say about
   what survived.

   @implNote Containment is deliberately NOT checked here. A provisioned root
   and a derived directory do not share a rule, so each caller keeps its own
   check; this function only walks and unlinks. */
void fr_remove_tree(const char *path);

typedef void (*fr_directory_visitor)(const char *name, int is_directory, int is_link,
                                     void *context);

/* Calls visit once per entry of directory, skipping "." and "..", in no
   particular order; a directory that cannot be opened visits nothing. name is
   the bare entry name and is valid only during the call. */
void fr_list_directory(const char *directory, fr_directory_visitor visit, void *context);

/* fr_list_directory that says why a directory could not be opened rather than
   visiting nothing, so a caller can tell an unreadable directory from an empty one. */
int fr_list_directory_checked(const char *directory, fr_directory_visitor visit, void *context,
                              fr_error *err);

#endif
