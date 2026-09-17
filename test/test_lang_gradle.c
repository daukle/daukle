#include "greatest.h"
#include "lang_gradle.h"

#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

static const char *TEMPLATE =
    "dependencies {\n"
    "    // ferrule:begin\n"
    "    // ferrule:end\n"
    "    testImplementation \"junit\"\n"
    "}\n";

static fr_resolved two_modules[2];

static cJSON *block_with_coordinate(const char *coordinate) {
    cJSON *block = cJSON_CreateObject();
    cJSON_AddStringToObject(block, "coordinate", coordinate);
    return block;
}

static void setup(void) {
    two_modules[0].project = "forebay/basekit";
    two_modules[0].module = "contracts";
    two_modules[0].block = block_with_coordinate("forebay:basekit:5.0.0:contracts");
    two_modules[1].project = "forebay/basekit";
    two_modules[1].module = "ir";
    two_modules[1].block = block_with_coordinate("forebay:basekit:5.0.0:ir");
}

static void teardown(void) {
    cJSON_Delete(two_modules[0].block);
    cJSON_Delete(two_modules[1].block);
}

static int apply(const fr_consumer *consumer, const fr_resolved *resolved, size_t count,
                 const char *original, char **out_text, fr_error *err) {
    return FR_LANGUAGE_GRADLE.apply(FR_LANGUAGE_GRADLE.state, consumer, resolved, count,
                                    original, out_text, err);
}

TEST writes_one_line_per_module_in_order(void) {
    setup();
    fr_consumer consumer = {0};
    consumer.configuration = "githubImplementation";
    char *text = NULL; fr_error err;
    ASSERT_EQ(FR_OK, apply(&consumer, two_modules, 2, TEMPLATE, &text, &err));
    ASSERT_STR_EQ(
        "dependencies {\n"
        "    // ferrule:begin\n"
        "    githubImplementation \"forebay:basekit:5.0.0:contracts\"\n"
        "    githubImplementation \"forebay:basekit:5.0.0:ir\"\n"
        "    // ferrule:end\n"
        "    testImplementation \"junit\"\n"
        "}\n", text);
    free(text);
    teardown();
    PASS();
}

TEST empties_the_region_for_no_modules(void) {
    fr_consumer consumer = {0};
    consumer.configuration = "githubImplementation";
    char *text = NULL; fr_error err;
    ASSERT_EQ(FR_OK, apply(&consumer, NULL, 0, TEMPLATE, &text, &err));
    ASSERT_STR_EQ(TEMPLATE, text);
    free(text);
    PASS();
}

TEST fails_without_a_configuration(void) {
    fr_consumer consumer = {0};
    char *text = NULL; fr_error err;
    ASSERT_EQ(FR_ERR, apply(&consumer, NULL, 0, TEMPLATE, &text, &err));
    ASSERT(strstr(err.message, "configuration") != NULL);
    PASS();
}

TEST names_the_module_whose_block_has_no_coordinate(void) {
    fr_resolved bare;
    bare.project = "forebay/basekit";
    bare.module = "loader";
    bare.block = cJSON_CreateObject();
    fr_consumer consumer = {0};
    consumer.configuration = "githubImplementation";
    char *text = NULL; fr_error err;
    ASSERT_EQ(FR_ERR, apply(&consumer, &bare, 1, TEMPLATE, &text, &err));
    ASSERT(strstr(err.message, "modules.loader.gradle") != NULL);
    cJSON_Delete(bare.block);
    PASS();
}

TEST fails_when_the_target_carries_no_region(void) {
    setup();
    fr_consumer consumer = {0};
    consumer.configuration = "githubImplementation";
    char *text = NULL; fr_error err;
    ASSERT_EQ(FR_ERR, apply(&consumer, two_modules, 2, "dependencies {\n}\n", &text, &err));
    ASSERT(strstr(err.message, "// ferrule:begin") != NULL);
    teardown();
    PASS();
}

TEST carries_the_expected_capability(void) {
    ASSERT_STR_EQ("ferrule.language/gradle", FR_LANGUAGE_GRADLE.capability);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(writes_one_line_per_module_in_order);
    RUN_TEST(empties_the_region_for_no_modules);
    RUN_TEST(fails_without_a_configuration);
    RUN_TEST(names_the_module_whose_block_has_no_coordinate);
    RUN_TEST(fails_when_the_target_carries_no_region);
    RUN_TEST(carries_the_expected_capability);
    GREATEST_MAIN_END();
}
