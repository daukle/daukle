#include "greatest.h"
#include "config_toml.h"
#include "manifest.h"
#include "region.h"

#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

/* The two seams fr_config_load_file composes, without its registry: the toml
   plugin turns a file into a document and the manifest layer validates it.
   A registry would also load the fixture's [plugins] table, which these tests
   are not about. */
static int read_fixture(const char *file_path, fr_manifest *out, fr_error *err) {
    memset(out, 0, sizeof *out);
    char *text = NULL;
    if (fr_file_read_text(file_path, &text, err) != FR_OK) return FR_ERR;
    cJSON *document = NULL;
    int parsed = FR_CONFIG_TOML.load(FR_CONFIG_TOML.state, text, file_path, ".", NULL, NULL,
                                     &document, err);
    free(text);
    if (parsed != FR_OK) return FR_ERR;
    return fr_manifest_from_document(document, file_path, out, err);
}

TEST reads_a_producer(void) {
    fr_manifest manifest; fr_error err;
    ASSERT_EQ(FR_OK, read_fixture("test/fixtures/producer/daukle.toml", &manifest, &err));
    ASSERT_STR_EQ("forebay/basekit", manifest.self.project);
    ASSERT_EQ(5, manifest.self.version.major);
    ASSERT_EQ(3, (int) manifest.self.module_count);
    fr_manifest_free(&manifest);
    PASS();
}

TEST finds_a_module_and_its_requires(void) {
    fr_manifest manifest; fr_error err;
    read_fixture("test/fixtures/producer/daukle.toml", &manifest, &err);
    const fr_module *ir = fr_project_module(&manifest.self, "ir");
    ASSERT(ir != NULL);
    ASSERT_EQ(1, (int) ir->requires_count);
    ASSERT_STR_EQ("contracts", ir->requires[0]);
    const cJSON *gradle = cJSON_GetObjectItemCaseSensitive(ir->blocks, "gradle");
    ASSERT(cJSON_IsObject(gradle));
    const cJSON *coordinate = cJSON_GetObjectItemCaseSensitive(gradle, "coordinate");
    ASSERT_STR_EQ("forebay:basekit:5.0.0:ir", coordinate->valuestring);
    fr_manifest_free(&manifest);
    PASS();
}

TEST leaves_an_absent_language_block_null(void) {
    fr_manifest manifest; fr_error err;
    read_fixture("test/fixtures/producer/daukle.toml", &manifest, &err);
    const fr_module *loader = fr_project_module(&manifest.self, "loader");
    ASSERT(loader != NULL);
    ASSERT(cJSON_GetObjectItemCaseSensitive(loader->blocks, "gradle") == NULL);
    fr_manifest_free(&manifest);
    PASS();
}

TEST returns_null_for_an_unknown_module(void) {
    fr_manifest manifest; fr_error err;
    read_fixture("test/fixtures/producer/daukle.toml", &manifest, &err);
    ASSERT(fr_project_module(&manifest.self, "nope") == NULL);
    fr_manifest_free(&manifest);
    PASS();
}

TEST reads_a_consumer(void) {
    fr_manifest manifest; fr_error err;
    ASSERT_EQ(FR_OK, read_fixture("test/fixtures/consumer/daukle.toml", &manifest, &err));
    ASSERT_EQ(1, (int) manifest.source_count);
    ASSERT_STR_EQ("forebay/basekit", manifest.sources[0].project);
    ASSERT_STR_EQ("path", manifest.sources[0].kind);
    const cJSON *path_field = cJSON_GetObjectItemCaseSensitive(manifest.sources[0].block, "path");
    ASSERT(cJSON_IsString(path_field));
    ASSERT_STR_EQ("./producer", path_field->valuestring);
    ASSERT_EQ(1, (int) manifest.consumer_count);
    ASSERT_STR_EQ("stub", manifest.consumers[0].id);
    ASSERT_STR_EQ("gradle", manifest.consumers[0].language);
    ASSERT_STR_EQ("githubImplementation", manifest.consumers[0].configuration);
    ASSERT_EQ(1, (int) manifest.consumers[0].dependency_count);
    ASSERT_EQ(FR_RANGE_CARET, manifest.consumers[0].dependencies[0].range.kind);
    ASSERT_STR_EQ("ir", manifest.consumers[0].dependencies[0].modules[0]);
    fr_manifest_free(&manifest);
    PASS();
}

TEST keeps_a_source_block_of_any_kind(void) {
    fr_manifest manifest;
    fr_error err;
    ASSERT_EQ(FR_OK, read_fixture("test/fixtures/consumer/daukle-unknown-source.toml", &manifest, &err));

    const fr_source *source = fr_manifest_source(&manifest, "forebay/basekit");
    ASSERT(source != NULL);
    ASSERT_STR_EQ("some-future-kind", source->kind);
    ASSERT(source->block != NULL);

    const cJSON *field = cJSON_GetObjectItemCaseSensitive(source->block, "whatever");
    ASSERT(cJSON_IsString(field));
    ASSERT_STR_EQ("value", field->valuestring);

    fr_manifest_free(&manifest);
    PASS();
}

