#ifndef DAUKLE_TASK_COMMAND_H
#define DAUKLE_TASK_COMMAND_H

#include "util/types.h"

/* Runs the program a manifest task declared, which is the one command daukle
   starts that no digest pins: D-53's section 2 takes that as the hatch's cost
   and keeps everything else, so the name is still bare, the arguments still
   reach the child as argv with no shell between, a batch file is still
   refused, and the tool report still names what ran.

   project_dir bounds command->cwd, and a command that has none runs in
   project_dir itself. A child that runs and fails is FR_ERR naming its exit
   code, because a task's own result is what its exit status says: unlike
   daukle.exec there is no caller here to hand a code to. */
int fr_task_command_run(const fr_task_command *command, const char *project_dir,
                        const char *task_name, fr_error *err);

#endif
