#include "config/toml_write.h"

#include "config/config_toml.h"
#include "util/error.h"
#include "util/strbuf.h"

#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

#define EXACT_INTEGER_LIMIT 9007199254740992.0

typedef struct {
    const char *const *first;
    size_t first_count;
} key_order;

static int write_value(fr_strbuf *buffer, const cJSON *item, const char *key, fr_error *err);

/* @implNote the vendored tomlc99 refuses a one-line basic string holding three single
   quotes ("triple-s-quote inside string lit"), so the first quote of each run is escaped. */
static int begins_triple_quote(const unsigned char *cursor) {
    return cursor[0] == '\'' && cursor[1] == '\'' && cursor[2] == '\'';
}

static void append_escaped(fr_strbuf *buffer, const char *text) {
    fr_strbuf_append(buffer, "\"");
    for (const unsigned char *cursor = (const unsigned char *) text; *cursor != '\0'; cursor++) {
        if (begins_triple_quote(cursor)) {
            fr_strbuf_append(buffer, "\\u0027");
            continue;
        }
        switch (*cursor) {
            case '"': fr_strbuf_append(buffer, "\\\""); break;
            case '\\': fr_strbuf_append(buffer, "\\\\"); break;
            case '\b': fr_strbuf_append(buffer, "\\b"); break;
            case '\t': fr_strbuf_append(buffer, "\\t"); break;
            case '\n': fr_strbuf_append(buffer, "\\n"); break;
            case '\f': fr_strbuf_append(buffer, "\\f"); break;
            case '\r': fr_strbuf_append(buffer, "\\r"); break;
            default:
                if (*cursor < 0x20 || *cursor == 0x7F) {
                    fr_strbuf_append_format(buffer, "\\u%04X", (unsigned) *cursor);
                } else {
                    fr_strbuf_append_bytes(buffer, (const char *) cursor, 1);
                }
        }
    }
    fr_strbuf_append(buffer, "\"");
}

