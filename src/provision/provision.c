#include "provision/provision.h"

#include "archive/archive.h"
#include "cache/cache.h"
#include "net/http.h"
#include "util/error.h"
#include "util/sha256.h"
#include "util/tree.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define PROVISION_DIGEST_LENGTH 64

static int make_directory(const char *directory) {
#ifdef _WIN32
    return CreateDirectoryA(directory, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
#else
    return mkdir(directory, 0777) == 0 || errno == EEXIST;
#endif
}

static int is_hex_digit(char character) {
    return (character >= '0' && character <= '9')
        || (character >= 'a' && character <= 'f')
        || (character >= 'A' && character <= 'F');
}

static int digest_is_well_formed(const char *sha256_hex) {
    if (sha256_hex == NULL) return 0;
    size_t index = 0;
    for (; sha256_hex[index] != '\0'; index++) {
        if (index >= PROVISION_DIGEST_LENGTH) return 0;
        if (!is_hex_digit(sha256_hex[index])) return 0;
    }
    return index == PROVISION_DIGEST_LENGTH;
}

/* A pin may be written in either case and names the same bytes either way, so
   the directory is spelled one way only: two spellings would unpack the same
   toolchain twice on a case-sensitive filesystem. */
static void to_lowercase_digest(const char *sha256_hex, char out[PROVISION_DIGEST_LENGTH + 1]) {
    for (size_t index = 0; index < PROVISION_DIGEST_LENGTH; index++) {
        char character = sha256_hex[index];
        out[index] = (character >= 'A' && character <= 'F')
                   ? (char) (character - 'A' + 'a')
                   : character;
    }
    out[PROVISION_DIGEST_LENGTH] = '\0';
}

int fr_provision_root_path(const char *sha256_hex, char *out, size_t out_size, fr_error *err) {
    if (!digest_is_well_formed(sha256_hex)) {
        fr_error_set(err, "\"%s\" is not a sha256 pin: 64 hexadecimal characters are required",
                     sha256_hex == NULL ? "" : sha256_hex);
        return FR_ERR;
    }

    char toolchains[1024];
    if (fr_cache_toolchains_root(toolchains, sizeof toolchains, err) != FR_OK) return FR_ERR;

    char digest[PROVISION_DIGEST_LENGTH + 1];
    to_lowercase_digest(sha256_hex, digest);

    int written = snprintf(out, out_size, "%s/%s", toolchains, digest);
    if (written < 0 || (size_t) written >= out_size) {
        fr_error_set(err, "the provisioned root for %s is too long", digest);
        return FR_ERR;
    }
    return FR_OK;
}

/* Distinguishes two temporary trees within one process, since a pid alone
   repeats across the several provisions a single run makes. */
static unsigned next_attempt(void) {
    static unsigned attempts = 0;
    return attempts++;
}

static int current_process_id(void) {
#ifdef _WIN32
    return _getpid();
#else
    return (int) getpid();
#endif
}

static int is_safe_host_character(char character) {
    return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'z')
        || (character >= 'A' && character <= 'Z') || character == '-' || character == '_';
}

/* @implNote A pid alone names a temporary tree uniquely on ONE machine, and the
   cache is routinely a home directory shared over NFS or SMB. Two machines that
   collide there delete each other's in-flight tree while make_parent_directories
   recreates as it goes, and the loser can then rename an INCOMPLETE tree under
   the content key, which breaks "a present directory is a complete one"
   permanently and with no digest re-check to catch it. */
static void host_component(char *out, size_t size) {
#ifdef _WIN32
    DWORD length = (DWORD) size;
    if (!GetComputerNameA(out, &length)) out[0] = '\0';
#else
    if (gethostname(out, size) != 0) out[0] = '\0';
    out[size - 1] = '\0';
#endif
    for (char *cursor = out; *cursor != '\0'; cursor++) {
        if (!is_safe_host_character(*cursor)) *cursor = '-';
    }
    if (out[0] == '\0') snprintf(out, size, "host");
}

static int fetch_and_verify(const char *url, const char *sha256_hex,
                            const fr_http_header *headers, size_t header_count,
                            const char *archive_path,
                            fr_error *err) {
    fr_sha256 digest;
    size_t length = 0;
    if (fr_http_get_to_file(url, headers, header_count, archive_path, FR_HTTP_MAX_FILE,
                            &digest, &length, err)
        != FR_OK) {
        return FR_ERR;
    }

    char actual[65];
    fr_sha256_final(&digest, actual);
    if (!fr_sha256_hex_equal(actual, sha256_hex)) {
        fr_error_set(err, "%s does not match its pin: expected %s, served %s", url, sha256_hex,
                     actual);
        return FR_ERR;
    }
    return FR_OK;
}

