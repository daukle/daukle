#include "util/error.h"

#include <stdarg.h>
#include <stdio.h>

void fr_error_set(fr_error *err, const char *fmt, ...) {
    if (err == NULL) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(err->message, sizeof err->message, fmt, args);
    va_end(args);
}
