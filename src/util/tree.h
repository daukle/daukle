#ifndef DAUKLE_TREE_H
#define DAUKLE_TREE_H

/* Removes path and everything beneath it, best effort: a child that cannot be
   removed leaves its parent behind rather than aborting the sweep, because both
   callers re-probe the result afterwards and have different things to say about
   what survived.

   @implNote Containment is deliberately NOT checked here. A provisioned root
   and a derived directory do not share a rule, so each caller keeps its own
   check; this function only walks and unlinks. */
void fr_remove_tree(const char *path);

#endif
