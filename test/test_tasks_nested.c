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

static char scratch[512];
static char log_text[256];
static char nested_message[512];

static void make_scratch(const char *label) {
    snprintf(scratch, sizeof scratch, "%s/daukle_test_tasks_nested_%s_%d", fr_test_temp_base(), label,
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

static int logging_run(void *state, const fr_task_run_context *context, fr_error *err) {
    (void) state; (void) err;
    strncat(log_text, context->name, 64);
    strncat(log_text, ";", 2);
    return FR_OK;
}

static int failing_run(void *state, const fr_task_run_context *context, fr_error *err) {
    (void) state; (void) context;
    fr_error_set(err, "the compiler said no");
    return FR_ERR;
}

/* Runs the task its state names through the registered runner, as daukle.run_task does, keeping
   a failure's message as a value rather than failing itself. */
static int nesting_run(void *state, const fr_task_run_context *context, fr_error *err) {
    (void) err;
    strncat(log_text, context->name, 64);
    strncat(log_text, "<", 2);
    fr_error nested = { { 0 } };
    fr_lua_task_runner runner = fr_lua_task_runner_in_progress();
    if (runner == NULL) {
        snprintf(nested_message, sizeof nested_message, "no runner");
    } else if (runner(state, &nested) != FR_OK) {
        snprintf(nested_message, sizeof nested_message, "%s", nested.message);
    }
    strncat(log_text, ">", 2);
    return FR_OK;
}

typedef struct {
    fr_registry *registry;
    fr_manifest manifest;
    fr_task_set set;
    fr_task_plan plan;
    fr_session session;
} nested_fixture;

static nested_fixture new_fixture(void) {
    nested_fixture fixture;
    memset(&fixture, 0, sizeof fixture);
    fixture.registry = fr_registry_create();
    return fixture;
}

static int run_goal(nested_fixture *fixture, const char *goal, fr_error *err) {
    fixture->manifest = manifest_of(
        "{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{},"
        "\"toolchains\":{\"a\":\">=1.0\"}}");
    if (fr_tasks_collect(fixture->registry, &fixture->manifest, &fixture->set, err) != FR_OK) return FR_ERR;
    if (fr_tasks_plan(&fixture->set, goal, &fixture->plan, err) != FR_OK) return FR_ERR;
    memset(&fixture->session, 0, sizeof fixture->session);
    fixture->session.registry = fixture->registry;
    fixture->session.manifest = fixture->manifest;
    fixture->session.manifest_dir = scratch;
    return fr_tasks_run(&fixture->set, &fixture->plan, &fixture->session, err);
}

static void free_fixture(nested_fixture *fixture) {
    fr_tasks_plan_free(&fixture->plan);
    fr_tasks_set_free(&fixture->set);
    fr_manifest_free(&fixture->manifest);
    fr_registry_destroy(fixture->registry);
    fr_test_remove_tree(scratch);
}

static void begin(const char *label) {
    make_scratch(label);
    log_text[0] = '\0';
    nested_message[0] = '\0';
}

TEST a_running_task_runs_another_with_what_it_depends_on(void) {
    begin("runs");
    nested_fixture fixture = new_fixture();
    const char *needs_one[] = { "a:one" };
    fr_task_plugin one = { "daukle.task/a:one", NULL, NULL, 0, logging_run, NULL };
    fr_task_plugin two = { "daukle.task/a:two", NULL, needs_one, 1, logging_run, NULL };
    fr_task_plugin outer = { "daukle.task/a:outer", NULL, NULL, 0, nesting_run, "a:two" };
    fr_error err;
    fr_registry_add_task(fixture.registry, &one, &err);
    fr_registry_add_task(fixture.registry, &two, &err);
    fr_registry_add_task(fixture.registry, &outer, &err);

    ASSERT_EQ(FR_OK, run_goal(&fixture, "a:outer", &err));
    ASSERT_STR_EQ("a:outer<a:one;a:two;>", log_text);
    ASSERT_STR_EQ("", nested_message);
    free_fixture(&fixture);
    PASS();
}

TEST a_task_the_nested_run_completed_is_not_run_again(void) {
    begin("once");
    nested_fixture fixture = new_fixture();
    const char *needs_both[] = { "a:outer", "a:two" };
    fr_task_plugin two = { "daukle.task/a:two", NULL, NULL, 0, logging_run, NULL };
    fr_task_plugin outer = { "daukle.task/a:outer", NULL, NULL, 0, nesting_run, "a:two" };
    fr_task_plugin last = { "daukle.task/a:last", NULL, needs_both, 2, logging_run, NULL };
    fr_error err;
    fr_registry_add_task(fixture.registry, &two, &err);
    fr_registry_add_task(fixture.registry, &outer, &err);
    fr_registry_add_task(fixture.registry, &last, &err);

    ASSERT_EQ(FR_OK, run_goal(&fixture, "a:last", &err));
    ASSERT_STR_EQ("a:outer<a:two;>a:last;", log_text);
    free_fixture(&fixture);
    PASS();
}

TEST a_failing_nested_task_is_reported_to_the_task_that_ran_it(void) {
    begin("fails");
    nested_fixture fixture = new_fixture();
    fr_task_plugin bad = { "daukle.task/a:bad", NULL, NULL, 0, failing_run, NULL };
    fr_task_plugin outer = { "daukle.task/a:outer", NULL, NULL, 0, nesting_run, "a:bad" };
    fr_error err;
    fr_registry_add_task(fixture.registry, &bad, &err);
    fr_registry_add_task(fixture.registry, &outer, &err);

    ASSERT_EQ(FR_OK, run_goal(&fixture, "a:outer", &err));
    ASSERT(strstr(nested_message, "task \"a:bad\"") != NULL);
    ASSERT(strstr(nested_message, "the compiler said no") != NULL);
    free_fixture(&fixture);
    PASS();
}

TEST a_task_may_not_run_itself_from_inside_itself(void) {
    begin("itself");
    nested_fixture fixture = new_fixture();
    fr_task_plugin outer = { "daukle.task/a:outer", NULL, NULL, 0, nesting_run, "a:outer" };
    fr_error err;
    fr_registry_add_task(fixture.registry, &outer, &err);

    ASSERT_EQ(FR_OK, run_goal(&fixture, "a:outer", &err));
    ASSERT_STR_EQ("a:outer<>", log_text);
    ASSERT(strstr(nested_message, "\"a:outer\" is already running") != NULL);
    free_fixture(&fixture);
    PASS();
}

TEST a_nested_goal_nothing_declares_is_refused(void) {
    begin("unknown");
    nested_fixture fixture = new_fixture();
    fr_task_plugin outer = { "daukle.task/a:outer", NULL, NULL, 0, nesting_run, "a:missing" };
    fr_error err;
    fr_registry_add_task(fixture.registry, &outer, &err);

    ASSERT_EQ(FR_OK, run_goal(&fixture, "a:outer", &err));
    ASSERT(strstr(nested_message, "no task \"a:missing\" is declared") != NULL);
    free_fixture(&fixture);
    PASS();
}

TEST no_runner_is_offered_once_the_run_ends(void) {
    begin("ends");
    nested_fixture fixture = new_fixture();
    fr_task_plugin one = { "daukle.task/a:one", NULL, NULL, 0, logging_run, NULL };
    fr_error err;
    fr_registry_add_task(fixture.registry, &one, &err);

    ASSERT_EQ(FR_OK, run_goal(&fixture, "a:one", &err));
    ASSERT(fr_lua_task_runner_in_progress() == NULL);
    free_fixture(&fixture);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(a_running_task_runs_another_with_what_it_depends_on);
    RUN_TEST(a_task_the_nested_run_completed_is_not_run_again);
    RUN_TEST(a_failing_nested_task_is_reported_to_the_task_that_ran_it);
    RUN_TEST(a_task_may_not_run_itself_from_inside_itself);
    RUN_TEST(a_nested_goal_nothing_declares_is_refused);
    RUN_TEST(no_runner_is_offered_once_the_run_ends);
    GREATEST_MAIN_END();
}
