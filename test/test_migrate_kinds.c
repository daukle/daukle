#include "greatest.h"

#include "config/config.h"
#include "config/config_dispatch_migrate.h"
#include "config/config_lua.h"
#include "config/manifest.h"
#include "migrate/result.h"
#include "plugin/plugins.h"
#include "plugin/registry.h"
#include "plugin/resolvers.h"
#include "project/region.h"
#include "project/sync.h"
#include "support.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    fr_registry *registry;
    fr_manifest manifest;
    fr_error err;
    int status;
} loaded_fixture;

static void load_fixture(loaded_fixture *fixture, const char *name) {
    char path[256];
    snprintf(path, sizeof path, "test/fixtures/%s/daukle.toml", name);
    memset(fixture, 0, sizeof *fixture);
    fixture->status = fr_build_registry(&fixture->registry, &fixture->err);
    if (fixture->status != FR_OK) return;
    fixture->status = fr_config_load_file(path, fixture->registry, &fixture->manifest, &fixture->err);
}

static void close_fixture(loaded_fixture *fixture) {
    if (fixture->status == FR_OK) fr_manifest_free(&fixture->manifest);
    fr_registry_destroy(fixture->registry);
    fr_lua_runtime_shutdown();
    fr_plugins_report_clear();
    fr_resolvers_clear();
}

static fr_migrate_call call_in(const char *manifest_dir) {
    fr_migrate_call call = { manifest_dir, "faketool", "stub", NULL, "../../..",
                             "build/daukle/stub", NULL };
    return call;
}

static int built_file_text(char **out_text) {
    fr_error err = { { 0 } };
    return fr_file_read_text("test/fixtures/migrate-kinds/build/daukle/stub/built.txt", out_text, &err);
}

TEST a_migrator_registers_under_its_plugin_label(void) {
    loaded_fixture fixture;
    load_fixture(&fixture, "migrate-kinds");
    const fr_migrator_plugin *migrator = fixture.status == FR_OK
        ? fr_registry_migrator_of(fixture.registry, "stub") : NULL;
    int found = migrator != NULL;
    int tool_matches = found && strcmp(migrator->tool, "faketool") == 0;
    int toolchain_matches = found && strcmp(migrator->toolchain, "stub") == 0;
    int capability_matches = found && strcmp(migrator->capability, "daukle.migrator/stub") == 0;
    size_t count = fr_registry_migrator_count(fixture.registry);
    int first_is_it = fr_registry_migrator_at(fixture.registry, 0) == migrator;
    close_fixture(&fixture);

    ASSERT_EQ(FR_OK, fixture.status);
    ASSERT(found);
    ASSERT(tool_matches);
    ASSERT(toolchain_matches);
    ASSERT(capability_matches);
    ASSERT_EQ(1u, count);
    ASSERT(first_is_it);
    PASS();
}

TEST run_returns_its_table_as_json(void) {
    loaded_fixture fixture;
    load_fixture(&fixture, "migrate-kinds");
    const fr_migrator_plugin *migrator = fixture.status == FR_OK
        ? fr_registry_migrator_of(fixture.registry, "stub") : NULL;
    fr_migrate_call call = call_in("test/fixtures/migrate-kinds");
    cJSON *result = NULL;
    fr_error err = { { 0 } };
    int status = migrator != NULL ? fr_lua_dispatch_migrator_run(migrator, &call, &result, &err) : FR_ERR;
    const cJSON *proof = result != NULL ? cJSON_GetObjectItemCaseSensitive(result, "proof") : NULL;
    const cJSON *task = proof != NULL ? cJSON_GetObjectItemCaseSensitive(proof, "task") : NULL;
    int wrote = result != NULL
        && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "wrote"));
    int task_matches = cJSON_IsString(task) && strcmp(task->valuestring, "stub:build") == 0;
    const cJSON *owned = result != NULL ? cJSON_GetObjectItemCaseSensitive(result, "owned") : NULL;
    int owned_is_one = cJSON_IsArray(owned) && cJSON_GetArraySize(owned) == 1;
    char *probe = NULL;
    fr_error read_err;
    int read_status = fr_file_read_text("test/fixtures/migrate-kinds/build/daukle/stub/probe.txt",
                                        &probe, &read_err);
    int probe_matches = read_status == FR_OK && strcmp(probe, "stub ../../..") == 0;
    free(probe);
    cJSON_Delete(result);
    close_fixture(&fixture);

    ASSERT_EQ(FR_OK, fixture.status);
    ASSERT_EQ(FR_OK, status);
    ASSERT(wrote);
    ASSERT(task_matches);
    ASSERT(owned_is_one);
    ASSERT(probe_matches);
    PASS();
}

