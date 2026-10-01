#include "util/tree.h"

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
        int is_directory = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        int is_link = (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        visit(child, is_directory, is_link);
    } while (FindNextFileA(handle, &found));
    FindClose(handle);
#else
    DIR *dir = opendir(directory);
    if (dir == NULL) return;
    const struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child[1024];
        int child_written = snprintf(child, sizeof child, "%s/%s", directory, entry->d_name);
        if (child_written < 0 || (size_t) child_written >= sizeof child) continue;
        struct stat info;
        if (lstat(child, &info) != 0) continue;
        int is_link = S_ISLNK(info.st_mode);
        visit(child, !is_link && S_ISDIR(info.st_mode), is_link);
    }
    closedir(dir);
#endif
}

/* A link is removed as the single node it is rather than descended into:
   opening a junction or a directory symlink for listing follows it
   transparently, which is exactly how an unguarded sweep deletes whatever the
   link points at instead of the link itself. */
static void remove_child(const char *child_path, int is_directory, int is_link) {
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
    for_each_child(path, remove_child);
#ifdef _WIN32
    RemoveDirectoryA(path);
#else
    rmdir(path);
#endif
}
