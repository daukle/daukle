#include "greatest.h"
#include "project/tasks.h"

#include "cJSON.h"
#include "config/config_lua.h"
#include "config/manifest.h"
#include "util/error.h"
#include "support.h"

#include <stdio.h>
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

/* Writes a marker beside the manifest, so a manifest task's child can say
   whether the plugin task it depends on had already run when it started. */
static int marker_run(void *state, const fr_task_run_context *context, fr_error *err) {
    (void) state; (void) context;
    char full[512];
    snprintf(full, sizeof full, "%s/prior.txt", scratch);
    FILE *file = fopen(full, "wb");
    if (file == NULL) {
        fr_error_set(err, "cannot create \"%s\"", full);
        return FR_ERR;
    }
    fclose(file);
    return FR_OK;
}

static const char *const CREDENTIALS[] = { "DAUKLE_TOKEN", "GITHUB_TOKEN" };

/* The child half of the escape-hatch tests. Spawned as this binary under its
   own bare name, so "tool" is a real search-path resolution rather than a
   path the test handed over, and it writes to a RELATIVE path, so where the
   file lands is what says which working directory daukle started it in.

   Each value goes on its own line and nothing else does: a credential that
   leaked would otherwise be asserted absent from a buffer holding the whole
   diagnostic that printed it. */
static int run_as_task_child(int argc, char **argv) {
    FILE *out = fopen(argv[2], "wb");
    if (out == NULL) return 70;
    for (int index = 4; index < argc; index++) fprintf(out, "[%s]\n", argv[index]);
    for (size_t index = 0; index < sizeof CREDENTIALS / sizeof CREDENTIALS[0]; index++) {
        const char *value = getenv(CREDENTIALS[index]);
        fprintf(out, "%s=%s\n", CREDENTIALS[index], value == NULL ? "<unset>" : value);
    }
    const char *ordinary = getenv("DAUKLE_TEST_ORDINARY");
    fprintf(out, "DAUKLE_TEST_ORDINARY=%s\n", ordinary == NULL ? "<unset>" : ordinary);
    fprintf(out, "prior=%s\n", path_exists("prior.txt") ? "yes" : "no");
    fclose(out);
    return atoi(argv[3]);
}

