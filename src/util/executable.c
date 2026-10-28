#include "util/executable.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdint.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

#define FR_EXECUTABLE_PATH_MAX 4096

static char *directory_of(const char *executable) {
    const char *last = NULL;
    for (const char *scan = executable; *scan != '\0'; scan++) {
        if (*scan == '/' || *scan == '\\') last = scan;
    }
    if (last == NULL) return NULL;
    size_t length = (size_t) (last - executable);
    char *directory = malloc(length + 1);
    if (directory == NULL) return NULL;
    memcpy(directory, executable, length);
    directory[length] = '\0';
    return directory;
}

/* @implNote Windows takes the ANSI spelling because every other path in core goes through narrow
   fopen; a wide path here would name a file the rest of core cannot open. */
char *fr_executable_directory(void) {
    char executable[FR_EXECUTABLE_PATH_MAX];
#if defined(_WIN32)
    DWORD length = GetModuleFileNameA(NULL, executable, (DWORD) sizeof executable);
    if (length == 0 || length >= sizeof executable) return NULL;
#elif defined(__APPLE__)
    char unresolved[FR_EXECUTABLE_PATH_MAX];
    uint32_t size = (uint32_t) sizeof unresolved;
    if (_NSGetExecutablePath(unresolved, &size) != 0) return NULL;
    if (realpath(unresolved, executable) == NULL) return NULL;
#else
    ssize_t length = readlink("/proc/self/exe", executable, sizeof executable - 1);
    if (length <= 0) return NULL;
    executable[length] = '\0';
#endif
    return directory_of(executable);
}
