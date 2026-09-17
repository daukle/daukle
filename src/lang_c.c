#include "lang_c.h"

#include "error.h"
#include "jsonx.h"
#include "lang_region.h"
#include "strbuf.h"

#include <stdio.h>
#include <stdlib.h>

static const char *C_BEGIN_MARKER = "# tiestone:begin";
static const char *C_END_MARKER = "# tiestone:end";

typedef struct {
    const char *package;
    const char *url;
    const char *sha256;
} c_entry;

static int read_entries(const fr_resolved *resolved, size_t count, c_entry **out, fr_error *err) {
    *out = NULL;
    if (count == 0) return FR_OK;

    c_entry *entries = malloc(count * sizeof *entries);
    if (entries == NULL) {
        fr_error_set(err, "out of memory reading %zu c blocks", count);
        return FR_ERR;
    }
    for (size_t index = 0; index < count; index++) {
        char path[256];
        snprintf(path, sizeof path, "modules.%s.c", resolved[index].module);
        if (fr_json_string(resolved[index].block, "package", path, &entries[index].package, err) != FR_OK
            || fr_json_string(resolved[index].block, "url", path, &entries[index].url, err) != FR_OK) {
            free(entries);
            return FR_ERR;
        }
        const cJSON *hash = cJSON_GetObjectItemCaseSensitive(resolved[index].block, "sha256");
        entries[index].sha256 = cJSON_IsString(hash) ? hash->valuestring : NULL;
    }
    *out = entries;
    return FR_OK;
}

/* The region carries its own include(FetchContent), so a consumer that pastes
   the markers into a fresh CMakeLists gets working cmake rather than an
   undefined command. */
static void append_declarations(fr_strbuf *buffer, const c_entry *entries, size_t count) {
    fr_strbuf_append(buffer, "include(FetchContent)\n");
    for (size_t index = 0; index < count; index++) {
        fr_strbuf_append_format(buffer, "FetchContent_Declare(%s URL \"%s\"",
                                entries[index].package, entries[index].url);
        if (entries[index].sha256 != NULL) {
            fr_strbuf_append_format(buffer, " URL_HASH SHA256=%s", entries[index].sha256);
        }
        fr_strbuf_append(buffer, ")\n");
        fr_strbuf_append_format(buffer, "FetchContent_MakeAvailable(%s)\n", entries[index].package);
    }
}

static int c_render(const fr_consumer *consumer, const fr_resolved *resolved, size_t count,
                    char **out_text, fr_error *err) {
    *out_text = NULL;

    if (consumer->configuration == NULL || consumer->configuration[0] == '\0') {
        fr_error_set(err, "consumer has no \"configuration\" for the c language plugin");
        return FR_ERR;
    }

    c_entry *entries = NULL;
    if (read_entries(resolved, count, &entries, err) != FR_OK) return FR_ERR;

    fr_strbuf buffer;
    fr_strbuf_init(&buffer);
    if (count > 0) {
        append_declarations(&buffer, entries, count);
        fr_strbuf_append_format(&buffer, "target_link_libraries(%s PRIVATE", consumer->configuration);
        for (size_t index = 0; index < count; index++) {
            fr_strbuf_append_format(&buffer, " %s", entries[index].package);
        }
        fr_strbuf_append(&buffer, ")");
    }
    free(entries);

    char *text = fr_strbuf_release(&buffer);
    if (text == NULL) {
        fr_error_set(err, "out of memory rendering %zu cmake declarations", count);
        return FR_ERR;
    }
    *out_text = text;
    return FR_OK;
}

static int c_apply(void *state, const fr_consumer *consumer, const fr_resolved *resolved,
                   size_t count, const char *original_text, char **out_text, fr_error *err) {
    (void) state;
    return fr_lang_region_apply(C_BEGIN_MARKER, C_END_MARKER, c_render,
                                consumer, resolved, count, original_text, out_text, err);
}

const fr_language_plugin FR_LANGUAGE_C = { "tiestone.language/c", c_apply, NULL };
