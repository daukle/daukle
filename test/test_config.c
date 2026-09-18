#include "greatest.h"
#include "config.h"
#include "manifest.h"
#include "registry.h"
#include "sync.h"

#include <stdlib.h>
#include <string.h>

TEST loads_a_json_manifest_through_the_seam(void) {
    fr_error err;
    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));
    fr_manifest manifest;
    ASSERT_EQ(FR_OK, fr_config_load_file("test/fixtures/consumer/daukle.json", registry, &manifest, &err));
    ASSERT_STR_EQ("forebay/stub-translator", manifest.self.project);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    PASS();
}

TEST rejects_a_file_no_format_claims(void) {
    fr_error err;
    fr_registry *registry = NULL;
    fr_build_registry(&registry, &err);
    fr_manifest manifest;
    ASSERT_EQ(FR_ERR, fr_config_load_file("test/fixtures/consumer/daukle.xyz", registry, &manifest, &err));
    ASSERT(strstr(err.message, "xyz") != NULL);
    fr_registry_destroy(registry);
    PASS();
}

TEST finds_the_only_manifest_in_a_directory(void) {
    fr_error err;
    fr_registry *registry = NULL;
    fr_build_registry(&registry, &err);
    char *found = NULL;
    ASSERT_EQ(FR_OK, fr_config_find("test/fixtures/search/json-only", registry, &found, &err));
    ASSERT(strstr(found, "daukle.json") != NULL);
    free(found);
    fr_registry_destroy(registry);
    PASS();
}

TEST refuses_two_manifests_in_one_directory(void) {
    fr_error err;
    fr_registry *registry = NULL;
    fr_build_registry(&registry, &err);
    char *found = NULL;
    ASSERT_EQ(FR_ERR, fr_config_find("test/fixtures/search/both", registry, &found, &err));
    ASSERT(strstr(err.message, "daukle.json") != NULL);
    fr_registry_destroy(registry);
    PASS();
}

TEST reports_a_directory_with_no_manifest(void) {
    fr_error err;
    fr_registry *registry = NULL;
    fr_build_registry(&registry, &err);
    char *found = NULL;
    ASSERT_EQ(FR_ERR, fr_config_find("test/fixtures/search/empty", registry, &found, &err));
    ASSERT(strstr(err.message, "daukle.json") != NULL);
    ASSERT(strstr(err.message, "daukle.toml") != NULL);
    fr_registry_destroy(registry);
    PASS();
}

TEST names_where_a_json_manifest_stops_parsing(void) {
    fr_error err;
    fr_registry *registry = NULL;
    fr_build_registry(&registry, &err);
    fr_manifest manifest;
    ASSERT_EQ(FR_ERR, fr_config_load_file("test/fixtures/json-broken/daukle.json",
                                          registry, &manifest, &err));
    ASSERT(strstr(err.message, "byte") != NULL);
    ASSERT(strstr(err.message, "version") != NULL);
    fr_registry_destroy(registry);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(loads_a_json_manifest_through_the_seam);
    RUN_TEST(rejects_a_file_no_format_claims);
    RUN_TEST(finds_the_only_manifest_in_a_directory);
    RUN_TEST(refuses_two_manifests_in_one_directory);
    RUN_TEST(reports_a_directory_with_no_manifest);
    RUN_TEST(names_where_a_json_manifest_stops_parsing);
    GREATEST_MAIN_END();
}
