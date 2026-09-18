#include "greatest.h"
#include "config_toml.h"
#include "region.h"

#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

static cJSON *parse_fixture(const char *path, int *status) {
    fr_error err;
    char *text = NULL;
    if (fr_file_read_text(path, &text, &err) != FR_OK) { *status = FR_ERR; return NULL; }
    cJSON *document = NULL;
    *status = FR_CONFIG_TOML.load(FR_CONFIG_TOML.state, text, path, ".", NULL, NULL, &document, &err);
    free(text);
    return document;
}

TEST maps_every_toml_type_onto_json(void) {
    int status = FR_ERR;
    cJSON *document = parse_fixture("test/fixtures/toml/types.toml", &status);
    ASSERT_EQ(FR_OK, status);
    ASSERT(cJSON_IsObject(document));

    ASSERT_STR_EQ("daukle", cJSON_GetObjectItemCaseSensitive(document, "name")->valuestring);
    ASSERT_EQ(3, cJSON_GetObjectItemCaseSensitive(document, "count")->valueint);
    ASSERT_EQ(1.5, cJSON_GetObjectItemCaseSensitive(document, "ratio")->valuedouble);
    ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(document, "enabled")));

    const cJSON *tags = cJSON_GetObjectItemCaseSensitive(document, "tags");
    ASSERT(cJSON_IsArray(tags));
    ASSERT_EQ(2, cJSON_GetArraySize(tags));

    const cJSON *empty_array = cJSON_GetObjectItemCaseSensitive(document, "empty_array");
    ASSERT(cJSON_IsArray(empty_array));
    ASSERT_EQ(0, cJSON_GetArraySize(empty_array));

    ASSERT_STR_EQ("2024-03-15T13:45:30+02:00",
                  cJSON_GetObjectItemCaseSensitive(document, "offset_date_time")->valuestring);
    ASSERT_STR_EQ("2024-03-15", cJSON_GetObjectItemCaseSensitive(document, "local_date")->valuestring);
    ASSERT_STR_EQ("13:45:30", cJSON_GetObjectItemCaseSensitive(document, "local_time")->valuestring);
    ASSERT_STR_EQ("2024-03-15T13:45:30.250",
                  cJSON_GetObjectItemCaseSensitive(document, "local_date_time_ms")->valuestring);

    const cJSON *timestamps = cJSON_GetObjectItemCaseSensitive(document, "timestamps");
    ASSERT(cJSON_IsArray(timestamps));
    ASSERT_EQ(2, cJSON_GetArraySize(timestamps));
    ASSERT_STR_EQ("2024-03-15T13:45:30+02:00", cJSON_GetArrayItem(timestamps, 0)->valuestring);
    ASSERT_STR_EQ("2024-03-15", cJSON_GetArrayItem(timestamps, 1)->valuestring);

    const cJSON *nested = cJSON_GetObjectItemCaseSensitive(document, "nested");
    ASSERT_STR_EQ("value", cJSON_GetObjectItemCaseSensitive(nested, "key")->valuestring);

    const cJSON *empty_table = cJSON_GetObjectItemCaseSensitive(document, "empty_table");
    ASSERT(cJSON_IsObject(empty_table));
    ASSERT_EQ(0, cJSON_GetArraySize(empty_table));

    const cJSON *rows = cJSON_GetObjectItemCaseSensitive(document, "rows");
    ASSERT(cJSON_IsArray(rows));
    ASSERT_EQ(2, cJSON_GetArraySize(rows));
    ASSERT_STR_EQ("second", cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(rows, 1), "id")->valuestring);

    cJSON_Delete(document);
    PASS();
}

TEST reports_a_broken_toml_file_with_its_origin(void) {
    fr_error err;
    char *text = NULL;
    fr_file_read_text("test/fixtures/toml/broken.toml", &text, &err);
    cJSON *document = NULL;
    ASSERT_EQ(FR_ERR, FR_CONFIG_TOML.load(FR_CONFIG_TOML.state, text,
                                          "test/fixtures/toml/broken.toml", ".", NULL, NULL,
                                          &document, &err));
    ASSERT(strstr(err.message, "broken.toml") != NULL);
    free(text);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(maps_every_toml_type_onto_json);
    RUN_TEST(reports_a_broken_toml_file_with_its_origin);
    GREATEST_MAIN_END();
}
