#include "util/tree.h"

#include "util/error.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <wchar.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifdef _WIN32
static void windows_reason(DWORD code, fr_error *err) {
    char text[256];
    DWORD length = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
                                  code, 0, text, sizeof text, NULL);
    while (length > 0 && (text[length - 1] == '\r' || text[length - 1] == '\n'
                          || text[length - 1] == '.' || text[length - 1] == ' ')) {
        text[--length] = '\0';
    }
    if (length == 0) fr_error_set(err, "Windows error %lu", (unsigned long) code);
    else fr_error_set(err, "%s", text);
}

/* @implNote The ANSI code page in both directions, because every other path in
   core reaches the file system through the ANSI CRT: a name listed here has to
   open there, exactly as it did when this file called the A functions. */
static wchar_t *to_wide(const char *text) {
    int length = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
    if (length <= 0) return NULL;
    wchar_t *wide = malloc((size_t) length * sizeof *wide);
    if (wide != NULL && MultiByteToWideChar(CP_ACP, 0, text, -1, wide, length) <= 0) {
        free(wide);
        return NULL;
    }
    return wide;
}

static char *to_narrow(const wchar_t *text) {
    int length = WideCharToMultiByte(CP_ACP, 0, text, -1, NULL, 0, NULL, NULL);
    if (length <= 0) return NULL;
    char *narrow = malloc((size_t) length);
    if (narrow != NULL && WideCharToMultiByte(CP_ACP, 0, text, -1, narrow, length, NULL, NULL) <= 0) {
        free(narrow);
        return NULL;
    }
    return narrow;
}

static wchar_t *concat_wide(const wchar_t *first, const wchar_t *second, const wchar_t *third) {
    size_t first_length = wcslen(first), second_length = wcslen(second);
    size_t third_length = wcslen(third);
    wchar_t *joined = malloc((first_length + second_length + third_length + 1) * sizeof *joined);
    if (joined == NULL) return NULL;
    memcpy(joined, first, first_length * sizeof *joined);
    memcpy(joined + first_length, second, second_length * sizeof *joined);
    memcpy(joined + first_length + second_length, third, (third_length + 1) * sizeof *joined);
    return joined;
}

static wchar_t *join_wide(const wchar_t *directory, const wchar_t *name) {
    size_t length = wcslen(directory);
    int has_separator = length > 0 && directory[length - 1] == L'\\';
    return concat_wide(directory, has_separator ? L"" : L"\\", name);
}

/* Keeps the two leading separators of a UNC path and the one after a drive. */
static void normalise_separators(wchar_t *path) {
    size_t kept = 0;
    for (size_t at = 0; path[at] != L'\0'; at++) {
        wchar_t character = path[at] == L'/' ? L'\\' : path[at];
        int repeats_separator = character == L'\\' && kept > 1 && path[kept - 1] == L'\\';
        if (!repeats_separator) path[kept++] = character;
    }
    while (kept > 3 && path[kept - 1] == L'\\') kept--;
    path[kept] = L'\0';
}

/* @implNote The \\?\ form is what lifts the MAX_PATH limit, and it also
   switches off the normalisation Windows would otherwise do, so the path is
   made absolute and its separators are normalised here first. */
static wchar_t *extended_path(const char *path) {
    wchar_t *wide = to_wide(path);
    if (wide == NULL) return NULL;
    DWORD needed = GetFullPathNameW(wide, 0, NULL, NULL);
    wchar_t *full = needed == 0 ? NULL : malloc(needed * sizeof *full);
    DWORD written = full == NULL ? 0 : GetFullPathNameW(wide, needed, full, NULL);
    free(wide);
    if (written == 0 || written >= needed) {
        free(full);
        return NULL;
    }
    if (wcsncmp(full, L"\\\\?\\", 4) == 0 || wcsncmp(full, L"\\\\.\\", 4) == 0) return full;
    normalise_separators(full);
    int is_unc = full[0] == L'\\' && full[1] == L'\\';
    wchar_t *extended = is_unc ? concat_wide(L"\\\\?\\UNC\\", L"", full + 2)
                               : concat_wide(L"\\\\?\\", L"", full);
    free(full);
    return extended;
}

