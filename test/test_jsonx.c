#include "greatest.h"
#include "jsonx.h"

#include <stdio.h>
#include <string.h>

static cJSON *parse(const char *text) { return cJSON_Parse(text); }

TEST reads_a_string_member(void) {
    cJSON *root = parse("{\"project\":\"forebay/basekit\"}");
    const char *value = NULL; fr_error err;
    ASSERT_EQ(FR_OK, fr_json_string(root, "project", "$", &value, &err));
    ASSERT_STR_EQ("forebay/basekit", value);
    cJSON_Delete(root);
    PASS();
}

TEST names_the_path_when_a_member_is_missing(void) {
    cJSON *root = parse("{}");
    const char *value = NULL; fr_error err;
    ASSERT_EQ(FR_ERR, fr_json_string(root, "project", "$", &value, &err));
    ASSERT(strstr(err.message, "project") != NULL);
    ASSERT(strstr(err.message, "$") != NULL);
    cJSON_Delete(root);
    PASS();
}

TEST names_the_path_when_a_member_has_the_wrong_type(void) {
    cJSON *root = parse("{\"project\":7}");
    const char *value = NULL; fr_error err;
    ASSERT_EQ(FR_ERR, fr_json_string(root, "project", "$", &value, &err));
    ASSERT(strstr(err.message, "string") != NULL);
    cJSON_Delete(root);
    PASS();
}

TEST reads_an_array_of_strings(void) {
    cJSON *root = parse("{\"requires\":[\"contracts\",\"ir\"]}");
    char **items = NULL; size_t count = 0; fr_error err;
    ASSERT_EQ(FR_OK, fr_json_array_of_strings(root, "requires", "$", &items, &count, &err));
    ASSERT_EQ(2, (int) count);
    ASSERT_STR_EQ("contracts", items[0]);
    ASSERT_STR_EQ("ir", items[1]);
    fr_string_array_free(items, count);
    cJSON_Delete(root);
    PASS();
}

TEST treats_a_missing_array_as_empty(void) {
    cJSON *root = parse("{}");
    char **items = NULL; size_t count = 0; fr_error err;
    ASSERT_EQ(FR_OK, fr_json_array_of_strings(root, "requires", "$", &items, &count, &err));
    ASSERT_EQ(0, (int) count);
    fr_string_array_free(items, count);
    cJSON_Delete(root);
    PASS();
}

TEST reports_a_missing_file_by_name(void) {
    cJSON *root = NULL; fr_error err;
    ASSERT_EQ(FR_ERR, fr_json_read_file("test/fixtures/does-not-exist.json", &root, &err));
    ASSERT(strstr(err.message, "does-not-exist.json") != NULL);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(reads_a_string_member);
    RUN_TEST(names_the_path_when_a_member_is_missing);
    RUN_TEST(names_the_path_when_a_member_has_the_wrong_type);
    RUN_TEST(reads_an_array_of_strings);
    RUN_TEST(treats_a_missing_array_as_empty);
    RUN_TEST(reports_a_missing_file_by_name);
    GREATEST_MAIN_END();
}
