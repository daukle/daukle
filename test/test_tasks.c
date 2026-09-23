#include "greatest.h"
#include "tasks.h"

#include "cJSON.h"
#include "config_lua.h"
#include "error.h"
#include "manifest.h"
#include "support.h"

#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>

static int never_runs(void *state, const fr_task_run_context *context, fr_error *err) {
    (void) state; (void) context; (void) err;
    return FR_ERR;
}

static char scratch[512];

static int path_exists(const char *path) {
#ifdef _WIN32
    struct _stat info;
    return _stat(path, &info) == 0;
#else
    struct stat info;
    return stat(path, &info) == 0;
#endif
}

/* Fails the run rather than merely recording something, so a hypothetical
   reordering that ran a task before creating its derived directory shows up
   as this test failing outright, not as a silently-missing assertion. */
static int counting_run(void *state, const fr_task_run_context *context, fr_error *err) {
    char full[512];
    snprintf(full, sizeof full, "%s/build/daukle/%s", scratch, context->toolchain->name);
    if (!path_exists(full)) {
        fr_error_set(err, "derived directory \"%s\" does not exist yet", full);
        return FR_ERR;
    }
    char *log = state;
    strncat(log, context->name, 64);
    strncat(log, ";", 2);
    return FR_OK;
}

static int failing_run(void *state, const fr_task_run_context *context, fr_error *err) {
    (void) state; (void) context;
    fr_error_set(err, "the compiler said no");
    return FR_ERR;
}

static char recorded_cwd[128];

/* Mirrors what config_lua.c's lua_task_run does around a real plugin's run
   callback: publish context->derived_dir_relative through fr_lua_set_task_cwd,
   the same accessor daukle.exec's cwd default reads, and reset it once done. */
static int cwd_recording_run(void *state, const fr_task_run_context *context, fr_error *err) {
    (void) state; (void) err;
    fr_lua_set_task_cwd(context->derived_dir_relative);
    const char *published = fr_lua_task_cwd();
    if (published != NULL) snprintf(recorded_cwd, sizeof recorded_cwd, "%s", published);
    fr_lua_set_task_cwd(NULL);
    return FR_OK;
}