static int unpack_into(const char *archive_path, const char *tree_path, fr_unpack_report *report,
                       fr_error *err) {
    if (!make_directory(tree_path)) {
        fr_error_set(err, "cannot create a directory to unpack into");
        return FR_ERR;
    }

    fr_archive *archive = NULL;
    if (fr_archive_open(archive_path, FR_UNPACK_DEFAULTS.max_member_bytes, &archive, err)
        != FR_OK) {
        return FR_ERR;
    }

    int result = fr_unpack(archive, tree_path, &FR_UNPACK_DEFAULTS, report, err);
    fr_archive_close(archive);
    return result;
}

/* The name a url's last segment suggests, which is cosmetic: the digest
   directory above it is the key, so a segment that cannot be used costs
   legibility in a classpath line and a diagnostic and nothing else. */
static void artifact_file_name(const char *url, char *out, size_t size) {
    const char *last_slash = strrchr(url, '/');
    const char *name = last_slash == NULL ? url : last_slash + 1;
    size_t length = strcspn(name, "?#");
    if (length == 0 || length + 1 > size) {
        snprintf(out, size, "artifact");
        return;
    }
    memcpy(out, name, length);
    out[length] = '\0';
    if (!fr_cache_component_is_safe(out, 0)) snprintf(out, size, "artifact");
}

int fr_artifact(const char *url, const char *sha256_hex, const fr_http_header *headers,
                size_t header_count, fr_artifact_result *out, fr_error *err) {
    memset(out, 0, sizeof *out);

    if (url == NULL || url[0] == '\0') {
        fr_error_set(err, "a pinned artifact names no file to fetch");
        return FR_ERR;
    }
    if (!digest_is_well_formed(sha256_hex)) {
        fr_error_set(err, "\"%s\" is not a sha256 pin: 64 hexadecimal characters are required",
                     sha256_hex == NULL ? "" : sha256_hex);
        return FR_ERR;
    }

    char artifacts[1024];
    if (fr_cache_artifacts_root(artifacts, sizeof artifacts, err) != FR_OK) return FR_ERR;

    char digest[PROVISION_DIGEST_LENGTH + 1];
    to_lowercase_digest(sha256_hex, digest);

    char name[256];
    artifact_file_name(url, name, sizeof name);

    char directory[1024];
    int written = snprintf(directory, sizeof directory, "%s/%s", artifacts, digest);
    if (written < 0 || (size_t) written >= sizeof directory) {
        fr_error_set(err, "the cache directory for %s is too long", digest);
        return FR_ERR;
    }
    written = snprintf(out->path, sizeof out->path, "%s/%s", directory, name);
    if (written < 0 || (size_t) written >= sizeof out->path) {
        out->path[0] = '\0';
        fr_error_set(err, "the cache path for %s is too long", digest);
        return FR_ERR;
    }

    if (fr_cache_file_exists(out->path)) {
        out->was_cached = 1;
        return FR_OK;
    }

    fr_cache_make_directories(artifacts);

    char host[64];
    host_component(host, sizeof host);

    char temporary[1024];
    written = snprintf(temporary, sizeof temporary, "%s/.partial-%s-%d-%u", artifacts, host,
                       current_process_id(), next_attempt());
    if (written < 0 || (size_t) written >= sizeof temporary) {
        fr_error_set(err, "the temporary artifact path is too long");
        out->path[0] = '\0';
        return FR_ERR;
    }
    remove(temporary);

    if (fetch_and_verify(url, sha256_hex, headers, header_count, temporary, err) != FR_OK) {
        remove(temporary);
        out->path[0] = '\0';
        return FR_ERR;
    }

    fr_cache_make_directories(directory);
    if (fr_cache_rename_file(temporary, out->path) != FR_OK) {
        remove(temporary);
        fr_error_set(err, "cannot move the fetched artifact for %s into the cache", digest);
        out->path[0] = '\0';
        return FR_ERR;
    }
    return FR_OK;
}

