#include "support.h"

#include "miniz.h"

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

#define TAR_BLOCK 512

static void write_octal(char *field, size_t width, unsigned long long value) {
    for (size_t index = width - 1; index > 0; index--) {
        field[index - 1] = (char) ('0' + (value & 7u));
        value >>= 3;
    }
    field[width - 1] = '\0';
}

void fr_test_tar_fix_checksum(char *buffer, size_t offset) {
    char *header = buffer + offset;
    memset(header + 148, ' ', 8);

    unsigned long sum = 0;
    for (size_t index = 0; index < TAR_BLOCK; index++) sum += (unsigned char) header[index];

    write_octal(header + 148, 7, sum);
    header[154] = '\0';
    header[155] = ' ';
}

size_t fr_test_tar_append(char *buffer, size_t offset, const char *name, char typeflag,
                          const char *content, size_t content_length) {
    char *header = buffer + offset;
    memset(header, 0, TAR_BLOCK);

    snprintf(header, 100, "%s", name);
    memcpy(header + 100, "0000644", 8);
    memcpy(header + 108, "0000000", 8);
    memcpy(header + 116, "0000000", 8);
    write_octal(header + 124, 12, content_length);
    write_octal(header + 136, 12, 0);
    header[156] = typeflag;
    memcpy(header + 257, "ustar", 6);
    memcpy(header + 263, "00", 2);
    fr_test_tar_fix_checksum(buffer, offset);

    offset += TAR_BLOCK;
    if (content_length > 0) {
        memcpy(buffer + offset, content, content_length);
        offset += ((content_length + TAR_BLOCK - 1) / TAR_BLOCK) * TAR_BLOCK;
    }
    return offset;
}

size_t fr_test_tar_end(char *buffer, size_t offset) {
    memset(buffer + offset, 0, 2 * TAR_BLOCK);
    return offset + 2 * TAR_BLOCK;
}

#define ZIP_END_OF_CENTRAL_DIRECTORY 0x06054b50u
#define ZIP_CENTRAL_HEADER 0x02014b50u
#define ZIP_CENTRAL_HEADER_SIZE 46u
#define ZIP_END_OF_CENTRAL_DIRECTORY_SIZE 22u

static unsigned long read_little_endian(const unsigned char *bytes, size_t width) {
    unsigned long value = 0;
    for (size_t index = width; index > 0; index--) {
        value = (value << 8) | bytes[index - 1];
    }
    return value;
}

static void write_little_endian(unsigned char *bytes, size_t width, unsigned long value) {
    for (size_t index = 0; index < width; index++) {
        bytes[index] = (unsigned char) (value & 0xffu);
        value >>= 8;
    }
}

static unsigned long unix_mode_of(const char *name, int executable) {
    size_t length = strlen(name);
    if (length > 0 && name[length - 1] == '/') return 040755u;
    return executable ? 0100755u : 0100644u;
}

/* miniz's writer has no say over "version made by" or the external attributes,
   so the fixture patches the central directory it produced. */
static void claim_unix_attributes(unsigned char *zip, size_t length, const char *const *names,
                                  const int *executable) {
    if (length < ZIP_END_OF_CENTRAL_DIRECTORY_SIZE) return;
    size_t end = length - ZIP_END_OF_CENTRAL_DIRECTORY_SIZE;
    while (read_little_endian(zip + end, 4) != ZIP_END_OF_CENTRAL_DIRECTORY) {
        if (end == 0) return;
        end--;
    }

    size_t count = (size_t) read_little_endian(zip + end + 10, 2);
    size_t entry = (size_t) read_little_endian(zip + end + 16, 4);
    for (size_t index = 0; index < count; index++) {
        if (entry + ZIP_CENTRAL_HEADER_SIZE > length) return;
        if (read_little_endian(zip + entry, 4) != ZIP_CENTRAL_HEADER) return;

        zip[entry + 5] = 3;
        write_little_endian(zip + entry + 38, 4, unix_mode_of(names[index], executable[index]) << 16);

        entry += ZIP_CENTRAL_HEADER_SIZE + (size_t) read_little_endian(zip + entry + 28, 2)
                 + (size_t) read_little_endian(zip + entry + 30, 2)
                 + (size_t) read_little_endian(zip + entry + 32, 2);
    }
}

size_t fr_test_zip_build(char *buffer, size_t size, const char *const *names,
                         const char *const *contents, const int *executable, size_t count,
                         int unix_made_by) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof zip);
    if (!mz_zip_writer_init_heap(&zip, 0, 64 * 1024)) return 0;

    for (size_t index = 0; index < count; index++) {
        if (!mz_zip_writer_add_mem(&zip, names[index], contents[index], strlen(contents[index]),
                                   MZ_NO_COMPRESSION)) {
            mz_zip_writer_end(&zip);
            return 0;
        }
    }

    void *built = NULL;
    size_t built_length = 0;
    if (!mz_zip_writer_finalize_heap_archive(&zip, &built, &built_length)) {
        mz_zip_writer_end(&zip);
        return 0;
    }

    size_t length = built_length <= size ? built_length : 0;
    if (length > 0) memcpy(buffer, built, length);
    mz_zip_writer_end(&zip);

    if (length > 0 && unix_made_by) {
        claim_unix_attributes((unsigned char *) buffer, length, names, executable);
    }
    return length;
}

size_t fr_test_gzip(char *out, size_t out_size, const char *data, size_t length) {
    static const unsigned char gzip_header[10] = { 0x1f, 0x8b, 0x08, 0, 0, 0, 0, 0, 0, 0xff };

    int flags = (int) tdefl_create_comp_flags_from_zip_params(MZ_DEFAULT_LEVEL, -15,
                                                              MZ_DEFAULT_STRATEGY);
    size_t deflated_length = 0;
    void *deflated = tdefl_compress_mem_to_heap(data, length, &deflated_length, flags);
    if (deflated == NULL) return 0;

    size_t total = sizeof gzip_header + deflated_length + 8;
    if (total > out_size) {
        mz_free(deflated);
        return 0;
    }

    unsigned char *cursor = (unsigned char *) out;
    memcpy(cursor, gzip_header, sizeof gzip_header);
    memcpy(cursor + sizeof gzip_header, deflated, deflated_length);
    cursor += sizeof gzip_header + deflated_length;
    write_little_endian(cursor, 4, mz_crc32(MZ_CRC32_INIT, (const unsigned char *) data, length));
    write_little_endian(cursor + 4, 4, (unsigned long) length);

    mz_free(deflated);
    return total;
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
