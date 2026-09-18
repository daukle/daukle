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

static int never_loads(void *state, const char *text, const char *origin, const char *base_dir,
                       fr_registry *registry, const struct cJSON *document,
                       struct cJSON **out, fr_error *err) {
    (void) state; (void) text; (void) origin; (void) base_dir;
    (void) registry; (void) document; (void) out; (void) err;
    return FR_ERR;
}

static const fr_config_plugin PRIMARY = { "daukle.config/aaa", "daukle.aaa", 0, never_loads, NULL };
static const fr_config_plugin OVERLAY = { "daukle.config/bbb", "daukle.bbb", 1, never_loads, NULL };

TEST returns_a_plugin_registered_under_its_capability(void) {
    fr_registry *registry = fr_registry_create();
    int called = 0;
    fr_source_plugin plugin = { "daukle.source/test", fake_load, &called };
    fr_error err;
    ASSERT_EQ(FR_OK, fr_registry_add_source(registry, &plugin, &err));

    const fr_source_plugin *found = fr_registry_source(registry, "daukle.source/test");
    ASSERT(found != NULL);
    fr_project project;
    ASSERT_EQ(FR_OK, found->load(found->state, "x/y", NULL, ".", &project, &err));
    ASSERT_EQ(1, called);
    fr_registry_destroy(registry);
    PASS();
}

TEST returns_null_for_an_unregistered_capability(void) {
    fr_registry *registry = fr_registry_create();
    ASSERT(fr_registry_source(registry, "daukle.source/absent") == NULL);
    fr_registry_destroy(registry);
    PASS();
}

TEST rejects_a_duplicate_capability(void) {
    fr_registry *registry = fr_registry_create();
    int called = 0;
    fr_source_plugin plugin = { "daukle.source/test", fake_load, &called };
    fr_error err;
    fr_registry_add_source(registry, &plugin, &err);
    ASSERT_EQ(FR_ERR, fr_registry_add_source(registry, &plugin, &err));
    ASSERT(strstr(err.message, "daukle.source/test") != NULL);
    fr_registry_destroy(registry);
    PASS();
}

TEST keeps_source_and_language_spaces_separate(void) {
    fr_registry *registry = fr_registry_create();
    int called = 0;
    fr_source_plugin plugin = { "daukle/x", fake_load, &called };
    fr_error err;
    fr_registry_add_source(registry, &plugin, &err);
    ASSERT(fr_registry_language(registry, "daukle/x") == NULL);
    fr_registry_destroy(registry);
    PASS();
}

TEST registers_and_finds_a_config_plugin(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_registry_add_config(registry, &PRIMARY, &err));
    const fr_config_plugin *found = fr_registry_config(registry, "daukle.config/aaa");
    ASSERT(found != NULL);
    ASSERT_STR_EQ("daukle.aaa", found->file_name);
    ASSERT(fr_registry_config(registry, "daukle.config/zzz") == NULL);
    fr_registry_destroy(registry);
    PASS();
}

TEST rejects_a_duplicate_config_capability(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_registry_add_config(registry, &PRIMARY, &err));
    ASSERT_EQ(FR_ERR, fr_registry_add_config(registry, &PRIMARY, &err));
    fr_registry_destroy(registry);
    PASS();
}

TEST walks_every_registered_config_plugin(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    fr_registry_add_config(registry, &PRIMARY, &err);
    fr_registry_add_config(registry, &OVERLAY, &err);
    ASSERT_EQ(2, (int) fr_registry_config_count(registry));
    ASSERT_EQ(0, fr_registry_config_at(registry, 0)->overlay);
    ASSERT_EQ(1, fr_registry_config_at(registry, 1)->overlay);
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
    RUN_TEST(registers_and_finds_a_config_plugin);
    RUN_TEST(rejects_a_duplicate_config_capability);
    RUN_TEST(walks_every_registered_config_plugin);
    GREATEST_MAIN_END();
}
