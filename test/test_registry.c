#include "greatest.h"
#include "registry.h"

#include "cJSON.h"

#include <stdio.h>
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

static int never_applies(void *state, const fr_consumer *consumer, const fr_resolved *resolved,
                         size_t count, const char *original_text, char **out_text, fr_error *err) {
    (void) state; (void) consumer; (void) resolved; (void) count;
    (void) original_text; (void) out_text; (void) err;
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

/* No language capability is compiled in any more, so every caller of
   fr_registry_add_language sits behind config_lua's own "declared twice" guard
   and nothing else asserts this branch. */
TEST rejects_a_duplicate_language_capability(void) {
    fr_registry *registry = fr_registry_create();
    fr_language_plugin plugin = { "daukle.language/test", never_applies, NULL };
    fr_error err;
    ASSERT_EQ(FR_OK, fr_registry_add_language(registry, &plugin, &err));
    ASSERT_EQ(FR_ERR, fr_registry_add_language(registry, &plugin, &err));
    ASSERT_STR_EQ("capability \"daukle.language/test\" is already registered", err.message);
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

static int stub_generate(void *state, const fr_toolchain *toolchain, const char *project,
                         const char *version, const char *root,
                         const fr_resolved *resolved, size_t count,
                         fr_generated_file **out_files, size_t *out_count, fr_error *err) {
    (void) state; (void) toolchain; (void) project; (void) version; (void) root;
    (void) resolved; (void) count; (void) err;
    *out_files = NULL;
    *out_count = 0;
    return FR_OK;
}

TEST a_toolchain_is_found_by_its_capability(void) {
    fr_registry *registry = fr_registry_create();
    fr_toolchain_plugin plugin = { "daukle.toolchain/stub", stub_generate, NULL };
    fr_error err;

    ASSERT_EQ(FR_OK, fr_registry_add_toolchain(registry, &plugin, &err));
    const fr_toolchain_plugin *found = fr_registry_toolchain(registry, "daukle.toolchain/stub");
    int found_it = found != NULL;
    int same_callback = found_it && found->generate == stub_generate;
    fr_registry_destroy(registry);

    ASSERT(found_it);
    ASSERT(same_callback);
    PASS();
}

TEST a_toolchain_capability_cannot_be_registered_twice(void) {
    fr_registry *registry = fr_registry_create();
    fr_toolchain_plugin plugin = { "daukle.toolchain/stub", stub_generate, NULL };
    fr_error err;
    fr_registry_add_toolchain(registry, &plugin, &err);

    int status = fr_registry_add_toolchain(registry, &plugin, &err);
    char message[sizeof err.message];
    snprintf(message, sizeof message, "%s", err.message);
    fr_registry_destroy(registry);

    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "is already registered") != NULL);
    PASS();
}

TEST a_toolchain_and_a_language_may_share_a_name(void) {
    fr_registry *registry = fr_registry_create();
    fr_toolchain_plugin toolchain = { "daukle.toolchain/stub", stub_generate, NULL };
    fr_error err;
    int added_toolchain = fr_registry_add_toolchain(registry, &toolchain, &err) == FR_OK;
    int toolchain_is_not_a_language = fr_registry_language(registry, "daukle.toolchain/stub") == NULL;
    fr_registry_destroy(registry);

    ASSERT(added_toolchain);
    ASSERT(toolchain_is_not_a_language);
    PASS();
}

static int never_runs(void *state, const fr_task_run_context *context, fr_error *err) {
    (void) state; (void) context; (void) err;
    return FR_ERR;
}

TEST a_task_is_found_by_its_capability(void) {
    fr_registry *registry = fr_registry_create();
    fr_task_plugin plugin = { "daukle.task/cmake:build", "build", NULL, 0, never_runs, NULL };
    fr_error err;
    ASSERT_EQ(FR_OK, fr_registry_add_task(registry, &plugin, &err));

    const fr_task_plugin *found = fr_registry_task(registry, "daukle.task/cmake:build");
    ASSERT(found != NULL);
    ASSERT_STR_EQ("build", found->part_of);
    fr_registry_destroy(registry);
    PASS();
}

TEST a_task_capability_cannot_be_registered_twice(void) {
    fr_registry *registry = fr_registry_create();
    fr_task_plugin plugin = { "daukle.task/build", NULL, NULL, 0, NULL, NULL };
    fr_error err;
    ASSERT_EQ(FR_OK, fr_registry_add_task(registry, &plugin, &err));
    ASSERT_EQ(FR_ERR, fr_registry_add_task(registry, &plugin, &err));
    ASSERT(strstr(err.message, "already registered") != NULL);
    fr_registry_destroy(registry);
    PASS();
}

TEST every_registered_task_is_walkable(void) {
    fr_registry *registry = fr_registry_create();
    fr_task_plugin first = { "daukle.task/build", NULL, NULL, 0, NULL, NULL };
    fr_task_plugin second = { "daukle.task/cmake:build", "build", NULL, 0, never_runs, NULL };
    fr_error err;
    ASSERT_EQ(FR_OK, fr_registry_add_task(registry, &first, &err));
    ASSERT_EQ(FR_OK, fr_registry_add_task(registry, &second, &err));

    ASSERT_EQ(2u, fr_registry_task_count(registry));
    ASSERT_STR_EQ("daukle.task/build", fr_registry_task_at(registry, 0)->capability);
    ASSERT_STR_EQ("daukle.task/cmake:build", fr_registry_task_at(registry, 1)->capability);
    ASSERT(fr_registry_task_at(registry, 2) == NULL);
    fr_registry_destroy(registry);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(returns_a_plugin_registered_under_its_capability);
    RUN_TEST(returns_null_for_an_unregistered_capability);
    RUN_TEST(rejects_a_duplicate_capability);
    RUN_TEST(rejects_a_duplicate_language_capability);
    RUN_TEST(keeps_source_and_language_spaces_separate);
    RUN_TEST(registers_and_finds_a_config_plugin);
    RUN_TEST(rejects_a_duplicate_config_capability);
    RUN_TEST(walks_every_registered_config_plugin);
    RUN_TEST(a_toolchain_is_found_by_its_capability);
    RUN_TEST(a_toolchain_capability_cannot_be_registered_twice);
    RUN_TEST(a_toolchain_and_a_language_may_share_a_name);
    RUN_TEST(a_task_is_found_by_its_capability);
    RUN_TEST(a_task_capability_cannot_be_registered_twice);
    RUN_TEST(every_registered_task_is_walkable);
    GREATEST_MAIN_END();
}