static int read_child_output(const char *relative, char *out, size_t size) {
    char full[600];
    snprintf(full, sizeof full, "%s/%s", scratch, relative);
    FILE *file = fopen(full, "rb");
    if (file == NULL) return 0;
    size_t read = fread(out, 1, size - 1, file);
    out[read] = '\0';
    fclose(file);
    return 1;
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

TEST a_publish_task_takes_its_toolchain_from_its_destination(void) {
    fr_registry *registry = fr_registry_create();
    fr_task_plugin plugin = { "daukle.task/publish:github", NULL, NULL, 0, never_runs, NULL };
    fr_error err;
    fr_registry_add_task(registry, &plugin, &err);

    fr_manifest manifest = manifest_of(
        "{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{},"
        "\"toolchains\":{\"gradle\":\"8.10\"},"
        "\"publish\":{\"github\":{\"from\":\"gradle\"}}}");
    fr_task_set set;
    ASSERT_EQ(FR_OK, fr_tasks_collect(registry, &manifest, &set, &err));
    ASSERT_EQ(1u, set.count);
    ASSERT_STR_EQ("publish:github", set.nodes[0].name);
    ASSERT(set.nodes[0].toolchain != NULL);
    ASSERT_STR_EQ("gradle", set.nodes[0].toolchain->name);
    ASSERT(set.nodes[0].publish != NULL);
    ASSERT_STR_EQ("github", set.nodes[0].publish->name);
    fr_tasks_set_free(&set);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    PASS();
}

TEST a_publish_task_with_no_destination_declared_is_skipped(void) {
    fr_registry *registry = fr_registry_create();
    fr_task_plugin plugin = { "daukle.task/publish:github", NULL, NULL, 0, never_runs, NULL };
    fr_error err;
    fr_registry_add_task(registry, &plugin, &err);

    fr_manifest manifest = manifest_of(
        "{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{},"
        "\"toolchains\":{\"gradle\":\"8.10\"}}");
    fr_task_set set;
    ASSERT_EQ(FR_OK, fr_tasks_collect(registry, &manifest, &set, &err));
    ASSERT_EQ(0u, set.count);
    fr_tasks_set_free(&set);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    PASS();
}

TEST an_ordinary_task_carries_no_publish_target(void) {
    fr_registry *registry = fr_registry_create();
    fr_task_plugin plugin = { "daukle.task/gradle:build", NULL, NULL, 0, never_runs, NULL };
    fr_error err;
    fr_registry_add_task(registry, &plugin, &err);

    fr_manifest manifest = manifest_of(
        "{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{},"
        "\"toolchains\":{\"gradle\":\"8.10\"}}");
    fr_task_set set;
    ASSERT_EQ(FR_OK, fr_tasks_collect(registry, &manifest, &set, &err));
    ASSERT_EQ(1u, set.count);
    ASSERT(set.nodes[0].publish == NULL);
    fr_tasks_set_free(&set);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    PASS();
}

TEST a_task_named_publish_is_refused(void) {
    fr_registry *registry = fr_registry_create();
    fr_task_plugin plugin = { "daukle.task/publish", NULL, NULL, 0, NULL, NULL };
    fr_error err;
    fr_registry_add_task(registry, &plugin, &err);

    fr_manifest manifest = manifest_of(
        "{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{}}");
    fr_task_set set;
    int status = fr_tasks_collect(registry, &manifest, &set, &err);
    char message[256];
    snprintf(message, sizeof message, "%s", status == FR_OK ? "" : err.message);
    if (status == FR_OK) fr_tasks_set_free(&set);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);

    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "that is also a daukle command") != NULL);
    PASS();
}

static char seen_publish_name[64];

static int publish_recording_run(void *state, const fr_task_run_context *context, fr_error *err) {
    (void) state; (void) err;
    snprintf(seen_publish_name, sizeof seen_publish_name, "%s",
             context->publish == NULL ? "<none>" : context->publish->name);
    return FR_OK;
}

TEST a_publish_task_run_receives_its_destination(void) {
    make_scratch("publish");
    fr_registry *registry = fr_registry_create();
    fr_task_plugin plugin = { "daukle.task/publish:github", NULL, NULL, 0,
                              publish_recording_run, NULL };
    fr_error err;
    fr_registry_add_task(registry, &plugin, &err);

    fr_manifest manifest = manifest_of(
        "{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{},"
        "\"toolchains\":{\"gradle\":\"8.10\"},"
        "\"publish\":{\"github\":{\"from\":\"gradle\"}}}");
    fr_task_set set;
    ASSERT_EQ(FR_OK, fr_tasks_collect(registry, &manifest, &set, &err));
    fr_task_plan plan;
    ASSERT_EQ(FR_OK, fr_tasks_plan(&set, "publish:github", &plan, &err));

    fr_session session;
    memset(&session, 0, sizeof session);
    session.registry = registry;
    session.manifest = manifest;
    session.manifest_dir = scratch;

    seen_publish_name[0] = '\0';
    int status = fr_tasks_run(&plan, &session, &err);
    char seen[64];
    snprintf(seen, sizeof seen, "%s", seen_publish_name);

    fr_tasks_plan_free(&plan);
    fr_tasks_set_free(&set);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_test_remove_tree(scratch);

    ASSERT_EQ(FR_OK, status);
    ASSERT_STR_EQ("github", seen);
    PASS();
}

