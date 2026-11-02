#ifndef DAUKLE_MIGRATE_REPORT_H
#define DAUKLE_MIGRATE_REPORT_H

#include "migrate/migrate_internal.h"
#include "util/types.h"

struct cJSON;

/* Appends one proof comparison to the report's list, with every difference.
   A side that cannot be read is recorded with its error, never raised: the
   report describes the run and the run already decided. */
int fr_migrate_report_comparison(fr_migrate_state *state, const char *instance,
                                 const struct cJSON *entry, fr_error *err);

/* Writes the whole run to path as JSON. status and run_err are what the run
   returned, so an error outcome carries its message. */
int fr_migrate_report_write(const char *path, const fr_migrate_state *state, int status,
                            fr_migrate_outcome outcome, const fr_error *run_err, fr_error *err);

#endif
