#include "greatest.h"

#include "config/config_toml.h"
#include "config/toml_write.h"

#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

static enum greatest_test_res round_trips(const char *json, const char *const *first,
                                          size_t first_count, char **text_out) {
    cJSON *original = cJSON_Parse(json);
    ASSERT(original != NULL);

    fr_error err;
    char *text = NULL;
    ASSERT_EQm("the writer refused a document it should write", FR_OK,
              fr_toml_write(original, first, first_count, &text, &err));
    ASSERT(text != NULL);

    cJSON *back = NULL;
    ASSERT_EQm("the written text does not parse", FR_OK,
               FR_CONFIG_TOML.load(FR_CONFIG_TOML.state, text, "t", NULL, NULL, NULL, &back, &err));
    ASSERTm("the text read back as a different document", cJSON_Compare(original, back, 1));

    cJSON_Delete(back);
    cJSON_Delete(original);
    if (text_out != NULL) {
        *text_out = text;
    } else {
        free(text);
    }
    PASS();
}

TEST scalars_tables_and_arrays_round_trip(void) {
    const char *json =
        "{\"schema\":1,\"project\":\"p\",\"version\":\"1.0.0\",\"modules\":{},"
        "\"plugins\":{\"node\":{\"url\":\"https://x.test/p.lua\",\"sha256\":\"ab\"}},"
        "\"toolchains\":{\"web\":{\"toolchain\":\"node\",\"dir\":\"apps/web\","
        "\"scripts\":[\"build\",\"test\"],\"flags\":{\"strict\":true,\"level\":3}}}}";
    const char *first[] = { "schema", "project", "version", "modules", "plugins" };
    char *text = NULL;
    CHECK_CALL(round_trips(json, first, 5, &text));
    const char *prefix = "schema = 1\nproject = \"p\"\nversion = \"1.0.0\"\n";
    ASSERT(strncmp(text, prefix, strlen(prefix)) == 0);
    ASSERT(strstr(text, "[toolchains.web.flags]") != NULL);
    ASSERT(strstr(text, "scripts = [\"build\", \"test\"]") != NULL);
    free(text);
    PASS();
}

TEST strings_are_escaped(void) {
    const char *json =
        "{\"a\":\"quote \\\" backslash \\\\ tab \\t newline \\n bell \\u0007 del \\u007f\"}";
    char *text = NULL;
    CHECK_CALL(round_trips(json, NULL, 0, &text));
    ASSERT(strstr(text, "\\\"") != NULL);
    ASSERT(strstr(text, "\\\\") != NULL);
    ASSERT(strstr(text, "\\t") != NULL);
    ASSERT(strstr(text, "\\n") != NULL);
    ASSERT(strstr(text, "\\u0007") != NULL);
    ASSERT(strstr(text, "\\u007F") != NULL);
    free(text);
    PASS();
}

TEST a_key_that_is_not_bare_is_quoted(void) {
    char *text = NULL;
    CHECK_CALL(round_trips("{\"tasks\":{\"web:build\":{\"dependsOn\":[\"x\"]}}}", NULL, 0, &text));
    ASSERT(strstr(text, "[tasks.\"web:build\"]") != NULL);
    free(text);
    PASS();
}

TEST an_array_of_tables_round_trips(void) {
    char *text = NULL;
    CHECK_CALL(round_trips("{\"run\":[{\"tool\":\"a\"},{\"tool\":\"b\"}]}", NULL, 0, &text));
    const char *header = strstr(text, "[[run]]");
    ASSERT(header != NULL);
    ASSERT(strstr(header + 1, "[[run]]") != NULL);
    free(text);
    PASS();
}

TEST a_mixed_array_holding_an_object_is_an_inline_table(void) {
    char *text = NULL;
    CHECK_CALL(round_trips("{\"items\":[1,{\"k\":\"v\"}],\"empty\":[]}", NULL, 0, &text));
    ASSERT(strstr(text, "{ k = \"v\" }") != NULL);
    free(text);
    PASS();
}

TEST null_is_refused(void) {
    cJSON *document = cJSON_Parse("{\"a\":null}");
    ASSERT(document != NULL);
    fr_error err;
    char *text = NULL;
    ASSERT_EQ(FR_ERR, fr_toml_write(document, NULL, 0, &text, &err));
    ASSERT(text == NULL);
    ASSERT(strstr(err.message, "\"a\"") != NULL);
    cJSON_Delete(document);
    PASS();
}

TEST a_fractional_number_stays_fractional(void) {
    char *text = NULL;
    CHECK_CALL(round_trips("{\"ratio\":0.5,\"count\":3}", NULL, 0, &text));
    ASSERT(strstr(text, "count = 3\n") != NULL);
    ASSERT(strstr(text, "3.0") == NULL);
    ASSERT(strstr(text, "ratio = 0.5") != NULL);
    free(text);
    PASS();
}

SUITE(toml_write) {
    RUN_TEST(scalars_tables_and_arrays_round_trip);
    RUN_TEST(strings_are_escaped);
    RUN_TEST(a_key_that_is_not_bare_is_quoted);
    RUN_TEST(an_array_of_tables_round_trips);
    RUN_TEST(a_mixed_array_holding_an_object_is_an_inline_table);
    RUN_TEST(null_is_refused);
    RUN_TEST(a_fractional_number_stays_fractional);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(toml_write);
    GREATEST_MAIN_END();
}
