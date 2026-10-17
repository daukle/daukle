#ifndef DAUKLE_TASKS_INTERNAL_H
#define DAUKLE_TASKS_INTERNAL_H

#include "project/tasks.h"

int fr_tasks_text_is_prefix(const char *declared, const char *name, size_t prefix);
int fr_tasks_check_name(const char *name, fr_error *err);
int fr_tasks_append(fr_task_set *set, const fr_task_node *node, fr_error *err);

/* Appends the node of plugin task name for every manifest toolchain whose
   plugin is the name's prefix: borrowed from the registry for the toolchain
   named like the prefix, renamed and owned by set for every other instance. */
int fr_tasks_append_toolchain_nodes(fr_task_set *set, const fr_manifest *manifest,
                                    const fr_task_plugin *plugin, const char *name, size_t prefix,
                                    fr_error *err);

#endif
