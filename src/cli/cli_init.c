#include "cli/cli_internal.h"

#include "cli/cli.h"
#include "cli/wrapper_pin.h"
#include "cli/wrapper_scripts.h"
#include "project/region.h"
#include "util/error.h"
#include "util/executable.h"
#include "util/version.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#define FR_INIT_MANIFEST "daukle.toml"
#define FR_INIT_POSIX_SCRIPT "daukle"
#define FR_INIT_POWERSHELL_SCRIPT "daukle.ps1"
#define FR_INIT_PIN_DIRECTORY ".daukle"
#define FR_INIT_PIN ".daukle/wrapper.toml"

enum { FR_INIT_WROTE, FR_INIT_PRESENT, FR_INIT_READY, FR_INIT_FAILED, FR_INIT_NO_PIN };

typedef struct {
    char *bytes;
    size_t length;
} init_pin;

static int file_exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) return 0;
    fclose(file);
    return 1;
}

static int write_bytes(const char *path, const void *bytes, size_t length) {
    FILE *file = fopen(path, "wb");
    if (file == NULL) return 0;
    size_t written = fwrite(bytes, 1, length, file);
    int closed = fclose(file) == 0;
    return written == length && closed;
}

static int make_directory(const char *path) {
#ifdef _WIN32
    return _mkdir(path) == 0 || errno == EEXIST;
#else
    return mkdir(path, 0777) == 0 || errno == EEXIST;
#endif
}

static int mark_executable(const char *path) {
#ifdef _WIN32
    (void) path;
    return 1;
#else
    return chmod(path, 0755) == 0;
#endif
}

static const char *project_name(const char *name) {
    static char cwd[1024];
#ifdef _WIN32
    const char *working = _getcwd(cwd, (int) sizeof cwd);
#else
    const char *working = getcwd(cwd, sizeof cwd);
#endif
    const char *project = name != NULL ? name : fr_cli_last_path_segment(working);
    return project == NULL || project[0] == '\0' ? "my-project" : project;
}

/* Scaffolding a person runs once, not generation daukle owns, which is why it may write at the
   project root at all: the no-files-at-the-root rule forbids daukle WRITING generated files there,
   and this writes the authored file the rule exists to protect. It declares no plugins, because
   what a project builds with is a decision and an empty [modules] is already a working manifest.
   D-70. */
static int write_manifest(const char *name, int verbose) {
    if (file_exists(FR_INIT_MANIFEST)) {
        fprintf(stderr, "daukle: %s already exists, and init will not overwrite it\n",
                FR_INIT_MANIFEST);
        return FR_INIT_PRESENT;
    }

    const char *project = project_name(name);
    char text[1024];
    if (!fr_cli_init_manifest(project, text, sizeof text)) {
        fprintf(stderr, "daukle: the project name is too long to write into %s\n", FR_INIT_MANIFEST);
        return FR_INIT_FAILED;
    }

    fr_error err;
    if (fr_file_write_text(FR_INIT_MANIFEST, text, &err) != FR_OK) {
        fr_cli_report_error(&err, verbose);
        return FR_INIT_FAILED;
    }
    printf("daukle: wrote %s for \"%s\".\n", FR_INIT_MANIFEST, project);
    printf("daukle: add a plugin under [plugins], then run \"daukle sync\".\n");
    return FR_INIT_WROTE;
}

static size_t count_wrapper_files_present(void) {
    return (size_t) file_exists(FR_INIT_POSIX_SCRIPT) + (size_t) file_exists(FR_INIT_POWERSHELL_SCRIPT) +
           (size_t) file_exists(FR_INIT_PIN);
}

static int report_no_pin(const char *searched) {
    fprintf(stderr,
            "daukle: no wrapper.toml ships with this daukle, so the wrapper was not written. A "
            "release carries one beside the binary; a daukle built from source has none. Looked "
            "for it at:\n");
    for (const char *line = searched; line != NULL && *line != '\0';) {
        const char *end = strchr(line, '\n');
        size_t length = end != NULL ? (size_t) (end - line) : strlen(line);
        fprintf(stderr, "  %.*s\n", (int) length, line);
        line = end != NULL ? end + 1 : NULL;
    }
    fprintf(stderr, "daukle: set DAUKLE_WRAPPER_PIN to a release's wrapper.toml to use that one\n");
    return FR_INIT_NO_PIN;
}