TEST build_sees_the_proof_run_returned(void) {
    loaded_fixture fixture;
    load_fixture(&fixture, "migrate-kinds");
    const fr_migrator_plugin *migrator = fixture.status == FR_OK
        ? fr_registry_migrator_of(fixture.registry, "stub") : NULL;
    fr_migrate_call call = call_in("test/fixtures/migrate-kinds");
    cJSON *result = NULL;
    fr_error err = { { 0 } };
    int run_status = migrator != NULL ? fr_lua_dispatch_migrator_run(migrator, &call, &result, &err) : FR_ERR;
    call.proof = result != NULL ? cJSON_GetObjectItemCaseSensitive(result, "proof") : NULL;
    int build_status = run_status == FR_OK ? fr_lua_dispatch_migrator_build(migrator, &call, &err) : FR_ERR;
    char *built = NULL;
    int read_status = built_file_text(&built);
    int built_matches = read_status == FR_OK && strcmp(built, "stub:build") == 0;
    free(built);
    cJSON_Delete(result);
    close_fixture(&fixture);

    ASSERT_EQ(FR_OK, fixture.status);
    ASSERT_EQ(FR_OK, run_status);
    ASSERT_EQ(FR_OK, build_status);
    ASSERT(built_matches);
    PASS();
}

TEST a_migrator_without_build_builds_nothing(void) {
    loaded_fixture fixture;
    load_fixture(&fixture, "migrate-no-build");
    const fr_migrator_plugin *migrator = fixture.status == FR_OK
        ? fr_registry_migrator_of(fixture.registry, "stub") : NULL;
    fr_migrate_call call = call_in("test/fixtures/migrate-no-build");
    fr_error err = { { 0 } };
    int status = migrator != NULL ? fr_lua_dispatch_migrator_build(migrator, &call, &err) : FR_ERR;
    char *built = NULL;
    fr_error read_err;
    int wrote_nothing = fr_file_read_text("test/fixtures/migrate-no-build/build/daukle/stub/built.txt",
                                          &built, &read_err) != FR_OK;
    free(built);
    close_fixture(&fixture);

    ASSERT_EQ(FR_OK, fixture.status);
    ASSERT(migrator != NULL);
    ASSERT_EQ(FR_OK, status);
    ASSERT(wrote_nothing);
    PASS();
}

