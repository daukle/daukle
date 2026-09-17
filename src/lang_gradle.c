#include "lang_gradle.h"

#include "error.h"
#include "jsonx.h"
#include "lang_region.h"

#include <stdio.h>
#include <stdlib.h>

static const char *GRADLE_LINE_FORMAT = "%s \"%s\"";
static const char *GRADLE_BEGIN_MARKER = "// daukle:begin";
static const char *GRADLE_END_MARKER = "// daukle:end";

static int coordinate_of(const fr_resolved *entry, const char **out, fr_error *err) {
    char path[256];
    snprintf(path, sizeof path, "modules.%s.gradle", entry->module);
    return fr_json_string(entry->block, "coordinate", path, out, err);
}

static int gradle_render(const fr_consumer *consumer, const fr_resolved *resolved,
                         size_t count, char **out_text, fr_error *err) {
    *out_text = NULL;

    if (consumer->configuration == NULL || consumer->configuration[0] == '\0') {
        fr_error_set(err, "consumer has no \"configuration\" for the gradle language plugin");
        return FR_ERR;
    }

    const char **coordinates = NULL;
    if (count > 0) {
        coordinates = malloc(count * sizeof *coordinates);
        if (coordinates == NULL) {
            fr_error_set(err, "out of memory rendering %zu gradle dependency lines", count);
            return FR_ERR;
        }
        for (size_t index = 0; index < count; index++) {
            if (coordinate_of(&resolved[index], &coordinates[index], err) != FR_OK) {
                free(coordinates);
                return FR_ERR;
            }
        }
    }

    size_t total_length = 1;
    for (size_t index = 0; index < count; index++) {
        int line_length = snprintf(NULL, 0, GRADLE_LINE_FORMAT,
                                   consumer->configuration, coordinates[index]);
        if (line_length < 0) {
            fr_error_set(err, "failed formatting gradle dependency line %zu", index);
            free(coordinates);
            return FR_ERR;
        }
        total_length += (size_t) line_length;
        if (index + 1 < count) total_length += 1;
    }

    char *text = malloc(total_length);
    if (text == NULL) {
        fr_error_set(err, "out of memory rendering %zu gradle dependency lines", count);
        free(coordinates);
        return FR_ERR;
    }

    char *cursor = text;
    size_t remaining = total_length;
    for (size_t index = 0; index < count; index++) {
        int written = snprintf(cursor, remaining, GRADLE_LINE_FORMAT,
                               consumer->configuration, coordinates[index]);
        if (written < 0) {
            free(text);
            fr_error_set(err, "failed formatting gradle dependency line %zu", index);
            free(coordinates);
            return FR_ERR;
        }
        cursor += written;
        remaining -= (size_t) written;
        if (index + 1 < count) {
            *cursor++ = '\n';
            remaining -= 1;
        }
    }
    *cursor = '\0';

    free(coordinates);
    *out_text = text;
    return FR_OK;
}

static int gradle_apply(void *state, const fr_consumer *consumer, const fr_resolved *resolved,
                        size_t count, const char *original_text, char **out_text, fr_error *err) {
    (void) state;
    return fr_lang_region_apply(GRADLE_BEGIN_MARKER, GRADLE_END_MARKER, gradle_render,
                                consumer, resolved, count, original_text, out_text, err);
}

const fr_language_plugin FR_LANGUAGE_GRADLE = { "daukle.language/gradle", gradle_apply, NULL };
