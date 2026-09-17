#include "greatest.h"
#include "registry.h"

#include "cJSON.h"

#include <string.h>

static int fake_load(void *state, const char *project, const cJSON *block,
                     const char *base_dir, fr_project *out, fr_error *err) {
    (void) project; (void) block; (void) base_dir; (void) out; (void) err;
    *(int *) state = 1;
    return FR_OK;
}

TEST returns_a_plugin_registered_under_its_capability(void) {
    fr_registry *registry = fr_registry_create();
    int called = 0;
    fr_source_plugin plugin = { "terko.source/test", fake_load, &called };
    fr_error err;
    ASSERT_EQ(FR_OK, fr_registry_add_source(registry, &plugin, &err));

    const fr_source_plugin *found = fr_registry_source(registry, "terko.source/test");
    ASSERT(found != NULL);
    fr_project project;
    ASSERT_EQ(FR_OK, found->load(found->state, "x/y", NULL, ".", &project, &err));
    ASSERT_EQ(1, called);
    fr_registry_destroy(registry);
    PASS();
}

TEST returns_null_for_an_unregistered_capability(void) {
    fr_registry *registry = fr_registry_create();
    ASSERT(fr_registry_source(registry, "terko.source/absent") == NULL);
    fr_registry_destroy(registry);
    PASS();
}

TEST rejects_a_duplicate_capability(void) {
    fr_registry *registry = fr_registry_create();
    int called = 0;
    fr_source_plugin plugin = { "terko.source/test", fake_load, &called };
    fr_error err;
    fr_registry_add_source(registry, &plugin, &err);
    ASSERT_EQ(FR_ERR, fr_registry_add_source(registry, &plugin, &err));
    ASSERT(strstr(err.message, "terko.source/test") != NULL);
    fr_registry_destroy(registry);
    PASS();
}

TEST keeps_source_and_language_spaces_separate(void) {
    fr_registry *registry = fr_registry_create();
    int called = 0;
    fr_source_plugin plugin = { "terko/x", fake_load, &called };
    fr_error err;
    fr_registry_add_source(registry, &plugin, &err);
    ASSERT(fr_registry_language(registry, "terko/x") == NULL);
    fr_registry_destroy(registry);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(returns_a_plugin_registered_under_its_capability);
    RUN_TEST(returns_null_for_an_unregistered_capability);
    RUN_TEST(rejects_a_duplicate_capability);
    RUN_TEST(keeps_source_and_language_spaces_separate);
    GREATEST_MAIN_END();
}
