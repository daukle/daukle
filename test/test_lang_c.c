#include "greatest.h"
#include "lang_c.h"

#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

static const char *TEMPLATE =
    "add_executable(demo main.c)\n"
    "# terko:begin\n"
    "# terko:end\n";

static cJSON *c_block(const char *package, const char *url, const char *sha256) {
    cJSON *block = cJSON_CreateObject();
    cJSON_AddStringToObject(block, "package", package);
    if (url != NULL) cJSON_AddStringToObject(block, "url", url);
    if (sha256 != NULL) cJSON_AddStringToObject(block, "sha256", sha256);
    return block;
}

static fr_resolved resolved_module(const char *module, cJSON *block) {
    fr_resolved entry;
    entry.project = "forebay/basekit";
    entry.module = (char *) module;
    entry.block = block;
    return entry;
}

static int apply(const char *configuration, const fr_resolved *resolved, size_t count,
                 const char *original, char **out_text, fr_error *err) {
    fr_consumer consumer = {0};
    consumer.configuration = (char *) configuration;
    return FR_LANGUAGE_C.apply(FR_LANGUAGE_C.state, &consumer, resolved, count,
                               original, out_text, err);
}

TEST declares_and_links_one_module(void) {
    cJSON *block = c_block("basekit-ir", "https://example.test/ir.tar.gz", NULL);
    fr_resolved one = resolved_module("ir", block);
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_OK, apply("demo", &one, 1, TEMPLATE, &out, &err));
    ASSERT_STR_EQ(
        "add_executable(demo main.c)\n"
        "# terko:begin\n"
        "include(FetchContent)\n"
        "FetchContent_Declare(basekit-ir URL \"https://example.test/ir.tar.gz\")\n"
        "FetchContent_MakeAvailable(basekit-ir)\n"
        "target_link_libraries(demo PRIVATE basekit-ir)\n"
        "# terko:end\n", out);
    free(out);
    cJSON_Delete(block);
    PASS();
}

TEST emits_a_hash_only_when_the_block_carries_one(void) {
    cJSON *first = c_block("basekit-contracts", "https://example.test/contracts.tar.gz", NULL);
    cJSON *second = c_block("basekit-ir", "https://example.test/ir.tar.gz", "abc123");
    fr_resolved modules[2];
    modules[0] = resolved_module("contracts", first);
    modules[1] = resolved_module("ir", second);
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_OK, apply("demo", modules, 2, TEMPLATE, &out, &err));
    ASSERT_STR_EQ(
        "add_executable(demo main.c)\n"
        "# terko:begin\n"
        "include(FetchContent)\n"
        "FetchContent_Declare(basekit-contracts URL \"https://example.test/contracts.tar.gz\")\n"
        "FetchContent_MakeAvailable(basekit-contracts)\n"
        "FetchContent_Declare(basekit-ir URL \"https://example.test/ir.tar.gz\" URL_HASH SHA256=abc123)\n"
        "FetchContent_MakeAvailable(basekit-ir)\n"
        "target_link_libraries(demo PRIVATE basekit-contracts basekit-ir)\n"
        "# terko:end\n", out);
    free(out);
    cJSON_Delete(first);
    cJSON_Delete(second);
    PASS();
}

TEST links_every_package_once_in_resolution_order(void) {
    cJSON *first = c_block("basekit-contracts", "https://example.test/contracts.tar.gz", NULL);
    cJSON *second = c_block("basekit-ir", "https://example.test/ir.tar.gz", NULL);
    fr_resolved modules[2];
    modules[0] = resolved_module("contracts", first);
    modules[1] = resolved_module("ir", second);
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_OK, apply("demo", modules, 2, TEMPLATE, &out, &err));
    const char *link = strstr(out, "target_link_libraries");
    ASSERT(link != NULL);
    ASSERT_STR_EQ("target_link_libraries(demo PRIVATE basekit-contracts basekit-ir)\n"
                  "# terko:end\n", link);
    free(out);
    cJSON_Delete(first);
    cJSON_Delete(second);
    PASS();
}

TEST empties_the_region_for_no_modules(void) {
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_OK, apply("demo", NULL, 0, TEMPLATE, &out, &err));
    ASSERT_STR_EQ(TEMPLATE, out);
    free(out);
    PASS();
}

TEST applying_twice_equals_applying_once(void) {
    cJSON *block = c_block("basekit-ir", "https://example.test/ir.tar.gz", NULL);
    fr_resolved one = resolved_module("ir", block);
    char *once = NULL; char *twice = NULL; fr_error err;
    ASSERT_EQ(FR_OK, apply("demo", &one, 1, TEMPLATE, &once, &err));
    ASSERT_EQ(FR_OK, apply("demo", &one, 1, once, &twice, &err));
    ASSERT_STR_EQ(once, twice);
    free(once);
    free(twice);
    cJSON_Delete(block);
    PASS();
}

TEST names_the_module_whose_block_has_no_url(void) {
    cJSON *block = c_block("basekit-ir", NULL, NULL);
    fr_resolved one = resolved_module("ir", block);
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_ERR, apply("demo", &one, 1, TEMPLATE, &out, &err));
    ASSERT(strstr(err.message, "modules.ir.c") != NULL);
    ASSERT(strstr(err.message, "url") != NULL);
    cJSON_Delete(block);
    PASS();
}

TEST fails_without_a_configuration(void) {
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_ERR, apply("", NULL, 0, TEMPLATE, &out, &err));
    ASSERT(strstr(err.message, "configuration") != NULL);
    PASS();
}

TEST fails_when_the_target_carries_no_region(void) {
    cJSON *block = c_block("basekit-ir", "https://example.test/ir.tar.gz", NULL);
    fr_resolved one = resolved_module("ir", block);
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_ERR, apply("demo", &one, 1, "add_executable(demo main.c)\n", &out, &err));
    ASSERT(strstr(err.message, "# terko:begin") != NULL);
    cJSON_Delete(block);
    PASS();
}

TEST carries_the_expected_capability(void) {
    ASSERT_STR_EQ("terko.language/c", FR_LANGUAGE_C.capability);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(declares_and_links_one_module);
    RUN_TEST(emits_a_hash_only_when_the_block_carries_one);
    RUN_TEST(links_every_package_once_in_resolution_order);
    RUN_TEST(empties_the_region_for_no_modules);
    RUN_TEST(applying_twice_equals_applying_once);
    RUN_TEST(names_the_module_whose_block_has_no_url);
    RUN_TEST(fails_without_a_configuration);
    RUN_TEST(fails_when_the_target_carries_no_region);
    RUN_TEST(carries_the_expected_capability);
    GREATEST_MAIN_END();
}