int fr_artifact_pin(const char *url, const fr_http_header *headers, size_t header_count,
                    fr_pin_result *out, fr_error *err) {
    memset(out, 0, sizeof *out);

    if (url == NULL || url[0] == '\0') {
        fr_error_set(err, "an unpinned fetch names no file to fetch");
        return FR_ERR;
    }

    char artifacts[1024];
    if (fr_cache_artifacts_root(artifacts, sizeof artifacts, err) != FR_OK) return FR_ERR;
    fr_cache_make_directories(artifacts);

    char host[64];
    host_component(host, sizeof host);

    char temporary[1024];
    int written = snprintf(temporary, sizeof temporary, "%s/.partial-%s-%d-%u", artifacts, host,
                           current_process_id(), next_attempt());
    if (written < 0 || (size_t) written >= sizeof temporary) {
        fr_error_set(err, "the temporary artifact path is too long");
        return FR_ERR;
    }
    remove(temporary);

    fr_sha256 digest;
    size_t length = 0;
    /* No remove on this path: fr_http_get_to_file removes the partial file
       itself, measured by mutating a remove here and watching nothing redden. */
    if (fr_http_get_to_file(url, headers, header_count, temporary, FR_HTTP_MAX_FILE, &digest,
                            &length, err) != FR_OK) {
        return FR_ERR;
    }
    fr_sha256_final(&digest, out->sha256);

    char name[256];
    artifact_file_name(url, name, sizeof name);

    char directory[1024];
    written = snprintf(directory, sizeof directory, "%s/%s", artifacts, out->sha256);
    if (written < 0 || (size_t) written >= sizeof directory) {
        remove(temporary);
        out->sha256[0] = '\0';
        fr_error_set(err, "the cache directory for %s is too long", url);
        return FR_ERR;
    }
    written = snprintf(out->path, sizeof out->path, "%s/%s", directory, name);
    if (written < 0 || (size_t) written >= sizeof out->path) {
        remove(temporary);
        out->path[0] = '\0';
        out->sha256[0] = '\0';
        fr_error_set(err, "the cache path for %s is too long", url);
        return FR_ERR;
    }

    /* Two urls serving one digest land on one file, so a second resolve of the
       same bytes keeps what is already there rather than renaming over it. */
    if (fr_cache_file_exists(out->path)) {
        remove(temporary);
        return FR_OK;
    }

    fr_cache_make_directories(directory);
    if (fr_cache_rename_file(temporary, out->path) != FR_OK) {
        remove(temporary);
        fr_error_set(err, "cannot move the fetched file for %s into the cache", url);
        out->path[0] = '\0';
        out->sha256[0] = '\0';
        return FR_ERR;
    }
    return FR_OK;
}

int fr_provision(const char *url, const char *sha256_hex, const fr_http_header *headers,
                 size_t header_count, fr_provision_result *out,
                 fr_error *err) {
    memset(out, 0, sizeof *out);

    if (url == NULL || url[0] == '\0') {
        fr_error_set(err, "a provisioned toolchain names no archive to fetch");
        return FR_ERR;
    }
    if (fr_provision_root_path(sha256_hex, out->root, sizeof out->root, err) != FR_OK) {
        out->root[0] = '\0';
        return FR_ERR;
    }

    if (fr_cache_directory_exists(out->root)) {
        out->was_cached = 1;
        return FR_OK;
    }

    char toolchains[1024];
    if (fr_cache_toolchains_root(toolchains, sizeof toolchains, err) != FR_OK) {
        out->root[0] = '\0';
        return FR_ERR;
    }
    fr_cache_make_directories(toolchains);

    char host[64];
    host_component(host, sizeof host);

    char temporary[1024];
    int written = snprintf(temporary, sizeof temporary, "%s/.partial-%s-%d-%u", toolchains, host,
                           current_process_id(), next_attempt());
    if (written < 0 || (size_t) written >= sizeof temporary) {
        fr_error_set(err, "the temporary provisioning directory is too long");
        out->root[0] = '\0';
        return FR_ERR;
    }
    fr_remove_tree(temporary);
    if (!make_directory(temporary)) {
        fr_error_set(err, "cannot create a temporary directory under %s", toolchains);
        out->root[0] = '\0';
        return FR_ERR;
    }

    /* Sized from the directory they extend, so neither call can truncate. */
    char archive_path[sizeof temporary + sizeof "/archive"];
    char tree_path[sizeof temporary + sizeof "/tree"];
    snprintf(archive_path, sizeof archive_path, "%s/archive", temporary);
    snprintf(tree_path, sizeof tree_path, "%s/tree", temporary);

    fr_unpack_report report;
    memset(&report, 0, sizeof report);

    int result = fetch_and_verify(url, sha256_hex, headers, header_count, archive_path, err);
    if (result == FR_OK) result = unpack_into(archive_path, tree_path, &report, err);
    if (result == FR_OK && fr_cache_rename_directory(tree_path, out->root) != FR_OK) {
        fr_error_set(err, "cannot move the unpacked tree for %s into the cache", sha256_hex);
        result = FR_ERR;
    }

    fr_remove_tree(temporary);
    if (result != FR_OK) {
        out->root[0] = '\0';
        return FR_ERR;
    }

    out->unpack = report;
    return FR_OK;
}