TEST a_manifest_orders_a_publish_step_after_a_build(void) {
    make_scratch("publish_order");
    char log[128];
    log[0] = '\0';

    fr_registry *registry = fr_registry_create();
    fr_task_plugin build = { "daukle.task/gradle:build", NULL, NULL, 0, counting_run, log };
    fr_task_plugin publish = { "daukle.task/publish:github", NULL, NULL, 0, counting_run, log };
    fr_error err;
    fr_registry_add_task(registry, &build, &err);
    fr_registry_add_task(registry, &publish, &err);

    fr_manifest manifest = manifest_of(
        "{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{},"
        "\"toolchains\":{\"gradle\":\"8.10\"},"
        "\"publish\":{\"github\":{\"from\":\"gradle\"}},"
        "\"tasks\":{\"publish:github\":{\"dependsOn\":[\"gradle:build\"]}}}");
    fr_task_set set;
    ASSERT_EQ(FR_OK, fr_tasks_collect(registry, &manifest, &set, &err));
    fr_task_plan plan;
    ASSERT_EQ(FR_OK, fr_tasks_plan(&set, "publish:github", &plan, &err));

    fr_session session;
    memset(&session, 0, sizeof session);
    session.registry = registry;
    session.manifest = manifest;
    session.manifest_dir = scratch;

    int status = fr_tasks_run(&plan, &session, &err);
    char order[128];
    snprintf(order, sizeof order, "%s", log);

    fr_tasks_plan_free(&plan);
    fr_tasks_set_free(&set);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_test_remove_tree(scratch);

    ASSERT_EQ(FR_OK, status);
    ASSERT_STR_EQ("gradle:build;publish:github;", order);
    PASS();
}

TEST a_destination_with_no_publisher_is_named(void) {
    fr_registry *registry = fr_registry_create();
    fr_error err;
    fr_manifest manifest = manifest_of(
        "{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{},"
        "\"toolchains\":{\"gradle\":\"8.10\"},"
        "\"publish\":{\"github\":{\"from\":\"gradle\"}}}");
    fr_task_set set;
    ASSERT_EQ(FR_OK, fr_tasks_collect(registry, &manifest, &set, &err));

    int status = fr_tasks_check_publish(&set, &manifest, NULL, &err);
    char message[256];
    snprintf(message, sizeof message, "%s", status == FR_OK ? "" : err.message);

    fr_tasks_set_free(&set);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);

    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "no plugin declares a publisher named \"github\"") != NULL);
    PASS();
}

TEST a_destination_with_a_publisher_passes_the_check(void) {
    fr_registry *registry = fr_registry_create();
    fr_task_plugin plugin = { "daukle.task/publish:github", NULL, NULL, 0, never_runs, NULL };
    fr_error err;
    fr_registry_add_task(registry, &plugin, &err);

    fr_manifest manifest = manifest_of(
        "{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{},"
        "\"toolchains\":{\"gradle\":\"8.10\"},"
        "\"publish\":{\"github\":{\"from\":\"gradle\"}}}");
    fr_task_set set;
    ASSERT_EQ(FR_OK, fr_tasks_collect(registry, &manifest, &set, &err));
    int status = fr_tasks_check_publish(&set, &manifest, NULL, &err);

    fr_tasks_set_free(&set);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);

    ASSERT_EQ(FR_OK, status);
    PASS();
}

TEST a_destination_whose_plan_cannot_be_built_is_refused(void) {
    fr_registry *registry = fr_registry_create();
    fr_task_plugin plugin = { "daukle.task/publish:github", NULL, NULL, 0, never_runs, NULL };
    fr_error err;
    fr_registry_add_task(registry, &plugin, &err);

    fr_manifest manifest = manifest_of(
        "{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{},"
        "\"toolchains\":{\"gradle\":\"8.10\"},"
        "\"publish\":{\"github\":{\"from\":\"gradle\"}},"
        "\"tasks\":{\"publish:github\":{\"dependsOn\":[\"gradle:buidl\"]}}}");
    fr_task_set set;
    ASSERT_EQ(FR_OK, fr_tasks_collect(registry, &manifest, &set, &err));

    int status = fr_tasks_check_publish(&set, &manifest, NULL, &err);
    char message[256];
    snprintf(message, sizeof message, "%s", status == FR_OK ? "" : err.message);

    fr_tasks_set_free(&set);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);

    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "depends on \"gradle:buidl\", which nothing declares") != NULL);
    PASS();
}