/* A language block is looked up by name in the module entry, which also holds
   the keys the core reads. A consumer naming its language after one of those
   would otherwise resolve to the core's own value. */
TEST rejects_a_consumer_language_that_names_a_reserved_module_key(void) {
    fr_manifest manifest; fr_error err;
    ASSERT_EQ(FR_ERR, read_fixture("test/fixtures/consumer-reserved-language/daukle.toml",
                                   &manifest, &err));
    ASSERT(strstr(err.message, "reserved") != NULL);
    ASSERT(strstr(err.message, "requires") != NULL);
    PASS();
}

TEST rejects_an_unknown_schema(void) {
    fr_manifest manifest; fr_error err;
    ASSERT_EQ(FR_ERR, read_fixture("test/fixtures/bad-schema/daukle.toml", &manifest, &err));
    ASSERT(strstr(err.message, "schema") != NULL);
    PASS();
}

TEST validates_a_document_built_in_memory(void) {
    cJSON *root = cJSON_Parse(
        "{\"schema\":1,\"project\":\"forebay/x\",\"version\":\"1.0.0\","
        "\"modules\":{},\"sources\":{},\"consumers\":[]}");
    ASSERT(root != NULL);
    fr_manifest manifest; fr_error err;
    ASSERT_EQ(FR_OK, fr_manifest_from_document(root, "<memory>", &manifest, &err));
    ASSERT_STR_EQ("forebay/x", manifest.self.project);
    fr_manifest_free(&manifest);
    PASS();
}

TEST reports_the_origin_of_a_bad_document(void) {
    cJSON *root = cJSON_Parse("{\"schema\":99}");
    ASSERT(root != NULL);
    fr_manifest manifest; fr_error err;
    ASSERT_EQ(FR_ERR, fr_manifest_from_document(root, "<memory>", &manifest, &err));
    ASSERT(strstr(err.message, "<memory>") != NULL);
    PASS();
}

TEST a_toolchain_block_is_read_with_its_own_keys(void) {
    fr_manifest manifest; fr_error err;
    ASSERT_EQ(FR_OK, read_fixture("test/fixtures/toolchain/daukle.toml", &manifest, &err));
    ASSERT_EQ(2, (int) manifest.toolchain_count);

    const fr_toolchain *stub = NULL;
    for (size_t index = 0; index < manifest.toolchain_count; index++) {
        if (strcmp(manifest.toolchains[index].name, "stub") == 0) stub = &manifest.toolchains[index];
    }
    int found = stub != NULL;
    int version_is_read = found && strcmp(stub->version, "21") == 0;
    const cJSON *target = found ? cJSON_GetObjectItemCaseSensitive(stub->block, "target") : NULL;
    int target_is_kept = target != NULL && cJSON_IsString(target)
                         && strcmp(target->valuestring, "app") == 0;
    fr_manifest_free(&manifest);

    ASSERT(found);
    ASSERT(version_is_read);
    ASSERT(target_is_kept);
    PASS();
}

TEST a_toolchain_written_as_a_string_is_a_version_constraint(void) {
    fr_manifest manifest; fr_error err;
    ASSERT_EQ(FR_OK, read_fixture("test/fixtures/toolchain/daukle.toml", &manifest, &err));

    const fr_toolchain *shorthand = NULL;
    for (size_t index = 0; index < manifest.toolchain_count; index++) {
        if (strcmp(manifest.toolchains[index].name, "shorthand") == 0) {
            shorthand = &manifest.toolchains[index];
        }
    }
    int found = shorthand != NULL;
    int version_is_read = found && strcmp(shorthand->version, ">=3.20") == 0;
    fr_manifest_free(&manifest);

    ASSERT(found);
    ASSERT(version_is_read);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(reads_a_producer);
    RUN_TEST(finds_a_module_and_its_requires);
    RUN_TEST(leaves_an_absent_language_block_null);
    RUN_TEST(returns_null_for_an_unknown_module);
    RUN_TEST(reads_a_consumer);
    RUN_TEST(keeps_a_source_block_of_any_kind);
    RUN_TEST(rejects_a_consumer_language_that_names_a_reserved_module_key);
    RUN_TEST(rejects_an_unknown_schema);
    RUN_TEST(validates_a_document_built_in_memory);
    RUN_TEST(reports_the_origin_of_a_bad_document);
    RUN_TEST(a_toolchain_block_is_read_with_its_own_keys);
    RUN_TEST(a_toolchain_written_as_a_string_is_a_version_constraint);
    GREATEST_MAIN_END();
}
