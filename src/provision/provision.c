#include "provision/provision.h"

#include "archive/archive.h"
#include "cache/cache.h"
#include "util/error.h"
#include "net/http.h"
#include "util/sha256.h"

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

typedef void (*child_visitor)(const char *child_path, int is_directory, int is_link);

static void for_each_child(const char *directory, child_visitor visit) {
#ifdef _WIN32
    char pattern[1024];
    int written = snprintf(pattern, sizeof pattern, "%s/*", directory);
    if (written < 0 || (size_t) written >= sizeof pattern) return;

    WIN32_FIND_DATAA found;
    HANDLE handle = FindFirstFileA(pattern, &found);
    if (handle == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(found.cFileName, ".") == 0 || strcmp(found.cFileName, "..") == 0) continue;
        char child[1024];
        int child_written = snprintf(child, sizeof child, "%s/%s", directory, found.cFileName);
        if (child_written < 0 || (size_t) child_written >= sizeof child) continue;
        visit(child, (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
              (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0);
    } while (FindNextFileA(handle, &found));
    FindClose(handle);
#else
    DIR *dir = opendir(directory);
    if (dir == NULL) return;
    const struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child[1024];
        int written = snprintf(child, sizeof child, "%s/%s", directory, entry->d_name);
        if (written < 0 || (size_t) written >= sizeof child) continue;
        struct stat info;
        if (lstat(child, &info) != 0) continue;
        int is_link = S_ISLNK(info.st_mode);
        visit(child, !is_link && S_ISDIR(info.st_mode), is_link);
    }
    closedir(dir);
#endif
}

static void remove_tree(const char *directory);

/* A link is removed as the single node it is rather than descended into:
   fr_unpack writes contained symlinks, and opening a directory symlink for
   listing follows it, which is how an unguarded sweep deletes through one. */
static void remove_child(const char *child_path, int is_directory, int is_link) {
    if (is_link) {
#ifdef _WIN32
        if (is_directory) RemoveDirectoryA(child_path); else remove(child_path);
#else
        remove(child_path);
#endif
        return;
    }
    if (is_directory) remove_tree(child_path);
    else remove(child_path);
}

static void remove_tree(const char *directory) {
    for_each_child(directory, remove_child);
#ifdef _WIN32
    RemoveDirectoryA(directory);
#else
    rmdir(directory);
#endif
}

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

static int fetch_and_verify(const char *url, const char *sha256_hex, const char *archive_path,
                            fr_error *err) {
    fr_sha256 digest;
    size_t length = 0;
    if (fr_http_get_to_file(url, NULL, 0, archive_path, FR_HTTP_MAX_FILE, &digest, &length, err)
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

int fr_provision(const char *url, const char *sha256_hex, fr_provision_result *out,
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
    remove_tree(temporary);
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

    int result = fetch_and_verify(url, sha256_hex, archive_path, err);
    if (result == FR_OK) result = unpack_into(archive_path, tree_path, &report, err);
    if (result == FR_OK && fr_cache_rename_directory(tree_path, out->root) != FR_OK) {
        fr_error_set(err, "cannot move the unpacked tree for %s into the cache", sha256_hex);
        result = FR_ERR;
    }

    remove_tree(temporary);
    if (result != FR_OK) {
        out->root[0] = '\0';
        return FR_ERR;
    }

    out->unpack = report;
    return FR_OK;
}
