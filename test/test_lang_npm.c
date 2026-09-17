#include "greatest.h"
#include "lang_npm.h"

#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

static const char *PACKAGE =
    "{\n"
    "  \"name\": \"stub-translator\",\n"
    "  \"dependencies\": {\n"
    "    \"@openauthjs/openauth\": \"^0.4.3\"\n"
    "  }\n"
    "}\n";

static const char *MANAGED_PACKAGE =
    "{\n"
    "  \"name\": \"stub-translator\",\n"
    "  \"dependencies\": {\n"
    "    \"@openauthjs/openauth\": \"^0.4.3\",\n"
    "    \"@intisy-ai/basekit-ir\": \"^4.0.0\",\n"
    "    \"@intisy/bayonet\": \"^1.7.0\"\n"
    "  },\n"
    "  \"terko\": {\n"
    "    \"managed\": {\n"
    "      \"dependencies\": [\n"
    "        \"@intisy-ai/basekit-ir\",\n"
    "        \"@intisy/bayonet\"\n"
    "      ]\n"
    "    }\n"
    "  }\n"
    "}\n";

static cJSON *npm_block(const char *package, const char *range) {
    cJSON *block = cJSON_CreateObject();
    cJSON_AddStringToObject(block, "package", package);
    cJSON_AddStringToObject(block, "range", range);
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
    return FR_LANGUAGE_NPM.apply(FR_LANGUAGE_NPM.state, &consumer, resolved, count,
                                 original, out_text, err);
}

TEST writes_a_dependency_and_the_ledger_that_owns_it(void) {
    cJSON *block = npm_block("@intisy-ai/basekit-ir", "^5.0.0");
    fr_resolved one = resolved_module("ir", block);
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_OK, apply("dependencies", &one, 1, PACKAGE, &out, &err));
    ASSERT_STR_EQ(
        "{\n"
        "  \"name\": \"stub-translator\",\n"
        "  \"dependencies\": {\n"
        "    \"@openauthjs/openauth\": \"^0.4.3\",\n"
        "    \"@intisy-ai/basekit-ir\": \"^5.0.0\"\n"
        "  },\n"
        "  \"terko\": {\n"
        "    \"managed\": {\n"
        "      \"dependencies\": [\n"
        "        \"@intisy-ai/basekit-ir\"\n"
        "      ]\n"
        "    }\n"
        "  }\n"
        "}\n", out);
    free(out);
    cJSON_Delete(block);
    PASS();
}

TEST updates_a_range_in_place_and_removes_what_left_the_manifest(void) {
    cJSON *block = npm_block("@intisy-ai/basekit-ir", "^5.0.0");
    fr_resolved one = resolved_module("ir", block);
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_OK, apply("dependencies", &one, 1, MANAGED_PACKAGE, &out, &err));
    ASSERT_STR_EQ(
        "{\n"
        "  \"name\": \"stub-translator\",\n"
        "  \"dependencies\": {\n"
        "    \"@openauthjs/openauth\": \"^0.4.3\",\n"
        "    \"@intisy-ai/basekit-ir\": \"^5.0.0\"\n"
        "  },\n"
        "  \"terko\": {\n"
        "    \"managed\": {\n"
        "      \"dependencies\": [\n"
        "        \"@intisy-ai/basekit-ir\"\n"
        "      ]\n"
        "    }\n"
        "  }\n"
        "}\n", out);
    free(out);
    cJSON_Delete(block);
    PASS();
}

TEST leaves_a_dependency_it_never_owned_alone(void) {
    cJSON *block = npm_block("@intisy-ai/basekit-ir", "^5.0.0");
    fr_resolved one = resolved_module("ir", block);
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_OK, apply("dependencies", &one, 1, MANAGED_PACKAGE, &out, &err));
    ASSERT(strstr(out, "\"@openauthjs/openauth\": \"^0.4.3\"") != NULL);
    free(out);
    cJSON_Delete(block);
    PASS();
}

TEST writes_dev_dependencies_from_the_same_plugin(void) {
    const char *document =
        "{\n"
        "  \"devDependencies\": {\n"
        "    \"typescript\": \"^5.4.0\"\n"
        "  }\n"
        "}\n";
    cJSON *block = npm_block("@intisy-ai/basekit-ir", "^5.0.0");
    fr_resolved one = resolved_module("ir", block);
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_OK, apply("devDependencies", &one, 1, document, &out, &err));
    ASSERT_STR_EQ(
        "{\n"
        "  \"devDependencies\": {\n"
        "    \"typescript\": \"^5.4.0\",\n"
        "    \"@intisy-ai/basekit-ir\": \"^5.0.0\"\n"
        "  },\n"
        "  \"terko\": {\n"
        "    \"managed\": {\n"
        "      \"devDependencies\": [\n"
        "        \"@intisy-ai/basekit-ir\"\n"
        "      ]\n"
        "    }\n"
        "  }\n"
        "}\n", out);
    free(out);
    cJSON_Delete(block);
    PASS();
}

TEST applying_twice_equals_applying_once(void) {
    cJSON *block = npm_block("@intisy-ai/basekit-ir", "^5.0.0");
    fr_resolved one = resolved_module("ir", block);
    char *once = NULL; char *twice = NULL; fr_error err;
    ASSERT_EQ(FR_OK, apply("dependencies", &one, 1, PACKAGE, &once, &err));
    ASSERT_EQ(FR_OK, apply("dependencies", &one, 1, once, &twice, &err));
    ASSERT_STR_EQ(once, twice);
    free(once);
    free(twice);
    cJSON_Delete(block);
    PASS();
}

