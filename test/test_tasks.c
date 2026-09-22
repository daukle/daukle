#include "greatest.h"
#include "tasks.h"

#include "cJSON.h"
#include "manifest.h"

#include <stdlib.h>
#include <string.h>

static int never_runs(void *state, const fr_task_run_context *context, fr_error *err) {
    (void) state; (void) context; (void) err;
    return FR_ERR;
}

static fr_manifest manifest_of(const char *json) {
    fr_manifest manifest; fr_error err;
    cJSON *root = cJSON_Parse(json);
    if (fr_manifest_from_document(root, "daukle.toml", &manifest, &err) != FR_OK) abort();
    return manifest;
}

TEST a_declared_task_needs_its_toolchain_declared(void) {
    fr_registry *registry = fr_registry_create();
    fr_task_plugin plugin = { "daukle.task/cmake:build", NULL, NULL, 0, never_runs, NULL };
    fr_error err;
    fr_registry_add_task(registry, &plugin, &err);

    fr_manifest without = manifest_of("{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{}}");
    fr_task_set set;
    ASSERT_EQ(FR_OK, fr_tasks_collect(registry, &without, &set, &err));
    ASSERT_EQ(0u, set.count);
    fr_tasks_set_free(&set);
    fr_manifest_free(&without);

    fr_manifest with = manifest_of(
        "{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{},\"toolchains\":{\"cmake\":\">=3.20\"}}");
    ASSERT_EQ(FR_OK, fr_tasks_collect(registry, &with, &set, &err));
    ASSERT_EQ(1u, set.count);
    ASSERT_STR_EQ("cmake:build", set.nodes[0].name);
    fr_tasks_set_free(&set);
    fr_manifest_free(&with);
    fr_registry_destroy(registry);
    PASS();
}

TEST a_bare_task_always_exists(void) {
    fr_registry *registry = fr_registry_create();
    fr_task_plugin plugin = { "daukle.task/build", NULL, NULL, 0, NULL, NULL };
    fr_error err;
    fr_registry_add_task(registry, &plugin, &err);

    fr_manifest manifest = manifest_of("{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{}}");
    fr_task_set set;
    ASSERT_EQ(FR_OK, fr_tasks_collect(registry, &manifest, &set, &err));
    ASSERT_EQ(1u, set.count);
    fr_tasks_set_free(&set);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    PASS();
}

TEST a_bare_task_may_not_carry_a_body(void) {
    fr_registry *registry = fr_registry_create();
    fr_task_plugin plugin = { "daukle.task/build", NULL, NULL, 0, never_runs, NULL };
    fr_error err;
    fr_registry_add_task(registry, &plugin, &err);

    fr_manifest manifest = manifest_of("{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{}}");
    fr_task_set set;
    ASSERT_EQ(FR_ERR, fr_tasks_collect(registry, &manifest, &set, &err));
    ASSERT(strstr(err.message, "may not run anything") != NULL);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    PASS();
}

TEST a_task_named_after_a_command_is_refused(void) {
    fr_registry *registry = fr_registry_create();
    fr_task_plugin plugin = { "daukle.task/clean", NULL, NULL, 0, NULL, NULL };
    fr_error err;
    fr_registry_add_task(registry, &plugin, &err);

    fr_manifest manifest = manifest_of("{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{}}");
    fr_task_set set;
    ASSERT_EQ(FR_ERR, fr_tasks_collect(registry, &manifest, &set, &err));
    ASSERT(strstr(err.message, "is also a daukle command") != NULL);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    PASS();
}

TEST a_manifest_may_not_claim_a_toolchain_prefix(void) {
    fr_registry *registry = fr_registry_create();
    fr_manifest manifest = manifest_of(
        "{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{},\"tasks\":{\"cmake:build\":{}}}");
    fr_task_set set; fr_error err;
    ASSERT_EQ(FR_ERR, fr_tasks_collect(registry, &manifest, &set, &err));
    ASSERT(strstr(err.message, "belongs to the plugin") != NULL);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    PASS();
}

TEST a_manifest_block_adds_edges_to_a_declared_task(void) {
    fr_registry *registry = fr_registry_create();
    fr_task_plugin plugin = { "daukle.task/cmake:build", NULL, NULL, 0, never_runs, NULL };
    fr_error err;
    fr_registry_add_task(registry, &plugin, &err);

    fr_manifest manifest = manifest_of(
        "{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{},\"toolchains\":{\"cmake\":\">=3.20\"},"
        "\"tasks\":{\"cmake:build\":{\"dependsOn\":[\"headers\"]},\"headers\":{}}}");
    fr_task_set set;
    ASSERT_EQ(FR_OK, fr_tasks_collect(registry, &manifest, &set, &err));
    ASSERT_EQ(2u, set.count);
    const fr_task_node *build = fr_tasks_find(&set, "cmake:build");
    ASSERT(build != NULL);
    ASSERT_EQ(1u, build->extra_depends_on_count);
    ASSERT_STR_EQ("headers", build->extra_depends_on[0]);
    fr_tasks_set_free(&set);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    PASS();
}

TEST a_malformed_task_name_is_refused(void) {
    fr_registry *registry = fr_registry_create();
    fr_manifest manifest = manifest_of(
        "{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{},\"tasks\":{\"-build\":{}}}");
    fr_task_set set; fr_error err;
    ASSERT_EQ(FR_ERR, fr_tasks_collect(registry, &manifest, &set, &err));
    ASSERT(strstr(err.message, "is not a usable task name") != NULL);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    PASS();
}

TEST a_task_capability_must_carry_the_task_prefix(void) {
    fr_registry *registry = fr_registry_create();
    fr_task_plugin plugin = { "daukle.other/x", NULL, NULL, 0, NULL, NULL };
    fr_error err;
    fr_registry_add_task(registry, &plugin, &err);

    fr_manifest manifest = manifest_of("{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{}}");
    fr_task_set set;
    ASSERT_EQ(FR_ERR, fr_tasks_collect(registry, &manifest, &set, &err));
    ASSERT(strstr(err.message, "is not a task capability") != NULL);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(a_declared_task_needs_its_toolchain_declared);
    RUN_TEST(a_bare_task_always_exists);
    RUN_TEST(a_bare_task_may_not_carry_a_body);
    RUN_TEST(a_task_named_after_a_command_is_refused);
    RUN_TEST(a_manifest_may_not_claim_a_toolchain_prefix);
    RUN_TEST(a_manifest_block_adds_edges_to_a_declared_task);
    RUN_TEST(a_malformed_task_name_is_refused);
    RUN_TEST(a_task_capability_must_carry_the_task_prefix);
    GREATEST_MAIN_END();
}