/* The one Critical the whole-branch review found lived exactly here: a
   destination used to be planned only once the destination before it had
   already uploaded, so a broken second one surfaced after the first had
   published, against a guarantee with no rollback behind it. Every other case
   in this file names ONE destination and passes against that bug unchanged,
   which is why this one names two and breaks only the later.

   The break is a CYCLE rather than a dangling dependency, and that distinction
   is the test: fr_tasks_plan runs check_every_edge over the whole set before it
   walks anything, so a missing task is refused whichever goal is planned and
   cannot be made to belong to one destination. A cycle is found by the walk, so
   it belongs to the goal that reaches it and to no other. */
TEST a_later_destinations_broken_plan_is_refused_before_any_of_them_runs(void) {
    fr_registry *registry = fr_registry_create();
    fr_task_plugin alpha = { "daukle.task/publish:alpha", NULL, NULL, 0, never_runs, NULL };
    fr_task_plugin beta = { "daukle.task/publish:beta", NULL, NULL, 0, never_runs, NULL };
    fr_task_plugin one = { "daukle.task/gradle:one", NULL, NULL, 0, never_runs, NULL };
    fr_task_plugin two = { "daukle.task/gradle:two", NULL, NULL, 0, never_runs, NULL };
    fr_error err;
    fr_registry_add_task(registry, &alpha, &err);
    fr_registry_add_task(registry, &beta, &err);
    fr_registry_add_task(registry, &one, &err);
    fr_registry_add_task(registry, &two, &err);

    fr_manifest manifest = manifest_of(
        "{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{},"
        "\"toolchains\":{\"gradle\":\"8.10\"},"
        "\"publish\":{\"alpha\":{\"from\":\"gradle\"},\"beta\":{\"from\":\"gradle\"}},"
        "\"tasks\":{\"publish:beta\":{\"dependsOn\":[\"gradle:one\"]},"
        "\"gradle:one\":{\"dependsOn\":[\"gradle:two\"]},"
        "\"gradle:two\":{\"dependsOn\":[\"gradle:one\"]}}}");
    fr_task_set set;
    ASSERT_EQ(FR_OK, fr_tasks_collect(registry, &manifest, &set, &err));

    int both = fr_tasks_check_publish(&set, &manifest, NULL, &err);
    static char message[256];
    snprintf(message, sizeof message, "%s", both == FR_OK ? "(accepted)" : err.message);
    int alpha_alone = fr_tasks_check_publish(&set, &manifest, "alpha", &err);
    static char alpha_message[256];
    snprintf(alpha_message, sizeof alpha_message, "%s",
             alpha_alone == FR_OK ? "" : err.message);

    fr_tasks_set_free(&set);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);

    ASSERT_EQm(message, FR_ERR, both);
    ASSERTm(message, strstr(message, "depend on each other in a cycle") != NULL);
    /* Without this the case could pass for a reason true of alpha as well, and
       would then be a test of two destinations in name only. */
    ASSERT_EQm(alpha_message, FR_OK, alpha_alone);
    PASS();
}

