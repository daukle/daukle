#include "support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#endif

const char *fr_test_temp_base(void) {
#ifdef _WIN32
    const char *base = getenv("TEMP");
    if (base == NULL) base = getenv("TMP");
    if (base == NULL) base = "C:/Windows/Temp";
#else
    const char *base = getenv("TMPDIR");
    if (base == NULL) base = "/tmp";
#endif
    return base;
}

int fr_test_process_id(void) {
#ifdef _WIN32
    return _getpid();
#else
    return (int) getpid();
#endif
}

int fr_test_make_directory(const char *path) {
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0777);
#endif
}

typedef void (*child_visitor)(const char *child_path, int is_directory, void *state);

static void for_each_child(const char *path, child_visitor visit, void *state) {
#ifdef _WIN32
    char pattern[1024];
    snprintf(pattern, sizeof pattern, "%s/*", path);

    WIN32_FIND_DATAA found;
    HANDLE handle = FindFirstFileA(pattern, &found);
    if (handle == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(found.cFileName, ".") == 0 || strcmp(found.cFileName, "..") == 0) continue;
        char child[1024];
        snprintf(child, sizeof child, "%s/%s", path, found.cFileName);
        visit(child, (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, state);
    } while (FindNextFileA(handle, &found));
    FindClose(handle);
#else
    DIR *dir = opendir(path);
    if (dir == NULL) return;
    const struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child[1024];
        snprintf(child, sizeof child, "%s/%s", path, entry->d_name);
        struct stat info;
        if (stat(child, &info) != 0) continue;
        visit(child, S_ISDIR(info.st_mode), state);
    }
    closedir(dir);
#endif
}

static void remove_child(const char *child_path, int is_directory, void *state) {
    (void) state;
    if (is_directory) fr_test_remove_tree(child_path);
    else remove(child_path);
}

/* Lets a test wipe its own private root without knowing the layout written
   inside it, so a change to the cache path cannot leave stale entries that a
   later test then reads as a hit. */
void fr_test_remove_tree(const char *path) {
    for_each_child(path, remove_child, NULL);
#ifdef _WIN32
    _rmdir(path);
#else
    rmdir(path);
#endif
}

typedef struct {
    const char *name;
    int count;
} file_counter;

static void count_child(const char *child_path, int is_directory, void *state) {
    file_counter *counter = state;
    if (is_directory) {
        for_each_child(child_path, count_child, state);
        return;
    }
    const char *slash = strrchr(child_path, '/');
    const char *base = slash == NULL ? child_path : slash + 1;
    if (strcmp(base, counter->name) == 0) counter->count++;
}

int fr_test_count_files(const char *root, const char *file_name) {
    file_counter counter;
    counter.name = file_name;
    counter.count = 0;
    for_each_child(root, count_child, &counter);
    return counter.count;
}

void fr_test_set_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value == NULL ? "" : value);
#else
    if (value == NULL) unsetenv(name);
    else setenv(name, value, 1);
#endif
}

int fr_test_get_working_directory(char *buffer, size_t size) {
#ifdef _WIN32
    return _getcwd(buffer, (int) size) != NULL;
#else
    return getcwd(buffer, size) != NULL;
#endif
}

int fr_test_set_working_directory(const char *path) {
#ifdef _WIN32
    return _chdir(path) == 0;
#else
    return chdir(path) == 0;
#endif
}

void fr_test_prepend_to_path_dir_of(const char *argv_zero) {
#ifdef _WIN32
    const char list_separator = ';';
#else
    const char list_separator = ':';
#endif
    const char *last_slash = strrchr(argv_zero, '/');
    const char *last_backslash = strrchr(argv_zero, '\\');
    const char *end_of_dir = last_slash;
    if (last_backslash != NULL && (end_of_dir == NULL || last_backslash > end_of_dir)) {
        end_of_dir = last_backslash;
    }
    size_t dir_length = end_of_dir == NULL ? 0 : (size_t) (end_of_dir - argv_zero);

    const char *old_path = getenv("PATH");
    if (old_path == NULL) old_path = "";

    size_t size = dir_length + 1 + strlen(old_path) + 1;
    char *new_path = malloc(size);
    if (new_path == NULL) return;
    if (dir_length > 0) {
        memcpy(new_path, argv_zero, dir_length);
        new_path[dir_length] = list_separator;
        memcpy(new_path + dir_length + 1, old_path, strlen(old_path) + 1);
    } else {
        memcpy(new_path, old_path, strlen(old_path) + 1);
    }

    fr_test_set_env("PATH", new_path);
    free(new_path);
}

long long fr_test_file_mtime(const char *path) {
#ifdef _WIN32
    struct _stat info;
    if (_stat(path, &info) != 0) return 0;
    return (long long) info.st_mtime;
#else
    struct stat info;
    if (stat(path, &info) != 0) return 0;
    return (long long) info.st_mtime;
#endif
}

void fr_test_sleep_past_mtime_resolution(void) {
#ifdef _WIN32
    Sleep(1100);
#else
    struct timespec duration;
    duration.tv_sec = 1;
    duration.tv_nsec = 100000000;
    nanosleep(&duration, NULL);
#endif
}