static int pin_describes_this_daukle(const char *found, const init_pin *pin) {
    char version[64];
    if (!fr_wrapper_pin_version(pin->bytes, version, sizeof version)) {
        fprintf(stderr, "daukle: %s names no version, so it is not a pin and the wrapper was not "
                        "written\n", found);
        return 0;
    }
    if (strcmp(version, fr_self_version()) != 0) {
        fprintf(stderr, "daukle: %s pins daukle \"%s\" but this is daukle %s, so the wrapper was "
                        "not written: whatever installed this daukle shipped the wrong pin\n",
                found, version, fr_self_version());
        return 0;
    }
    return 1;
}

static int load_pin(init_pin *pin, int verbose) {
    char *directory = fr_executable_directory();
    char *found = NULL;
    char *searched = NULL;
    int located = fr_wrapper_pin_find(getenv("DAUKLE_WRAPPER_PIN"), directory, &found, &searched);
    free(directory);
    if (!located) {
        int status = report_no_pin(searched);
        free(searched);
        return status;
    }

    fr_error err;
    int status = FR_INIT_READY;
    if (fr_file_read_bytes(found, &pin->bytes, &pin->length, &err) != FR_OK) {
        fr_cli_report_error(&err, verbose);
        status = FR_INIT_FAILED;
    } else if (!pin_describes_this_daukle(found, pin)) {
        status = FR_INIT_NO_PIN;
    }
    free(found);
    return status;
}

static void remove_partial_wrapper(void) {
    remove(FR_INIT_POSIX_SCRIPT);
    remove(FR_INIT_POWERSHELL_SCRIPT);
    remove(FR_INIT_PIN);
}

static int write_wrapper_files(const init_pin *pin) {
    int written = write_bytes(FR_INIT_POSIX_SCRIPT, fr_wrapper_posix_script,
                              fr_wrapper_posix_script_length) &&
                  mark_executable(FR_INIT_POSIX_SCRIPT) &&
                  write_bytes(FR_INIT_POWERSHELL_SCRIPT, fr_wrapper_powershell_script,
                              fr_wrapper_powershell_script_length) &&
                  make_directory(FR_INIT_PIN_DIRECTORY) &&
                  write_bytes(FR_INIT_PIN, pin->bytes, pin->length);
    if (!written) {
        remove_partial_wrapper();
        fprintf(stderr, "daukle: could not write the wrapper here, and removed what was written\n");
        return FR_INIT_FAILED;
    }
    printf("daukle: wrote %s\n", FR_INIT_POSIX_SCRIPT);
    printf("daukle: wrote %s\n", FR_INIT_POWERSHELL_SCRIPT);
    printf("daukle: wrote %s\n", FR_INIT_PIN);
    return FR_INIT_WROTE;
}

/* The three files are one unit: a partial wrapper is a project mid-edit, and filling in its gaps
   would pair scripts and a pin from two different releases. */
static int write_wrapper(int verbose) {
    size_t present = count_wrapper_files_present();
    if (present == 3) {
        fprintf(stderr, "daukle: the wrapper is already here; \"./daukle wrapper update\" "
                        "refreshes it\n");
        return FR_INIT_PRESENT;
    }
    if (present > 0) {
        fprintf(stderr, "daukle: part of the wrapper is already here (%s, %s and %s are one unit), "
                        "so init writes none of it; \"./daukle wrapper update\" replaces all three\n",
                FR_INIT_POSIX_SCRIPT, FR_INIT_POWERSHELL_SCRIPT, FR_INIT_PIN);
        return FR_INIT_PRESENT;
    }

    init_pin pin = { NULL, 0 };
    int status = load_pin(&pin, verbose);
    if (status == FR_INIT_READY) status = write_wrapper_files(&pin);
    free(pin.bytes);
    return status;
}

int fr_cli_init(const char *name, int verbose) {
    int manifest = write_manifest(name, verbose);
    if (manifest == FR_INIT_FAILED) return 1;

    int wrapper = write_wrapper(verbose);
    if (wrapper == FR_INIT_FAILED) return 1;
    if (wrapper == FR_INIT_NO_PIN) return 3;
    if (manifest == FR_INIT_PRESENT && wrapper == FR_INIT_PRESENT) return 2;

    printf("daukle: review and commit them.\n");
    return 0;
}
