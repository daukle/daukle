#include "greatest.h"
#include "jsonedit.h"

#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

static const char *PACKAGE =
    "{\n"
    "  \"name\": \"stub-translator\",\n"
    "  \"dependencies\": {\n"
    "    \"@openauthjs/openauth\": \"^0.4.3\",\n"
    "    \"@intisy-ai/basekit-ir\": \"^4.0.0\"\n"
    "  }\n"
    "}\n";

TEST replaces_a_value_in_place(void) {
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_OK, fr_json_edit_set_string(PACKAGE, "dependencies", "@intisy-ai/basekit-ir",
                                             "^5.0.0", &out, &err));
    ASSERT_STR_EQ(
        "{\n"
        "  \"name\": \"stub-translator\",\n"
        "  \"dependencies\": {\n"
        "    \"@openauthjs/openauth\": \"^0.4.3\",\n"
        "    \"@intisy-ai/basekit-ir\": \"^5.0.0\"\n"
        "  }\n"
        "}\n", out);
    free(out);
    PASS();
}

TEST appends_a_new_member_at_the_existing_indentation(void) {
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_OK, fr_json_edit_set_string(PACKAGE, "dependencies", "@intisy/bayonet",
                                             "^1.8.0", &out, &err));
    ASSERT_STR_EQ(
        "{\n"
        "  \"name\": \"stub-translator\",\n"
        "  \"dependencies\": {\n"
        "    \"@openauthjs/openauth\": \"^0.4.3\",\n"
        "    \"@intisy-ai/basekit-ir\": \"^4.0.0\",\n"
        "    \"@intisy/bayonet\": \"^1.8.0\"\n"
        "  }\n"
        "}\n", out);
    free(out);
    PASS();
}

TEST creates_a_missing_nested_path(void) {
    char *out = NULL; fr_error err;
    const char *managed[] = { "@intisy-ai/basekit-ir" };
    ASSERT_EQ(FR_OK, fr_json_edit_set_string_array(PACKAGE, "daukle.managed", "dependencies",
                                                   managed, 1, &out, &err));
    ASSERT_STR_EQ(
        "{\n"
        "  \"name\": \"stub-translator\",\n"
        "  \"dependencies\": {\n"
        "    \"@openauthjs/openauth\": \"^0.4.3\",\n"
        "    \"@intisy-ai/basekit-ir\": \"^4.0.0\"\n"
        "  },\n"
        "  \"daukle\": {\n"
        "    \"managed\": {\n"
        "      \"dependencies\": [\n"
        "        \"@intisy-ai/basekit-ir\"\n"
        "      ]\n"
        "    }\n"
        "  }\n"
        "}\n", out);
    free(out);
    PASS();
}

TEST writes_an_empty_array_inline(void) {
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_OK, fr_json_edit_set_string_array(PACKAGE, "", "keywords", NULL, 0, &out, &err));
    ASSERT(strstr(out, "\"keywords\": []\n}\n") != NULL);
    free(out);
    PASS();
}

TEST removes_a_member_and_its_separator(void) {
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_OK, fr_json_edit_remove(PACKAGE, "dependencies", "@intisy-ai/basekit-ir",
                                         &out, &err));
    ASSERT_STR_EQ(
        "{\n"
        "  \"name\": \"stub-translator\",\n"
        "  \"dependencies\": {\n"
        "    \"@openauthjs/openauth\": \"^0.4.3\"\n"
        "  }\n"
        "}\n", out);
    free(out);
    PASS();
}

TEST removes_the_first_of_several(void) {
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_OK, fr_json_edit_remove(PACKAGE, "dependencies", "@openauthjs/openauth",
                                         &out, &err));
    ASSERT_STR_EQ(
        "{\n"
        "  \"name\": \"stub-translator\",\n"
        "  \"dependencies\": {\n"
        "    \"@intisy-ai/basekit-ir\": \"^4.0.0\"\n"
        "  }\n"
        "}\n", out);
    free(out);
    PASS();
}

TEST removing_the_only_member_leaves_an_empty_object(void) {
    const char *text = "{\n  \"deps\": {\n    \"a\": \"1\"\n  }\n}\n";
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_OK, fr_json_edit_remove(text, "deps", "a", &out, &err));
    ASSERT_STR_EQ("{\n  \"deps\": {}\n}\n", out);
    free(out);
    PASS();
}

TEST removing_an_absent_key_changes_nothing(void) {
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_OK, fr_json_edit_remove(PACKAGE, "dependencies", "absent", &out, &err));
    ASSERT_STR_EQ(PACKAGE, out);
    free(out);
    PASS();
}

TEST removing_from_an_absent_object_changes_nothing(void) {
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_OK, fr_json_edit_remove(PACKAGE, "daukle.managed", "dependencies", &out, &err));
    ASSERT_STR_EQ(PACKAGE, out);
    free(out);
    PASS();
}