typedef void (*wide_visitor)(const WIN32_FIND_DATAW *found, void *context);

static DWORD list_wide(const wchar_t *directory, wide_visitor visit, void *context) {
    wchar_t *pattern = join_wide(directory, L"*");
    if (pattern == NULL) return ERROR_NOT_ENOUGH_MEMORY;
    WIN32_FIND_DATAW found;
    HANDLE handle = FindFirstFileW(pattern, &found);
    DWORD failure = handle == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    free(pattern);
    if (handle == INVALID_HANDLE_VALUE) return failure;
    do {
        if (wcscmp(found.cFileName, L".") == 0 || wcscmp(found.cFileName, L"..") == 0) continue;
        visit(&found, context);
    } while (FindNextFileW(handle, &found));
    FindClose(handle);
    return ERROR_SUCCESS;
}

static int is_directory_entry(const WIN32_FIND_DATAW *found) {
    return (found->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static int is_link_entry(const WIN32_FIND_DATAW *found) {
    return (found->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

typedef struct {
    fr_directory_visitor visit;
    void *context;
} narrow_relay;

static void relay_entry(const WIN32_FIND_DATAW *found, void *context) {
    const narrow_relay *relay = context;
    char *name = to_narrow(found->cFileName);
    if (name == NULL) return;
    relay->visit(name, is_directory_entry(found), is_link_entry(found), relay->context);
    free(name);
}

static void remove_tree_wide(const wchar_t *path);

static void remove_child(const WIN32_FIND_DATAW *found, void *context) {
    wchar_t *child = join_wide(context, found->cFileName);
    if (child == NULL) return;
    if (is_directory_entry(found) && !is_link_entry(found)) remove_tree_wide(child);
    else if (is_directory_entry(found)) RemoveDirectoryW(child);
    else DeleteFileW(child);
    free(child);
}

static void remove_tree_wide(const wchar_t *path) {
    (void) list_wide(path, remove_child, (void *) path);
    RemoveDirectoryW(path);
}
#else
static char *join_path(const char *directory, const char *name) {
    size_t length = strlen(directory) + 1 + strlen(name) + 1;
    char *joined = malloc(length);
    if (joined != NULL) snprintf(joined, length, "%s/%s", directory, name);
    return joined;
}

static void remove_child(const char *name, int is_directory, int is_link, void *context) {
    char *child_path = join_path((const char *) context, name);
    if (child_path == NULL) return;
    if (is_directory && !is_link) fr_remove_tree(child_path);
    else remove(child_path);
    free(child_path);
}
#endif

void fr_list_directory(const char *directory, fr_directory_visitor visit, void *context) {
    fr_error ignored;
    (void) fr_list_directory_checked(directory, visit, context, &ignored);
}

int fr_list_directory_checked(const char *directory, fr_directory_visitor visit, void *context,
                              fr_error *err) {
#ifdef _WIN32
    wchar_t *extended = extended_path(directory);
    if (extended == NULL) {
        fr_error_set(err, "the path cannot be converted for the Windows API");
        return FR_ERR;
    }
    narrow_relay relay = { visit, context };
    DWORD failure = list_wide(extended, relay_entry, &relay);
    free(extended);
    if (failure != ERROR_SUCCESS) {
        windows_reason(failure, err);
        return FR_ERR;
    }
#else
    DIR *dir = opendir(directory);
    if (dir == NULL) {
        fr_error_set(err, "%s", strerror(errno));
        return FR_ERR;
    }
    const struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char *child = join_path(directory, entry->d_name);
        if (child == NULL) continue;
        struct stat info;
        int stat_failed = lstat(child, &info) != 0;
        free(child);
        if (stat_failed) continue;
        int is_link = S_ISLNK(info.st_mode);
        visit(entry->d_name, !is_link && S_ISDIR(info.st_mode), is_link, context);
    }
    closedir(dir);
#endif
    return FR_OK;
}

void fr_remove_tree(const char *path) {
#ifdef _WIN32
    wchar_t *extended = extended_path(path);
    if (extended == NULL) return;
    remove_tree_wide(extended);
    free(extended);
#else
    fr_list_directory(path, remove_child, (void *) path);
    rmdir(path);
#endif
}