static int is_bare_key(const char *key) {
    if (key[0] == '\0') return 0;
    for (const char *cursor = key; *cursor != '\0'; cursor++) {
        char letter = *cursor;
        int bare = (letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z') ||
                   (letter >= '0' && letter <= '9') || letter == '_' || letter == '-';
        if (!bare) return 0;
    }
    return 1;
}

static void append_key(fr_strbuf *buffer, const char *key) {
    if (is_bare_key(key)) {
        fr_strbuf_append(buffer, key);
    } else {
        append_escaped(buffer, key);
    }
}

static void append_number(fr_strbuf *buffer, double number) {
    int integral = number >= -EXACT_INTEGER_LIMIT && number <= EXACT_INTEGER_LIMIT &&
                   number == (double) (long long) number;
    if (integral) {
        fr_strbuf_append_format(buffer, "%lld", (long long) number);
    } else {
        fr_strbuf_append_format(buffer, "%.17g", number);
    }
}

static int is_array_of_tables(const cJSON *item) {
    if (!cJSON_IsArray(item) || item->child == NULL) return 0;
    for (const cJSON *element = item->child; element != NULL; element = element->next) {
        if (!cJSON_IsObject(element)) return 0;
    }
    return 1;
}

static int write_inline_array(fr_strbuf *buffer, const cJSON *array, const char *key,
                              fr_error *err) {
    fr_strbuf_append(buffer, "[");
    for (const cJSON *element = array->child; element != NULL; element = element->next) {
        if (element != array->child) fr_strbuf_append(buffer, ", ");
        if (write_value(buffer, element, key, err) != FR_OK) return FR_ERR;
    }
    fr_strbuf_append(buffer, "]");
    return FR_OK;
}

static int write_inline_table(fr_strbuf *buffer, const cJSON *object, fr_error *err) {
    if (object->child == NULL) {
        fr_strbuf_append(buffer, "{}");
        return FR_OK;
    }
    fr_strbuf_append(buffer, "{ ");
    for (const cJSON *member = object->child; member != NULL; member = member->next) {
        if (member != object->child) fr_strbuf_append(buffer, ", ");
        append_key(buffer, member->string);
        fr_strbuf_append(buffer, " = ");
        if (write_value(buffer, member, member->string, err) != FR_OK) return FR_ERR;
    }
    fr_strbuf_append(buffer, " }");
    return FR_OK;
}

static int write_value(fr_strbuf *buffer, const cJSON *item, const char *key, fr_error *err) {
    if (cJSON_IsString(item)) {
        append_escaped(buffer, item->valuestring);
    } else if (cJSON_IsNumber(item)) {
        append_number(buffer, item->valuedouble);
    } else if (cJSON_IsBool(item)) {
        fr_strbuf_append(buffer, cJSON_IsTrue(item) ? "true" : "false");
    } else if (cJSON_IsArray(item)) {
        return write_inline_array(buffer, item, key, err);
    } else if (cJSON_IsObject(item)) {
        return write_inline_table(buffer, item, err);
    } else {
        fr_error_set(err, "cannot write \"%s\" as toml: toml has no null", key);
        return FR_ERR;
    }
    return FR_OK;
}

static int is_listed_first(const key_order *order, const char *key) {
    for (size_t index = 0; index < order->first_count; index++) {
        if (strcmp(order->first[index], key) == 0) return 1;
    }
    return 0;
}

static int fill_in_order(const cJSON *object, const key_order *order, const cJSON **items) {
    size_t count = 0;
    for (size_t index = 0; index < order->first_count; index++) {
        const cJSON *member = cJSON_GetObjectItemCaseSensitive(object, order->first[index]);
        if (member != NULL) items[count++] = member;
    }
    for (const cJSON *member = object->child; member != NULL; member = member->next) {
        if (!is_listed_first(order, member->string)) items[count++] = member;
    }
    return (int) count;
}

static char *child_path(const char *parent, const char *key) {
    fr_strbuf buffer;
    fr_strbuf_init(&buffer);
    if (parent[0] != '\0') {
        fr_strbuf_append(&buffer, parent);
        fr_strbuf_append(&buffer, ".");
    }
    append_key(&buffer, key);
    return fr_strbuf_release(&buffer);
}

static int write_table(fr_strbuf *buffer, const char *path, const cJSON *object,
                       const key_order *order, fr_error *err);

static int write_headed(fr_strbuf *buffer, const char *path, const cJSON *table,
                        const char *open, const char *close, fr_error *err) {
    key_order unordered = { NULL, 0 };
    fr_strbuf_append(buffer, open);
    fr_strbuf_append(buffer, path);
    fr_strbuf_append(buffer, close);
    return write_table(buffer, path, table, &unordered, err);
}

static int write_nested(fr_strbuf *buffer, const char *path, const cJSON *member, fr_error *err) {
    char *member_path = child_path(path, member->string);
    if (member_path == NULL) {
        fr_error_set(err, "out of memory writing \"%s\" as toml", member->string);
        return FR_ERR;
    }
    int status = FR_OK;
    if (cJSON_IsObject(member)) {
        status = write_headed(buffer, member_path, member, "\n[", "]\n", err);
    } else {
        for (const cJSON *element = member->child; element != NULL && status == FR_OK;
             element = element->next) {
            status = write_headed(buffer, member_path, element, "\n[[", "]]\n", err);
        }
    }
    free(member_path);
    return status;
}

static int is_nested(const cJSON *member) {
    return cJSON_IsObject(member) || is_array_of_tables(member);
}

static int write_table(fr_strbuf *buffer, const char *path, const cJSON *object,
                       const key_order *order, fr_error *err) {
    size_t capacity = 0;
    for (const cJSON *member = object->child; member != NULL; member = member->next) capacity++;
    capacity += order->first_count;
    const cJSON **items = malloc((capacity + 1) * sizeof *items);
    if (items == NULL) {
        fr_error_set(err, "out of memory writing a toml table");
        return FR_ERR;
    }
    int count = fill_in_order(object, order, items);

    int status = FR_OK;
    for (int index = 0; index < count && status == FR_OK; index++) {
        if (is_nested(items[index])) continue;
        append_key(buffer, items[index]->string);
        fr_strbuf_append(buffer, " = ");
        status = write_value(buffer, items[index], items[index]->string, err);
        fr_strbuf_append(buffer, "\n");
    }
    for (int pass = 0; pass < 2; pass++) {
        for (int index = 0; index < count && status == FR_OK; index++) {
            int wanted = pass == 0 ? cJSON_IsObject(items[index]) : is_array_of_tables(items[index]);
            if (wanted) status = write_nested(buffer, path, items[index], err);
        }
    }
    free(items);
    return status;
}

static int read_back_equal(const cJSON *document, const char *text, fr_error *err) {
    cJSON *back = NULL;
    if (FR_CONFIG_TOML.load(FR_CONFIG_TOML.state, text, "the written manifest", NULL, NULL, NULL,
                            &back, err) != FR_OK) {
        return FR_ERR;
    }
    int same = cJSON_Compare(document, back, 1);
    cJSON_Delete(back);
    if (!same) {
        fr_error_set(err, "the TOML writer produced text that reads back differently; this is a "
                          "daukle bug");
        return FR_ERR;
    }
    return FR_OK;
}

int fr_toml_write(const cJSON *document, const char *const *first, size_t first_count,
                  char **out, fr_error *err) {
    *out = NULL;
    if (!cJSON_IsObject(document)) {
        fr_error_set(err, "a toml document must be an object");
        return FR_ERR;
    }
    fr_strbuf buffer;
    fr_strbuf_init(&buffer);
    key_order order = { first, first_count };
    if (write_table(&buffer, "", document, &order, err) != FR_OK) {
        fr_strbuf_free(&buffer);
        return FR_ERR;
    }
    char *text = fr_strbuf_release(&buffer);
    if (text == NULL) {
        fr_error_set(err, "out of memory writing a toml document");
        return FR_ERR;
    }
    if (read_back_equal(document, text, err) != FR_OK) {
        free(text);
        return FR_ERR;
    }
    *out = text;
    return FR_OK;
}
