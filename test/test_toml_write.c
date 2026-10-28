#include "greatest.h"

#include "config/config_toml.h"
#include "config/toml_write.h"

#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

static enum greatest_test_res round_trips_document(cJSON *original, const char *const *first,
                                                   size_t first_count, char **text_out) {
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

static enum greatest_test_res round_trips(const char *json, const char *const *first,
                                          size_t first_count, char **text_out) {
    cJSON *original = cJSON_Parse(json);
    ASSERT(original != NULL);
    CHECK_CALL(round_trips_document(original, first, first_count, text_out));
    PASS();
}

static enum greatest_test_res round_trips_text(const char *value, char **text_out) {
    cJSON *original = cJSON_CreateObject();
    ASSERT(original != NULL);
    ASSERT(cJSON_AddStringToObject(original, "a", value) != NULL);
    CHECK_CALL(round_trips_document(original, NULL, 0, text_out));
    PASS();
}

static char *with_crlf(const char *text) {
    size_t length = strlen(text);
    char *converted = malloc(length * 2 + 1);
    if (converted == NULL) return NULL;
    char *out = converted;
    for (const char *cursor = text; *cursor != '\0'; cursor++) {
        if (*cursor == '\n') *out++ = '\r';
        *out++ = *cursor;
    }
    *out = '\0';
    return converted;
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
        "{\"a\":\"quote \\\" backslash \\\\ tab \\t bell \\u0007 del \\u007f\"}";
    char *text = NULL;
    CHECK_CALL(round_trips(json, NULL, 0, &text));
    ASSERT(strstr(text, "\\\"") != NULL);
    ASSERT(strstr(text, "\\\\") != NULL);
    ASSERT(strstr(text, "\\t") != NULL);
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

TEST a_one_line_string_holding_triple_quotes_round_trips(void) {
    char *text = NULL;
    CHECK_CALL(round_trips("{\"a\":\"a'''b\",\"x'''y\":\"v\",\"c\":\"q''''\"}", NULL, 0, &text));
    ASSERT(strstr(text, "\\u0027") != NULL);
    ASSERT(strstr(text, "'''") == NULL);
    free(text);
    PASS();
}

TEST a_multi_line_text_is_a_literal(void) {
    char *text = NULL;
    CHECK_CALL(round_trips_text("x\ny\n", &text));
    ASSERT_STR_EQ("a = '''\nx\ny\n'''\n", text);
    free(text);
    PASS();
}

TEST a_crlf_text_is_literal_and_keeps_its_carriage_returns(void) {
    char *text = NULL;
    CHECK_CALL(round_trips_text("x\r\ny\r\n", &text));
    ASSERT(strstr(text, "'''\nx\r\ny\r\n'''") != NULL);
    free(text);
    PASS();
}

TEST a_text_holding_triple_quotes_is_multi_line_basic(void) {
    char *text = NULL;
    CHECK_CALL(round_trips_text("s = '''q'''\n", &text));
    ASSERT_STR_EQ("a = \"\"\"\ns = '''q'''\n\"\"\"\n", text);
    free(text);
    PASS();
}

TEST a_text_ending_in_a_backslash_ends_in_an_escape(void) {
    char *text = NULL;
    CHECK_CALL(round_trips_text("'''\nx\n\\", &text));
    ASSERT(strstr(text, "x\n\\u005C\"\"\"") != NULL);
    free(text);
    CHECK_CALL(round_trips_text("'''\nx\n\\\\", &text));
    ASSERT(strstr(text, "x\n\\\\\\u005C\"\"\"") != NULL);
    free(text);
    CHECK_CALL(round_trips_text("x\n\\", &text));
    ASSERT_STR_EQ("a = '''\nx\n\\'''\n", text);
    free(text);
    PASS();
}

TEST a_lone_carriage_return_or_a_control_character_is_basic(void) {
    static const struct {
        const char *value;
        const char *escape;
    } cases[] = {
        { "x\ry\n", "x\\ry\n" },
        { "x\x01\n", "x\\u0001\n" },
        { "x\x7f\n", "x\\u007F\n" },
        { "x\b\f\n", "x\\u0008\\u000C\n" },
        { "x\n\r", "x\n\\r\"\"\"" },
    };
    for (size_t index = 0; index < sizeof cases / sizeof cases[0]; index++) {
        char *text = NULL;
        CHECK_CALL(round_trips_text(cases[index].value, &text));
        ASSERT(strstr(text, "\"\"\"") != NULL);
        ASSERT(strstr(text, cases[index].escape) != NULL);
        free(text);
    }
    PASS();
}

TEST a_text_ending_in_quotes_round_trips(void) {
    static const char *const literal[] = { "x\n'", "x\n''" };
    static const char *const basic[] = { "'''\nx\n\"", "'''\nx\n\"\"", "'''\nx\n\"\"\"" };
    for (size_t index = 0; index < sizeof literal / sizeof literal[0]; index++) {
        char *text = NULL;
        CHECK_CALL(round_trips_text(literal[index], &text));
        ASSERT(strncmp(text, "a = '''\n", 8) == 0);
        free(text);
    }
    for (size_t index = 0; index < sizeof basic / sizeof basic[0]; index++) {
        char *text = NULL;
        CHECK_CALL(round_trips_text(basic[index], &text));
        ASSERT(strncmp(text, "a = \"\"\"\n", 8) == 0);
        free(text);
    }
    PASS();
}

TEST a_text_starting_with_blank_lines_keeps_them(void) {
    char *text = NULL;
    CHECK_CALL(round_trips_text("\n\nx\n", &text));
    ASSERT_STR_EQ("a = '''\n\n\nx\n'''\n", text);
    free(text);
    PASS();
}

TEST a_multi_line_string_inside_an_array_stays_on_one_line(void) {
    char *text = NULL;
    CHECK_CALL(round_trips("{\"a\":[\"x\\ny\"],\"b\":{\"c\":[{\"d\":\"x\\ny\"}]}}", NULL, 0, &text));
    ASSERT(strstr(text, "a = [\"x\\ny\"]\n") != NULL);
    ASSERT(strstr(text, "d = '''\nx\ny'''\n") != NULL);
    free(text);
    PASS();
}

TEST the_measured_sources_round_trip(void) {
    static const char *const sources[] = {
        "def a = ''\n"
        "def b = \"double ${project.name} $version\"\n"
        "def c = 'C:\\\\Users\\\\x' + \"\\n\\t\\u00e9\"\n"
        "def d = /a\\/b}/\n"
        "def e = /$/\n"
        "def f = \"\"\"...\\\"\"\"\"\"\n"
        "def g = '\t'\n"
        "// caf\xc3\xa9\n"
        "println g // a trailing quote'\n",
        "val s = \"\"\"a } ${\"b\"} c $ \\n\"\"\"\nprintln(s)\n",
        "def s = '''\nmulti\n'''\nprintln s\n",
        "\n\nprintln 'x'\n",
        "println 'a'\nprintln 'x'",
        "def p = 'C:'\ndef q = 'ends in \\",
    };
    for (size_t index = 0; index < sizeof sources / sizeof sources[0]; index++) {
        CHECK_CALL(round_trips_text(sources[index], NULL));
        char *crlf = with_crlf(sources[index]);
        ASSERT(crlf != NULL);
        enum greatest_test_res result = round_trips_text(crlf, NULL);
        free(crlf);
        if (result != GREATEST_TEST_RES_PASS) return result;
    }
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
    RUN_TEST(a_one_line_string_holding_triple_quotes_round_trips);
    RUN_TEST(a_multi_line_text_is_a_literal);
    RUN_TEST(a_crlf_text_is_literal_and_keeps_its_carriage_returns);
    RUN_TEST(a_text_holding_triple_quotes_is_multi_line_basic);
    RUN_TEST(a_text_ending_in_a_backslash_ends_in_an_escape);
    RUN_TEST(a_lone_carriage_return_or_a_control_character_is_basic);
    RUN_TEST(a_text_ending_in_quotes_round_trips);
    RUN_TEST(a_text_starting_with_blank_lines_keeps_them);
    RUN_TEST(a_multi_line_string_inside_an_array_stays_on_one_line);
    RUN_TEST(the_measured_sources_round_trip);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(toml_write);
    GREATEST_MAIN_END();
}