TEST a_translator_is_found_by_tool_and_id_and_maps_data(void) {
    loaded_fixture fixture;
    load_fixture(&fixture, "migrate-kinds");
    const fr_translator_plugin *translator = fixture.status == FR_OK
        ? fr_registry_translator(fixture.registry, "faketool", "org.example.release") : NULL;
    const fr_translator_plugin *other_id = fixture.status == FR_OK
        ? fr_registry_translator(fixture.registry, "faketool", "org.example.other") : NULL;
    cJSON *foreign = cJSON_Parse("{\"tool\":\"faketool\",\"kind\":\"plugins\","
                                 "\"id\":\"org.example.release\",\"instance\":\"stub\","
                                 "\"config\":{\"draft\":true}}");
    cJSON *result = NULL;
    fr_error err = { { 0 } };
    int status = translator != NULL ? fr_lua_dispatch_translator(translator, foreign, &result, &err) : FR_ERR;
    const cJSON *manifest = result != NULL ? cJSON_GetObjectItemCaseSensitive(result, "manifest") : NULL;
    const cJSON *publish = manifest != NULL ? cJSON_GetObjectItemCaseSensitive(manifest, "publish") : NULL;
    const cJSON *release = publish != NULL ? cJSON_GetObjectItemCaseSensitive(publish, "release") : NULL;
    const cJSON *from = release != NULL ? cJSON_GetObjectItemCaseSensitive(release, "from") : NULL;
    const cJSON *draft = release != NULL ? cJSON_GetObjectItemCaseSensitive(release, "draft") : NULL;
    int from_matches = cJSON_IsString(from) && strcmp(from->valuestring, "stub") == 0;
    int draft_is_true = cJSON_IsTrue(draft);
    cJSON_Delete(foreign);
    cJSON_Delete(result);
    close_fixture(&fixture);

    ASSERT_EQ(FR_OK, fixture.status);
    ASSERT(translator != NULL);
    ASSERT(other_id == NULL);
    ASSERT_EQ(FR_OK, status);
    ASSERT(from_matches);
    ASSERT(draft_is_true);
    PASS();
}

TEST a_migrator_run_is_inside_the_exec_window(void) {
    loaded_fixture fixture;
    load_fixture(&fixture, "migrate-execs");
    const fr_migrator_plugin *migrator = fixture.status == FR_OK
        ? fr_registry_migrator_of(fixture.registry, "stub") : NULL;
    fr_migrate_call call = call_in("test/fixtures/migrate-execs");
    cJSON *result = NULL;
    fr_error err = { { 0 } };
    int status = migrator != NULL ? fr_lua_dispatch_migrator_run(migrator, &call, &result, &err) : FR_OK;
    int reached_the_argument_check = strstr(err.message, "must be a tool handle") != NULL;
    int refused_by_window = strstr(err.message, "available only inside") != NULL;
    cJSON_Delete(result);
    close_fixture(&fixture);

    ASSERT_EQ(FR_OK, fixture.status);
    ASSERT(migrator != NULL);
    ASSERT_EQ(FR_ERR, status);
    ASSERT(reached_the_argument_check);
    ASSERT(!refused_by_window);
    PASS();
}

TEST a_translator_may_not_exec(void) {
    loaded_fixture fixture;
    load_fixture(&fixture, "migrate-translator-execs");
    const fr_translator_plugin *translator = fixture.status == FR_OK
        ? fr_registry_translator(fixture.registry, "faketool", "org.example.release") : NULL;
    cJSON *foreign = cJSON_Parse("{\"tool\":\"faketool\",\"kind\":\"plugins\","
                                 "\"id\":\"org.example.release\",\"instance\":\"stub\","
                                 "\"config\":{}}");
    cJSON *result = NULL;
    fr_error err = { { 0 } };
    int status = translator != NULL ? fr_lua_dispatch_translator(translator, foreign, &result, &err) : FR_OK;
    int names_the_window = strstr(err.message, "while a translator translates") != NULL;
    cJSON_Delete(foreign);
    cJSON_Delete(result);
    close_fixture(&fixture);

    ASSERT_EQ(FR_OK, fixture.status);
    ASSERT(translator != NULL);
    ASSERT_EQ(FR_ERR, status);
    ASSERT(names_the_window);
    PASS();
}

TEST a_migrator_needs_its_toolchain_declared_above_it(void) {
    loaded_fixture fixture;
    load_fixture(&fixture, "migrate-no-toolchain");
    int names_the_rule = strstr(fixture.err.message,
        "migrator for \"faketool\" names toolchain \"stub\", which is"
        " not declared above it in this plugin") != NULL;
    close_fixture(&fixture);

    ASSERT_EQ(FR_ERR, fixture.status);
    ASSERT(names_the_rule);
    PASS();
}

