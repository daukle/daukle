#include "greatest.h"
#include "config.h"
#include "config_toml.h"
#include "manifest.h"
#include "registry.h"
#include "sync.h"

#include <stdlib.h>
#include <string.h>

static int never_loads(void *state, const char *text, const char *origin, const char *base_dir,
                        fr_registry *registry, const struct cJSON *document,
                        struct cJSON **out, fr_error *err) {
    (void) state; (void) text; (void) origin; (void) base_dir;
    (void) registry; (void) document; (void) out; (void) err;
    return FR_ERR;
}

TEST loads_a_toml_manifest_through_the_seam(void) {
    fr_error err;
    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));
    fr_manifest manifest;
    ASSERT_EQ(FR_OK, fr_config_load_file("test/fixtures/consumer/daukle.toml", registry, &manifest, &err));
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
    ASSERT(strstr(err.message, "reads \"xyz\"") != NULL);
    ASSERT(strstr(err.message, "daukle.toml") != NULL);
    fr_registry_destroy(registry);
    PASS();
}

/* The refusal has to land before the file is opened, so a user upgrading from a
   daukle that read json meets the format, not a missing-file error. */
TEST rejects_a_json_manifest_naming_the_formats_it_reads(void) {
    fr_error err;
    fr_registry *registry = NULL;
    fr_build_registry(&registry, &err);
    fr_manifest manifest;
    ASSERT_EQ(FR_ERR, fr_config_load_file("test/fixtures/consumer/daukle.json", registry, &manifest, &err));
    ASSERT(strstr(err.message, "reads \"json\"") != NULL);
    ASSERT(strstr(err.message, "daukle.toml") != NULL);
    ASSERT(strstr(err.message, "daukle.lua") != NULL);
    fr_registry_destroy(registry);
    PASS();
}

TEST finds_the_only_manifest_in_a_directory(void) {
    fr_error err;
    fr_registry *registry = NULL;
    fr_build_registry(&registry, &err);
    char *found = NULL;
    ASSERT_EQ(FR_OK, fr_config_find("test/fixtures/search/toml-only", registry, &found, &err));
    ASSERT(strstr(found, "daukle.toml") != NULL);
    free(found);
    fr_registry_destroy(registry);
    PASS();
}

/* Once json is gone the built registry holds exactly one primary format (toml)
   and one overlay (lua), so two primaries can never collide there. The error
   this proves still needs a live path, so this test builds its own registry
   with a second, throwaway primary instead of relying on fr_build_registry. */
TEST refuses_two_manifests_in_one_directory(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_registry_add_config(registry, &FR_CONFIG_TOML, &err));
    static const fr_config_plugin second_primary = {
        "test.config/second-primary", "daukle.second", 0, never_loads, NULL
    };
    ASSERT_EQ(FR_OK, fr_registry_add_config(registry, &second_primary, &err));

    char *found = NULL;
    ASSERT_EQ(FR_ERR, fr_config_find("test/fixtures/search/both", registry, &found, &err));
    ASSERT(strstr(err.message, "daukle.toml") != NULL);
    ASSERT(strstr(err.message, "daukle.second") != NULL);
    fr_registry_destroy(registry);
    PASS();
}

TEST reports_a_directory_with_no_manifest(void) {
    fr_error err;
    fr_registry *registry = NULL;
    fr_build_registry(&registry, &err);
    char *found = NULL;
    ASSERT_EQ(FR_ERR, fr_config_find("test/fixtures/search/empty", registry, &found, &err));
    ASSERT(strstr(err.message, "daukle.toml") != NULL);
    ASSERT(strstr(err.message, "daukle.lua") != NULL);
    ASSERT(strstr(err.message, "daukle.json") == NULL);
    fr_registry_destroy(registry);
    PASS();
}

TEST names_where_a_toml_manifest_stops_parsing(void) {
    fr_error err;
    fr_registry *registry = NULL;
    fr_build_registry(&registry, &err);
    fr_manifest manifest;
    ASSERT_EQ(FR_ERR, fr_config_load_file("test/fixtures/toml-broken/daukle.toml",
                                          registry, &manifest, &err));
    ASSERT(strstr(err.message, "toml-broken/daukle.toml") != NULL);
    ASSERT(strstr(err.message, "line 3") != NULL);
    fr_registry_destroy(registry);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(loads_a_toml_manifest_through_the_seam);
    RUN_TEST(rejects_a_file_no_format_claims);
    RUN_TEST(rejects_a_json_manifest_naming_the_formats_it_reads);
    RUN_TEST(finds_the_only_manifest_in_a_directory);
    RUN_TEST(refuses_two_manifests_in_one_directory);
    RUN_TEST(reports_a_directory_with_no_manifest);
    RUN_TEST(names_where_a_toml_manifest_stops_parsing);
    GREATEST_MAIN_END();
}
