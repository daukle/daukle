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

TEST a_diamond_runs_its_shared_dependency_once(void) {
    fr_registry *registry = fr_registry_create();
    fr_manifest manifest = manifest_of(
        "{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{},\"tasks\":{"
        "\"all\":{\"dependsOn\":[\"left\",\"right\"]},"
        "\"left\":{\"dependsOn\":[\"base\"]},"
        "\"right\":{\"dependsOn\":[\"base\"]},"
        "\"base\":{}}}");
    fr_task_set set; fr_error err;
    ASSERT_EQ(FR_OK, fr_tasks_collect(registry, &manifest, &set, &err));

    fr_task_plan plan;
    ASSERT_EQ(FR_OK, fr_tasks_plan(&set, "all", &plan, &err));
    ASSERT_EQ(4u, plan.count);
    ASSERT_STR_EQ("base", plan.nodes[0]->name);
    ASSERT_STR_EQ("all", plan.nodes[3]->name);
    fr_tasks_plan_free(&plan);
    fr_tasks_set_free(&set);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    PASS();
}

TEST a_cycle_names_its_members(void) {
    fr_registry *registry = fr_registry_create();
    fr_manifest manifest = manifest_of(
        "{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{},\"tasks\":{"
        "\"a\":{\"dependsOn\":[\"b\"]},\"b\":{\"dependsOn\":[\"a\"]}}}");
    fr_task_set set; fr_error err;
    fr_tasks_collect(registry, &manifest, &set, &err);

    fr_task_plan plan;
    ASSERT_EQ(FR_ERR, fr_tasks_plan(&set, "a", &plan, &err));
    ASSERT(strstr(err.message, "depend on each other in a cycle") != NULL);
    ASSERT(strstr(err.message, "a -> b -> a") != NULL);
    fr_tasks_set_free(&set);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    PASS();
}

TEST a_part_of_naming_nothing_is_refused(void) {
    fr_registry *registry = fr_registry_create();
    fr_manifest manifest = manifest_of(
        "{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{},\"tasks\":{"
        "\"mine\":{\"partOf\":\"build\"}}}");
    fr_task_set set; fr_error err;
    fr_tasks_collect(registry, &manifest, &set, &err);

    fr_task_plan plan;
    ASSERT_EQ(FR_ERR, fr_tasks_plan(&set, "mine", &plan, &err));
    ASSERT(strstr(err.message, "is part of \"build\", which nothing declares") != NULL);
    fr_tasks_set_free(&set);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    PASS();
}

TEST a_depends_on_naming_nothing_is_refused(void) {
    fr_registry *registry = fr_registry_create();
    fr_manifest manifest = manifest_of(
        "{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{},\"tasks\":{"
        "\"mine\":{\"dependsOn\":[\"ghost\"]}}}");
    fr_task_set set; fr_error err;
    fr_tasks_collect(registry, &manifest, &set, &err);

    fr_task_plan plan;
    ASSERT_EQ(FR_ERR, fr_tasks_plan(&set, "mine", &plan, &err));
    ASSERT(strstr(err.message, "depends on \"ghost\", which nothing declares") != NULL);
    fr_tasks_set_free(&set);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    PASS();
}

TEST an_aggregator_pulls_in_what_joined_it(void) {
    fr_registry *registry = fr_registry_create();
    fr_manifest manifest = manifest_of(
        "{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{},\"tasks\":{"
        "\"build\":{},\"mine\":{\"partOf\":\"build\"}}}");
    fr_task_set set; fr_error err;
    fr_tasks_collect(registry, &manifest, &set, &err);

    fr_task_plan plan;
    ASSERT_EQ(FR_OK, fr_tasks_plan(&set, "build", &plan, &err));
    ASSERT_EQ(2u, plan.count);
    ASSERT_STR_EQ("mine", plan.nodes[0]->name);
    ASSERT_STR_EQ("build", plan.nodes[1]->name);
    fr_tasks_plan_free(&plan);
    fr_tasks_set_free(&set);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    PASS();
}

TEST the_same_graph_plans_the_same_order_twice(void) {
    fr_registry *registry = fr_registry_create();
    fr_manifest manifest = manifest_of(
        "{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{},\"tasks\":{"
        "\"all\":{\"dependsOn\":[\"one\",\"two\",\"three\"]},"
        "\"one\":{},\"two\":{\"dependsOn\":[\"one\"]},\"three\":{\"dependsOn\":[\"one\"]}}}");
    fr_task_set set; fr_error err;
    fr_tasks_collect(registry, &manifest, &set, &err);

    fr_task_plan first, second;
    ASSERT_EQ(FR_OK, fr_tasks_plan(&set, "all", &first, &err));
    ASSERT_EQ(FR_OK, fr_tasks_plan(&set, "all", &second, &err));
    ASSERT_EQ(first.count, second.count);
    for (size_t index = 0; index < first.count; index++) {
        ASSERT_STR_EQ(first.nodes[index]->name, second.nodes[index]->name);
    }
    /* and it is the order declaration implies, not merely a stable one */
    ASSERT_STR_EQ("one", first.nodes[0]->name);
    ASSERT_STR_EQ("all", first.nodes[first.count - 1]->name);
    fr_tasks_plan_free(&first);
    fr_tasks_plan_free(&second);
    fr_tasks_set_free(&set);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    PASS();
}

TEST a_goal_nothing_declares_is_refused(void) {
    fr_registry *registry = fr_registry_create();
    fr_manifest manifest = manifest_of("{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{}}");
    fr_task_set set; fr_error err;
    fr_tasks_collect(registry, &manifest, &set, &err);

    fr_task_plan plan;
    ASSERT_EQ(FR_ERR, fr_tasks_plan(&set, "build", &plan, &err));
    ASSERT(strstr(err.message, "no task \"build\" is declared") != NULL);
    fr_tasks_set_free(&set);
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
    RUN_TEST(a_diamond_runs_its_shared_dependency_once);
    RUN_TEST(a_cycle_names_its_members);
    RUN_TEST(a_part_of_naming_nothing_is_refused);
    RUN_TEST(a_depends_on_naming_nothing_is_refused);
    RUN_TEST(an_aggregator_pulls_in_what_joined_it);
    RUN_TEST(the_same_graph_plans_the_same_order_twice);
    RUN_TEST(a_goal_nothing_declares_is_refused);
    GREATEST_MAIN_END();
}
