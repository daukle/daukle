#include "util/tree.h"

#include "util/error.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
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
#endif

void fr_list_directory(const char *directory, fr_directory_visitor visit, void *context) {
    fr_error ignored;
    (void) fr_list_directory_checked(directory, visit, context, &ignored);
}

int fr_list_directory_checked(const char *directory, fr_directory_visitor visit, void *context,
                              fr_error *err) {
#ifdef _WIN32
    char pattern[1024];
    int written = snprintf(pattern, sizeof pattern, "%s/*", directory);
    if (written < 0 || (size_t) written >= sizeof pattern) {
        fr_error_set(err, "the path is too long");
        return FR_ERR;
    }

    WIN32_FIND_DATAA found;
    HANDLE handle = FindFirstFileA(pattern, &found);
    if (handle == INVALID_HANDLE_VALUE) {
        windows_reason(GetLastError(), err);
        return FR_ERR;
    }
    do {
        if (strcmp(found.cFileName, ".") == 0 || strcmp(found.cFileName, "..") == 0) continue;
        int is_directory = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        int is_link = (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        visit(found.cFileName, is_directory, is_link, context);
    } while (FindNextFileA(handle, &found));
    FindClose(handle);
#else
    DIR *dir = opendir(directory);
    if (dir == NULL) {
        fr_error_set(err, "%s", strerror(errno));
        return FR_ERR;
    }
    const struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child[1024];
        int child_written = snprintf(child, sizeof child, "%s/%s", directory, entry->d_name);
        if (child_written < 0 || (size_t) child_written >= sizeof child) continue;
        struct stat info;
        if (lstat(child, &info) != 0) continue;
        int is_link = S_ISLNK(info.st_mode);
        visit(entry->d_name, !is_link && S_ISDIR(info.st_mode), is_link, context);
    }
    closedir(dir);
#endif
    return FR_OK;
}

/* A link is removed as the single node it is rather than descended into:
   opening a junction or a directory symlink for listing follows it
   transparently, which is exactly how an unguarded sweep deletes whatever the
   link points at instead of the link itself. */
static void remove_child(const char *name, int is_directory, int is_link, void *context) {
    char child_path[1024];
    int written = snprintf(child_path, sizeof child_path, "%s/%s", (const char *) context, name);
    if (written < 0 || (size_t) written >= sizeof child_path) return;
    if (is_link) {
#ifdef _WIN32
        if (is_directory) RemoveDirectoryA(child_path); else remove(child_path);
#else
        remove(child_path);
#endif
        return;
    }
    if (is_directory) fr_remove_tree(child_path);
    else remove(child_path);
}

void fr_remove_tree(const char *path) {
    fr_list_directory(path, remove_child, (void *) path);
#ifdef _WIN32
    RemoveDirectoryA(path);
#else
    rmdir(path);
#endif
}