TEST two_consumers_on_one_document_reach_a_fixed_point(void) {
    const char *document =
        "{\n"
        "  \"dependencies\": {},\n"
        "  \"devDependencies\": {}\n"
        "}\n";
    cJSON *runtime_block = npm_block("@intisy-ai/basekit-ir", "^5.0.0");
    cJSON *tool_block = npm_block("@intisy/bayonet", "^1.8.0");
    fr_resolved runtime = resolved_module("ir", runtime_block);
    fr_resolved tool = resolved_module("loader", tool_block);
    fr_error err;

    char *first = NULL; char *second = NULL;
    ASSERT_EQ(FR_OK, apply("dependencies", &runtime, 1, document, &first, &err));
    ASSERT_EQ(FR_OK, apply("devDependencies", &tool, 1, first, &second, &err));

    char *third = NULL; char *fourth = NULL;
    ASSERT_EQ(FR_OK, apply("dependencies", &runtime, 1, second, &third, &err));
    ASSERT_EQ(FR_OK, apply("devDependencies", &tool, 1, third, &fourth, &err));
    ASSERT_STR_EQ(second, fourth);

    ASSERT(strstr(fourth, "\"@intisy-ai/basekit-ir\"") != NULL);
    ASSERT(strstr(fourth, "\"@intisy/bayonet\"") != NULL);

    free(first); free(second); free(third); free(fourth);
    cJSON_Delete(runtime_block);
    cJSON_Delete(tool_block);
    PASS();
}

TEST reversing_the_consumer_order_reaches_the_same_document(void) {
    const char *document =
        "{\n"
        "  \"dependencies\": {},\n"
        "  \"devDependencies\": {}\n"
        "}\n";
    cJSON *runtime_block = npm_block("@intisy-ai/basekit-ir", "^5.0.0");
    cJSON *tool_block = npm_block("@intisy/bayonet", "^1.8.0");
    fr_resolved runtime = resolved_module("ir", runtime_block);
    fr_resolved tool = resolved_module("loader", tool_block);
    fr_error err;

    char *a1 = NULL; char *a2 = NULL; char *a3 = NULL;
    ASSERT_EQ(FR_OK, apply("devDependencies", &tool, 1, document, &a1, &err));
    ASSERT_EQ(FR_OK, apply("dependencies", &runtime, 1, a1, &a2, &err));
    ASSERT_EQ(FR_OK, apply("devDependencies", &tool, 1, a2, &a3, &err));
    ASSERT_STR_EQ(a2, a3);

    ASSERT(strstr(a3, "\"@intisy-ai/basekit-ir\": \"^5.0.0\"") != NULL);
    ASSERT(strstr(a3, "\"@intisy/bayonet\": \"^1.8.0\"") != NULL);

    free(a1); free(a2); free(a3);
    cJSON_Delete(runtime_block);
    cJSON_Delete(tool_block);
    PASS();
}

TEST leaves_a_document_alone_when_it_owns_nothing(void) {
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_OK, apply("dependencies", NULL, 0, PACKAGE, &out, &err));
    ASSERT_STR_EQ(PACKAGE, out);
    free(out);
    PASS();
}

TEST names_the_module_whose_block_has_no_package(void) {
    cJSON *block = cJSON_CreateObject();
    fr_resolved bare = resolved_module("loader", block);
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_ERR, apply("dependencies", &bare, 1, PACKAGE, &out, &err));
    ASSERT(strstr(err.message, "modules.loader.npm") != NULL);
    cJSON_Delete(block);
    PASS();
}

TEST fails_without_a_configuration(void) {
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_ERR, apply("", NULL, 0, PACKAGE, &out, &err));
    ASSERT(strstr(err.message, "configuration") != NULL);
    PASS();
}

TEST rejects_a_configuration_that_names_a_path(void) {
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_ERR, apply("terko.managed", NULL, 0, PACKAGE, &out, &err));
    ASSERT(strstr(err.message, "not a path") != NULL);
    PASS();
}

TEST reports_a_document_that_is_not_json(void) {
    cJSON *block = npm_block("@intisy-ai/basekit-ir", "^5.0.0");
    fr_resolved one = resolved_module("ir", block);
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_ERR, apply("dependencies", &one, 1, "{ not json", &out, &err));
    ASSERT(strstr(err.message, "json") != NULL);
    cJSON_Delete(block);
    PASS();
}

TEST carries_the_expected_capability(void) {
    ASSERT_STR_EQ("terko.language/npm", FR_LANGUAGE_NPM.capability);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(writes_a_dependency_and_the_ledger_that_owns_it);
    RUN_TEST(updates_a_range_in_place_and_removes_what_left_the_manifest);
    RUN_TEST(leaves_a_dependency_it_never_owned_alone);
    RUN_TEST(writes_dev_dependencies_from_the_same_plugin);
    RUN_TEST(applying_twice_equals_applying_once);
    RUN_TEST(two_consumers_on_one_document_reach_a_fixed_point);
    RUN_TEST(reversing_the_consumer_order_reaches_the_same_document);
    RUN_TEST(leaves_a_document_alone_when_it_owns_nothing);
    RUN_TEST(names_the_module_whose_block_has_no_package);
    RUN_TEST(fails_without_a_configuration);
    RUN_TEST(rejects_a_configuration_that_names_a_path);
    RUN_TEST(reports_a_document_that_is_not_json);
    RUN_TEST(carries_the_expected_capability);
    GREATEST_MAIN_END();
}