TEST braces_and_quotes_inside_a_string_do_not_move_the_nesting(void) {
    const char *text =
        "{\n"
        "  \"description\": \"a { b [ c \\\" d } e\",\n"
        "  \"deps\": {\n"
        "    \"a\": \"1\"\n"
        "  }\n"
        "}\n";
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_OK, fr_json_edit_set_string(text, "deps", "a", "2", &out, &err));
    ASSERT_STR_EQ(
        "{\n"
        "  \"description\": \"a { b [ c \\\" d } e\",\n"
        "  \"deps\": {\n"
        "    \"a\": \"2\"\n"
        "  }\n"
        "}\n", out);
    free(out);
    PASS();
}

TEST keeps_a_tab_indented_document_tab_indented(void) {
    const char *text = "{\n\t\"deps\": {\n\t\t\"a\": \"1\"\n\t}\n}\n";
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_OK, fr_json_edit_set_string(text, "deps", "b", "2", &out, &err));
    ASSERT_STR_EQ("{\n\t\"deps\": {\n\t\t\"a\": \"1\",\n\t\t\"b\": \"2\"\n\t}\n}\n", out);
    free(out);
    PASS();
}

TEST keeps_a_four_space_document_four_spaced(void) {
    const char *text = "{\n    \"deps\": {}\n}\n";
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_OK, fr_json_edit_set_string(text, "deps", "a", "1", &out, &err));
    ASSERT_STR_EQ("{\n    \"deps\": {\n        \"a\": \"1\"\n    }\n}\n", out);
    free(out);
    PASS();
}

TEST edits_a_single_line_object(void) {
    const char *text = "{\"deps\":{\"a\":\"1\"}}";
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_OK, fr_json_edit_set_string(text, "deps", "b", "2", &out, &err));
    ASSERT_STR_EQ("{\"deps\":{\"a\":\"1\", \"b\": \"2\"}}", out);
    free(out);
    PASS();
}

TEST survives_a_document_without_a_trailing_newline(void) {
    const char *text = "{\n  \"deps\": {\n    \"a\": \"1\"\n  }\n}";
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_OK, fr_json_edit_set_string(text, "deps", "a", "2", &out, &err));
    ASSERT_STR_EQ("{\n  \"deps\": {\n    \"a\": \"2\"\n  }\n}", out);
    free(out);
    PASS();
}

TEST escapes_a_value_that_needs_it(void) {
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_OK, fr_json_edit_set_string(PACKAGE, "", "note", "a \"b\" \\ c", &out, &err));
    ASSERT(strstr(out, "\"note\": \"a \\\"b\\\" \\\\ c\"") != NULL);
    cJSON *parsed = cJSON_Parse(out);
    ASSERT(parsed != NULL);
    cJSON_Delete(parsed);
    free(out);
    PASS();
}

TEST every_result_is_still_valid_json(void) {
    const char *managed[] = { "a", "b" };
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_OK, fr_json_edit_set_string_array(PACKAGE, "daukle.managed", "dependencies",
                                                   managed, 2, &out, &err));
    cJSON *parsed = cJSON_Parse(out);
    ASSERT(parsed != NULL);
    cJSON_Delete(parsed);

    char *removed = NULL;
    ASSERT_EQ(FR_OK, fr_json_edit_remove(out, "dependencies", "@openauthjs/openauth", &removed, &err));
    parsed = cJSON_Parse(removed);
    ASSERT(parsed != NULL);
    cJSON_Delete(parsed);
    free(removed);
    free(out);
    PASS();
}

TEST reports_a_path_segment_that_is_not_an_object(void) {
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_ERR, fr_json_edit_set_string(PACKAGE, "name", "a", "1", &out, &err));
    ASSERT(strstr(err.message, "not an object") != NULL);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(replaces_a_value_in_place);
    RUN_TEST(appends_a_new_member_at_the_existing_indentation);
    RUN_TEST(creates_a_missing_nested_path);
    RUN_TEST(writes_an_empty_array_inline);
    RUN_TEST(removes_a_member_and_its_separator);
    RUN_TEST(removes_the_first_of_several);
    RUN_TEST(removing_the_only_member_leaves_an_empty_object);
    RUN_TEST(removing_an_absent_key_changes_nothing);
    RUN_TEST(removing_from_an_absent_object_changes_nothing);
    RUN_TEST(braces_and_quotes_inside_a_string_do_not_move_the_nesting);
    RUN_TEST(keeps_a_tab_indented_document_tab_indented);
    RUN_TEST(keeps_a_four_space_document_four_spaced);
    RUN_TEST(edits_a_single_line_object);
    RUN_TEST(survives_a_document_without_a_trailing_newline);
    RUN_TEST(escapes_a_value_that_needs_it);
    RUN_TEST(every_result_is_still_valid_json);
    RUN_TEST(reports_a_path_segment_that_is_not_an_object);
    GREATEST_MAIN_END();
}