#define FILES_PROOF "\"proof\":{\"task\":\"t\",\"compare\":[{\"old\":\"a\",\"new\":\"b\",\"as\":\"bytes\"}]}"

static int files_result_status(const char *files_json, fr_error *err) {
    char json[1024];
    snprintf(json, sizeof json, "{\"manifest\":{\"toolchains\":{\"gradle\":{}}},\"files\":%s,%s}",
             files_json, FILES_PROOF);
    cJSON *result = cJSON_Parse(json);
    fr_blockers blockers = { 0 };
    int status = result != NULL ? fr_migrate_check_run_result(result, "gradle", &blockers, err) : -1;
    cJSON_Delete(result);
    fr_blockers_free(&blockers);
    return status;
}

TEST migrate_files_refuses_a_path_outside_the_instance(void) {
    const char *refused[] = {
        "[{\"path\":\"daukle/other/x\",\"text\":\"t\"}]",
        "[{\"path\":\"../x\",\"text\":\"t\"}]",
        "[{\"path\":\"/abs/x\",\"text\":\"t\"}]",
        "[{\"path\":\"daukle/gradle/a\\\\b\",\"text\":\"t\"}]",
        "[{\"path\":\"daukle/gradle/..\",\"text\":\"t\"}]",
        "[{\"path\":\"daukle/gradle/\",\"text\":\"t\"}]",
        "[{\"path\":\"daukle/gradle\",\"text\":\"t\"}]",
    };
    for (size_t index = 0; index < sizeof refused / sizeof *refused; index++) {
        fr_error err = { { 0 } };
        ASSERT_EQm(refused[index], FR_ERR, files_result_status(refused[index], &err));
        ASSERTm(err.message, strstr(err.message, "files") != NULL);
        ASSERTm(err.message, strstr(err.message, "malformed") != NULL);
    }
    PASS();
}

TEST migrate_files_accepts_a_path_inside_the_instance(void) {
    fr_error err = { { 0 } };
    ASSERT_EQ(FR_OK, files_result_status(
        "[{\"path\":\"daukle/gradle/logic/a.kts\",\"text\":\"t\"}]", &err));
    PASS();
}

TEST migrate_files_refuses_a_non_string_text(void) {
    fr_error err = { { 0 } };
    ASSERT_EQ(FR_ERR, files_result_status("[{\"path\":\"daukle/gradle/x\",\"text\":7}]", &err));
    ASSERT(strstr(err.message, "files") != NULL);
    fr_error missing = { { 0 } };
    ASSERT_EQ(FR_ERR, files_result_status("[{\"path\":\"daukle/gradle/x\"}]", &missing));
    ASSERT(strstr(missing.message, "files") != NULL);
    PASS();
}

SUITE(migrate_kinds) {
    RUN_TEST(a_migrator_registers_under_its_plugin_label);
    RUN_TEST(run_returns_its_table_as_json);
    RUN_TEST(build_sees_the_proof_run_returned);
    RUN_TEST(a_migrator_without_build_builds_nothing);
    RUN_TEST(a_translator_is_found_by_tool_and_id_and_maps_data);
    RUN_TEST(a_migrator_run_is_inside_the_exec_window);
    RUN_TEST(a_translator_may_not_exec);
    RUN_TEST(a_migrator_needs_its_toolchain_declared_above_it);
    RUN_TEST(migrate_files_refuses_a_path_outside_the_instance);
    RUN_TEST(migrate_files_accepts_a_path_inside_the_instance);
    RUN_TEST(migrate_files_refuses_a_non_string_text);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(migrate_kinds);
    fr_test_remove_tree("test/fixtures/migrate-kinds/build");
    fr_test_remove_tree("test/fixtures/migrate-no-build/build");
    fr_test_remove_tree("test/fixtures/migrate-execs/build");
    GREATEST_MAIN_END();
}