static void make_scratch(const char *label) {
    snprintf(scratch, sizeof scratch, "%s/daukle_test_tasks_run_%s_%d", fr_test_temp_base(), label,
             fr_test_process_id());
    fr_test_remove_tree(scratch);
    fr_test_make_directory(scratch);
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

TEST a_cycle_through_part_of_is_refused(void) {
    fr_registry *registry = fr_registry_create();
    fr_manifest manifest = manifest_of(
        "{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{},\"tasks\":{"
        "\"a\":{\"partOf\":\"b\"},\"b\":{\"partOf\":\"a\"}}}");
    fr_task_set set; fr_error err;
    fr_tasks_collect(registry, &manifest, &set, &err);

    fr_task_plan plan;
    ASSERT_EQ(FR_ERR, fr_tasks_plan(&set, "a", &plan, &err));
    ASSERT(strstr(err.message, "depend on each other in a cycle") != NULL);
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

/* Planning the same fr_task_set object twice is trivially equal to itself and
   proves nothing about ordering. The real question is whether two manifests
   that declare the SAME graph, but with their [tasks] entries written in a
   different order, plan to the same order. That question is worth asking
   specifically because visit_joiners walks the set in set order (the
   manifest's own declaration order, for a manifest-only task), so a
   partOf-heavy graph genuinely could come out differently depending on how
   it was written down. Here mid_a and mid_b both join root through partOf, but
   mid_b also depends on mid_a, so wherever visit_joiners' scan meets them
   first, the dependency forces mid_a to be visited, and appended, before
   mid_b: the declared order of the surrounding text cannot move that pair
   relative to each other, however it is written. */
TEST the_same_graph_plans_the_same_order_in_either_declaration_order(void) {
    fr_registry *registry = fr_registry_create();
    fr_manifest first_order = manifest_of(
        "{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{},\"tasks\":{"
        "\"root\":{},"
        "\"mid_a\":{\"partOf\":\"root\"},"
        "\"mid_b\":{\"partOf\":\"root\",\"dependsOn\":[\"mid_a\"]}}}");
    fr_manifest second_order = manifest_of(
        "{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{},\"tasks\":{"
        "\"mid_b\":{\"partOf\":\"root\",\"dependsOn\":[\"mid_a\"]},"
        "\"mid_a\":{\"partOf\":\"root\"},"
        "\"root\":{}}}");
    fr_task_set first_set, second_set; fr_error err;
    ASSERT_EQ(FR_OK, fr_tasks_collect(registry, &first_order, &first_set, &err));
    ASSERT_EQ(FR_OK, fr_tasks_collect(registry, &second_order, &second_set, &err));

    fr_task_plan first, second;
    ASSERT_EQ(FR_OK, fr_tasks_plan(&first_set, "root", &first, &err));
    ASSERT_EQ(FR_OK, fr_tasks_plan(&second_set, "root", &second, &err));
    ASSERT_EQ(first.count, second.count);
    for (size_t index = 0; index < first.count; index++) {
        ASSERT_STR_EQ(first.nodes[index]->name, second.nodes[index]->name);
    }
    /* and it is the order the dependency implies, not merely a stable one */
    ASSERT_STR_EQ("mid_a", first.nodes[0]->name);
    ASSERT_STR_EQ("mid_b", first.nodes[1]->name);
    ASSERT_STR_EQ("root", first.nodes[2]->name);

    fr_tasks_plan_free(&first);
    fr_tasks_plan_free(&second);
    fr_tasks_set_free(&first_set);
    fr_tasks_set_free(&second_set);
    fr_manifest_free(&first_order);
    fr_manifest_free(&second_order);
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

TEST a_plan_runs_its_tasks_in_order(void) {
    make_scratch("order");
    char log[128];
    log[0] = '\0';

    fr_registry *registry = fr_registry_create();
    const char *depends_on_one[] = { "a:one" };
    fr_task_plugin one = { "daukle.task/a:one", NULL, NULL, 0, counting_run, log };
    fr_task_plugin two = { "daukle.task/a:two", NULL, depends_on_one, 1, counting_run, log };
    fr_error err;
    fr_registry_add_task(registry, &one, &err);
    fr_registry_add_task(registry, &two, &err);

    fr_manifest manifest = manifest_of(
        "{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{},"
        "\"toolchains\":{\"a\":\">=1.0\"}}");
    fr_task_set set;
    ASSERT_EQ(FR_OK, fr_tasks_collect(registry, &manifest, &set, &err));

    fr_task_plan plan;
    ASSERT_EQ(FR_OK, fr_tasks_plan(&set, "a:two", &plan, &err));

    fr_session session;
    memset(&session, 0, sizeof session);
    session.registry = registry;
    session.manifest = manifest;
    session.manifest_dir = scratch;

    ASSERT_EQ(FR_OK, fr_tasks_run(&plan, &session, &err));
    ASSERT_STR_EQ("a:one;a:two;", log);

    fr_tasks_plan_free(&plan);
    fr_tasks_set_free(&set);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_test_remove_tree(scratch);
    PASS();
}

TEST a_failing_task_stops_the_run_naming_itself(void) {
    make_scratch("fail");

    fr_registry *registry = fr_registry_create();
    fr_task_plugin one = { "daukle.task/a:one", NULL, NULL, 0, failing_run, NULL };
    fr_error err;
    fr_registry_add_task(registry, &one, &err);

    fr_manifest manifest = manifest_of(
        "{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{},"
        "\"toolchains\":{\"a\":\">=1.0\"}}");
    fr_task_set set;
    ASSERT_EQ(FR_OK, fr_tasks_collect(registry, &manifest, &set, &err));

    fr_task_plan plan;
    ASSERT_EQ(FR_OK, fr_tasks_plan(&set, "a:one", &plan, &err));

    fr_session session;
    memset(&session, 0, sizeof session);
    session.registry = registry;
    session.manifest = manifest;
    session.manifest_dir = scratch;

    ASSERT_EQ(FR_ERR, fr_tasks_run(&plan, &session, &err));
    ASSERT(strstr(err.message, "task \"a:one\"") != NULL);
    ASSERT(strstr(err.message, "the compiler said no") != NULL);

    fr_tasks_plan_free(&plan);
    fr_tasks_set_free(&set);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_test_remove_tree(scratch);
    PASS();
}

TEST a_run_publishes_the_base_relative_derived_directory(void) {
    make_scratch("cwd");
    recorded_cwd[0] = '\0';

    fr_registry *registry = fr_registry_create();
    fr_task_plugin one = { "daukle.task/a:one", NULL, NULL, 0, cwd_recording_run, NULL };
    fr_error err;
    fr_registry_add_task(registry, &one, &err);

    fr_manifest manifest = manifest_of(
        "{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{},"
        "\"toolchains\":{\"a\":\">=1.0\"}}");
    fr_task_set set;
    ASSERT_EQ(FR_OK, fr_tasks_collect(registry, &manifest, &set, &err));

    fr_task_plan plan;
    ASSERT_EQ(FR_OK, fr_tasks_plan(&set, "a:one", &plan, &err));

    fr_session session;
    memset(&session, 0, sizeof session);
    session.registry = registry;
    session.manifest = manifest;
    session.manifest_dir = scratch;

    ASSERT_EQ(FR_OK, fr_tasks_run(&plan, &session, &err));
    ASSERT_STR_EQ("build/daukle/a", recorded_cwd);
    ASSERT(strstr(recorded_cwd, scratch) == NULL);

    fr_tasks_plan_free(&plan);
    fr_tasks_set_free(&set);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_test_remove_tree(scratch);
    PASS();
}

/* A partOf joiner and a dependsOn joiner point at "build" in opposite
   directions (mine runs before build, build runs before other), so they must
   come back from different calls, one per fr_task_join_kind, rather than
   merged into one list that cannot tell them apart. */
TEST a_task_lists_what_joined_it(void) {
    fr_registry *registry = fr_registry_create();
    fr_manifest manifest = manifest_of(
        "{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{},\"tasks\":{"
        "\"build\":{},\"mine\":{\"partOf\":\"build\"},\"other\":{\"dependsOn\":[\"build\"]}}}");
    fr_task_set set; fr_error err;
    fr_tasks_collect(registry, &manifest, &set, &err);

    const char *part_of_names[8];
    size_t part_of_count = fr_tasks_joiners(&set, "build", FR_TASK_JOIN_PART_OF, part_of_names, 8);
    ASSERT_EQ(1u, part_of_count);
    ASSERT_STR_EQ("mine", part_of_names[0]);

    const char *depends_on_names[8];
    size_t depends_on_count = fr_tasks_joiners(&set, "build", FR_TASK_JOIN_DEPENDS_ON, depends_on_names, 8);
    ASSERT_EQ(1u, depends_on_count);
    ASSERT_STR_EQ("other", depends_on_names[0]);

    fr_tasks_set_free(&set);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    PASS();
}

/* main.c has no test binary of its own, so the sentence run_task prints for
   an unknown goal is tested here, against the function that actually
   composes it, rather than against a token (the plugin count) that also
   appears unchanged in the function's own input. */
TEST the_zero_plugin_unknown_message_says_the_project_has_none(void) {
    char message[256];
    fr_tasks_unknown_message("build", 0, message, sizeof message);
    ASSERT(strstr(message, "declares no plugins") != NULL);
    ASSERT(strstr(message, "\"build\"") != NULL);
    PASS();
}

TEST the_nonzero_plugin_unknown_message_names_the_count(void) {
    char message[256];
    fr_tasks_unknown_message("build", 3, message, sizeof message);
    ASSERT(strstr(message, "tasks come from plugins") != NULL);
    ASSERT(strstr(message, "3") != NULL);
    ASSERT(strstr(message, "\"build\"") != NULL);
    PASS();
}

TEST joiners_past_capacity_are_still_counted(void) {
    fr_registry *registry = fr_registry_create();
    fr_manifest manifest = manifest_of(
        "{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{},\"tasks\":{"
        "\"build\":{},"
        "\"one\":{\"dependsOn\":[\"build\"]},"
        "\"two\":{\"dependsOn\":[\"build\"]},"
        "\"three\":{\"dependsOn\":[\"build\"]}}}");
    fr_task_set set; fr_error err;
    fr_tasks_collect(registry, &manifest, &set, &err);

    const char *names[2];
    size_t count = fr_tasks_joiners(&set, "build", FR_TASK_JOIN_DEPENDS_ON, names, 2);
    ASSERT_EQ(3u, count);
    ASSERT_STR_EQ("one", names[0]);
    ASSERT_STR_EQ("two", names[1]);
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
    RUN_TEST(a_cycle_through_part_of_is_refused);
    RUN_TEST(a_part_of_naming_nothing_is_refused);
    RUN_TEST(a_depends_on_naming_nothing_is_refused);
    RUN_TEST(an_aggregator_pulls_in_what_joined_it);
    RUN_TEST(the_same_graph_plans_the_same_order_in_either_declaration_order);
    RUN_TEST(a_goal_nothing_declares_is_refused);
    RUN_TEST(a_plan_runs_its_tasks_in_order);
    RUN_TEST(a_failing_task_stops_the_run_naming_itself);
    RUN_TEST(a_run_publishes_the_base_relative_derived_directory);
    RUN_TEST(a_task_lists_what_joined_it);
    RUN_TEST(the_zero_plugin_unknown_message_says_the_project_has_none);
    RUN_TEST(the_nonzero_plugin_unknown_message_names_the_count);
    RUN_TEST(joiners_past_capacity_are_still_counted);
    GREATEST_MAIN_END();
}