TEST two_destinations_that_both_plan_pass_the_check(void) {
    fr_registry *registry = fr_registry_create();
    fr_task_plugin alpha = { "daukle.task/publish:alpha", NULL, NULL, 0, never_runs, NULL };
    fr_task_plugin beta = { "daukle.task/publish:beta", NULL, NULL, 0, never_runs, NULL };
    fr_task_plugin build = { "daukle.task/gradle:build", NULL, NULL, 0, never_runs, NULL };
    fr_error err;
    fr_registry_add_task(registry, &alpha, &err);
    fr_registry_add_task(registry, &beta, &err);
    fr_registry_add_task(registry, &build, &err);

    fr_manifest manifest = manifest_of(
        "{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{},"
        "\"toolchains\":{\"gradle\":\"8.10\"},"
        "\"publish\":{\"alpha\":{\"from\":\"gradle\"},\"beta\":{\"from\":\"gradle\"}},"
        "\"tasks\":{\"publish:alpha\":{\"dependsOn\":[\"gradle:build\"]},"
        "\"publish:beta\":{\"dependsOn\":[\"gradle:build\"]}}}");
    fr_task_set set;
    ASSERT_EQ(FR_OK, fr_tasks_collect(registry, &manifest, &set, &err));

    int status = fr_tasks_check_publish(&set, &manifest, NULL, &err);
    static char message[256];
    snprintf(message, sizeof message, "%s", status == FR_OK ? "" : err.message);

    fr_tasks_set_free(&set);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);

    ASSERT_EQm(message, FR_OK, status);
    PASS();
}

/* The plan every escape-hatch test below runs: collect, plan the one goal,
   and run it against a session rooted at the scratch directory. */
static int run_goal(fr_registry *registry, fr_manifest *manifest, const char *goal,
                    fr_error *err) {
    fr_task_set set;
    if (fr_tasks_collect(registry, manifest, &set, err) != FR_OK) return FR_ERR;

    fr_task_plan plan;
    if (fr_tasks_plan(&set, goal, &plan, err) != FR_OK) {
        fr_tasks_set_free(&set);
        return FR_ERR;
    }

    fr_session session;
    memset(&session, 0, sizeof session);
    session.registry = registry;
    session.manifest = *manifest;
    session.manifest_dir = scratch;

    int status = fr_tasks_run(&plan, &session, err);
    fr_tasks_plan_free(&plan);
    fr_tasks_set_free(&set);
    return status;
}

/* One argument holds a space and one holds a quote, and each is asserted to
   have arrived whole: an argument a shell would have split or unquoted is the
   only thing that tells "argv" and "a command string" apart. */
TEST a_manifest_task_runs_the_program_it_names(void) {
    make_scratch("hatch");
    fr_test_set_env("DAUKLE_TOKEN", "secret-daukle");
    fr_test_set_env("GITHUB_TOKEN", "secret-github");
    fr_test_set_env("DAUKLE_TEST_ORDINARY", "inherited");

    fr_registry *registry = fr_registry_create();
    fr_manifest manifest = manifest_of(
        "{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{},"
        "\"tasks\":{\"hatch\":{\"run\":{\"tool\":\"test_tasks\",\"args\":"
        "[\"--task-child\",\"out.txt\",\"0\",\"one two\",\"a\\\"b\"]}}}}");

    fr_error err;
    int status = run_goal(registry, &manifest, "hatch", &err);
    char output[512];
    int read = read_child_output("out.txt", output, sizeof output);

    fr_test_set_env("DAUKLE_TOKEN", NULL);
    fr_test_set_env("GITHUB_TOKEN", NULL);
    fr_test_set_env("DAUKLE_TEST_ORDINARY", NULL);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_test_remove_tree(scratch);

    ASSERT_EQ_FMT(FR_OK, status, "%d");
    ASSERT(read);
    ASSERT(strstr(output, "[one two]\n") != NULL);
    ASSERT(strstr(output, "[a\"b]\n") != NULL);
    ASSERT(strstr(output, "DAUKLE_TOKEN=<unset>\n") != NULL);
    ASSERT(strstr(output, "GITHUB_TOKEN=<unset>\n") != NULL);
    /* Without this the two above would also pass on a child that inherited
       no environment at all, which is not what daukle promises. */
    ASSERT(strstr(output, "DAUKLE_TEST_ORDINARY=inherited\n") != NULL);
    ASSERT(strstr(output, "prior=no\n") != NULL);
    PASS();
}

