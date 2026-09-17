#include "cli.h"
#include "sync.h"

#include <stdio.h>

#define TERKO_VERSION "0.1.0"

static int run(const char *manifest_path, int write, int use_cache) {
    fr_error err;
    fr_sync_report report;
    int status = 0;
    if (fr_sync(manifest_path, write, use_cache, &report, &err) != FR_OK) {
        fprintf(stderr, "terko: %s\n", err.message);
        if (write && report.count > 0) {
            fprintf(stderr, "terko: %zu file%s updated before the failure\n",
                    report.count, report.count == 1 ? "" : "s");
        }
        status = 1;
    } else if (write) {
        printf("terko: %s\n", report.count > 0 ? "updated" : "already in sync");
    } else if (report.count > 0) {
        for (size_t index = 0; index < report.count; index++) {
            fprintf(stderr, "terko: %s is out of date\n", report.files[index]);
        }
        fprintf(stderr, "terko: run \"terko sync\"\n");
        status = 1;
    } else {
        printf("terko: in sync\n");
    }
    fr_sync_report_free(&report);
    return status;
}

int main(int argc, char **argv) {
    fr_cli_options options;
    fr_cli_parse(argc, argv, &options);

    switch (options.command) {
        case FR_CLI_VERSION:
            printf("terko %s\n", TERKO_VERSION);
            return 0;
        case FR_CLI_SYNC:
            return run(options.manifest_path, 1, options.use_cache);
        case FR_CLI_CHECK:
            return run(options.manifest_path, 0, options.use_cache);
        case FR_CLI_USAGE:
            break;
    }

    fprintf(stderr, "usage: terko [--version | sync [manifest] | check [manifest]] [--no-cache]\n");
    return 2;
}
