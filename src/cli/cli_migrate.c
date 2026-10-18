#include "cli/cli_internal.h"

#include "migrate/catalog.h"
#include "migrate/migrate.h"
#include "util/error.h"

#include <stdio.h>
#include <stdlib.h>

#define FR_CLI_MIGRATE_BLOCKED 3
#define FR_CLI_MIGRATE_UNPROVED 4

static int finish_migration(int verbose) {
    fr_error err;
    if (fr_migrate_done(&err) != FR_OK) {
        fr_cli_report_error(&err, verbose);
        return 1;
    }
    printf("daukle: deleted build/daukle/migrate/old; the migration is finished\n");
    return 0;
}

static int exit_status_of(fr_migrate_outcome outcome) {
    switch (outcome) {
        case FR_MIGRATE_DONE:
            return 0;
        case FR_MIGRATE_BLOCKED:
            return FR_CLI_MIGRATE_BLOCKED;
        case FR_MIGRATE_UNPROVED:
            return FR_CLI_MIGRATE_UNPROVED;
    }
    return 1;
}

int fr_cli_migrate(const fr_cli_options *options) {
    if (options->migrate_done) {
        if (options->catalog_count > 0 || options->with_count > 0) {
            fprintf(stderr, "daukle: \"migrate --done\" only deletes what a migration kept, so it"
                            " takes no --catalog or --with\n");
            return 2;
        }
        return finish_migration(options->verbose);
    }

    fr_error err;
    const char **locations = NULL;
    size_t location_count = 0;
    if (fr_catalog_locations(getenv("DAUKLE_CATALOGS"), options->catalogs, options->catalog_count,
                             &locations, &location_count, &err) != FR_OK) {
        fr_cli_report_error(&err, options->verbose);
        return 1;
    }

    fr_migrate_options migrate_options = { locations, location_count, options->with,
                                           options->with_count, options->use_cache };
    fr_migrate_outcome outcome = FR_MIGRATE_BLOCKED;
    int status = fr_migrate_run(&migrate_options, &outcome, &err);
    free((void *) locations);
    if (status != FR_OK) {
        fr_cli_report_error(&err, options->verbose);
        return 1;
    }
    return exit_status_of(outcome);
}