TEST a_failing_program_stops_the_plan_and_leaves_what_ran_run(void) {
    make_scratch("hatchfail");
    char log[128];
    log[0] = '\0';

    fr_registry *registry = fr_registry_create();
    fr_task_plugin one = { "daukle.task/a:one", NULL, NULL, 0, counting_run, log };
    fr_error err;
    fr_registry_add_task(registry, &one, &err);

    fr_manifest manifest = manifest_of(
        "{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{},"
        "\"toolchains\":{\"a\":\">=1.0\"},"
        "\"tasks\":{\"hatch\":{\"dependsOn\":[\"a:one\"],\"run\":{\"tool\":\"test_tasks\","
        "\"args\":[\"--task-child\",\"out.txt\",\"3\"]}}}}");

    int status = run_goal(registry, &manifest, "hatch", &err);
    char message[sizeof err.message];
    snprintf(message, sizeof message, "%s", err.message);

    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_test_remove_tree(scratch);

    ASSERT_EQ_FMT(FR_ERR, status, "%d");
    ASSERT(strstr(message, "task \"hatch\"") != NULL);
    ASSERT(strstr(message, "exited with code 3") != NULL);
    ASSERT_STR_EQ("a:one;", log);
    PASS();
}

/* The child reports whether the marker the plugin task writes was already
   there, so this fails if the two merely both ran. */
TEST depends_on_orders_a_manifest_run_after_a_plugin_task(void) {
    make_scratch("hatchorder");

    fr_registry *registry = fr_registry_create();
    fr_task_plugin one = { "daukle.task/a:one", NULL, NULL, 0, marker_run, NULL };
    fr_error err;
    fr_registry_add_task(registry, &one, &err);

    fr_manifest manifest = manifest_of(
        "{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{},"
        "\"toolchains\":{\"a\":\">=1.0\"},"
        "\"tasks\":{\"hatch\":{\"dependsOn\":[\"a:one\"],\"run\":{\"tool\":\"test_tasks\","
        "\"args\":[\"--task-child\",\"out.txt\",\"0\"]}}}}");

    int status = run_goal(registry, &manifest, "hatch", &err);
    char output[512];
    int read = read_child_output("out.txt", output, sizeof output);

    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_test_remove_tree(scratch);

    ASSERT_EQ_FMT(FR_OK, status, "%d");
    ASSERT(read);
    ASSERT(strstr(output, "prior=yes\n") != NULL);
    PASS();
}

/* The escaping half aims at a sibling that EXISTS, so an implementation with
   no containment at all would succeed rather than fail for the unrelated
   reason that the directory is not there. */
TEST a_run_cwd_is_relative_to_the_project_and_cannot_leave_it(void) {
    make_scratch("hatchcwd");
    char inside[600];
    snprintf(inside, sizeof inside, "%s/sub", scratch);
    fr_test_make_directory(inside);

    char outside_name[128];
    snprintf(outside_name, sizeof outside_name, "daukle_test_tasks_outside_%d",
             fr_test_process_id());
    char outside[600];
    snprintf(outside, sizeof outside, "%s/%s", fr_test_temp_base(), outside_name);
    fr_test_remove_tree(outside);
    fr_test_make_directory(outside);

    char text[700];
    snprintf(text, sizeof text,
             "{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{},"
             "\"tasks\":{\"hatch\":{\"run\":{\"tool\":\"test_tasks\",\"cwd\":\"sub\","
             "\"args\":[\"--task-child\",\"out.txt\",\"0\"]}},"
             "\"escape\":{\"run\":{\"tool\":\"test_tasks\",\"cwd\":\"../%s\","
             "\"args\":[\"--task-child\",\"out.txt\",\"0\"]}}}}", outside_name);

    fr_registry *registry = fr_registry_create();
    fr_manifest manifest = manifest_of(text);

    fr_error err;
    int status = run_goal(registry, &manifest, "hatch", &err);
    char in_subdirectory[600];
    snprintf(in_subdirectory, sizeof in_subdirectory, "%s/out.txt", inside);
    char at_root[600];
    snprintf(at_root, sizeof at_root, "%s/out.txt", scratch);
    int landed_in_subdirectory = path_exists(in_subdirectory);
    int landed_at_root = path_exists(at_root);

    fr_error escape_err;
    int escape_status = run_goal(registry, &manifest, "escape", &escape_err);
    char escape_message[sizeof escape_err.message];
    snprintf(escape_message, sizeof escape_message, "%s", escape_err.message);
    char outside_output[600];
    snprintf(outside_output, sizeof outside_output, "%s/out.txt", outside);
    int landed_outside = path_exists(outside_output);

    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_test_remove_tree(scratch);
    fr_test_remove_tree(outside);

    ASSERT_EQ_FMT(FR_OK, status, "%d");
    ASSERT(landed_in_subdirectory);
    ASSERT_FALSE(landed_at_root);
    ASSERT_EQ_FMT(FR_ERR, escape_status, "%d");
    ASSERT(strstr(escape_message, "outside the project directory") != NULL);
    ASSERT_FALSE(landed_outside);
    PASS();
}

/* A manifest block naming a declared task adds edges to it and creates no
   second node, so a run here would be read, stored and never reached. */
TEST a_manifest_block_adding_edges_may_not_also_carry_a_run(void) {
    fr_registry *registry = fr_registry_create();
    fr_task_plugin one = { "daukle.task/a:one", NULL, NULL, 0, never_runs, NULL };
    fr_error err;
    fr_registry_add_task(registry, &one, &err);

    fr_manifest manifest = manifest_of(
        "{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{},"
        "\"toolchains\":{\"a\":\">=1.0\"},"
        "\"tasks\":{\"a:one\":{\"run\":{\"tool\":\"git\"}}}}");

    fr_task_set set;
    ASSERT_EQ(FR_ERR, fr_tasks_collect(registry, &manifest, &set, &err));
    ASSERT(strstr(err.message, "may not carry a run") != NULL);

    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    if (argc >= 4 && strcmp(argv[1], "--task-child") == 0) return run_as_task_child(argc, argv);
    fr_test_prepend_to_path_dir_of(argv[0]);
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
    RUN_TEST(a_publish_task_takes_its_toolchain_from_its_destination);
    RUN_TEST(a_publish_task_with_no_destination_declared_is_skipped);
    RUN_TEST(an_ordinary_task_carries_no_publish_target);
    RUN_TEST(a_task_named_publish_is_refused);
    RUN_TEST(a_publish_task_run_receives_its_destination);
    RUN_TEST(a_manifest_orders_a_publish_step_after_a_build);
    RUN_TEST(a_destination_with_no_publisher_is_named);
    RUN_TEST(a_destination_with_a_publisher_passes_the_check);
    RUN_TEST(a_destination_whose_plan_cannot_be_built_is_refused);
    RUN_TEST(a_later_destinations_broken_plan_is_refused_before_any_of_them_runs);
    RUN_TEST(two_destinations_that_both_plan_pass_the_check);
    RUN_TEST(a_manifest_task_runs_the_program_it_names);
    RUN_TEST(a_failing_program_stops_the_plan_and_leaves_what_ran_run);
    RUN_TEST(depends_on_orders_a_manifest_run_after_a_plugin_task);
    RUN_TEST(a_run_cwd_is_relative_to_the_project_and_cannot_leave_it);
    RUN_TEST(a_manifest_block_adding_edges_may_not_also_carry_a_run);
    GREATEST_MAIN_END();
}
