#include "greatest.h"

#include "migrate/merge.h"
#include "migrate/result.h"
#include "support.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cJSON *parse(const char *text) {
    return cJSON_Parse(text);
}

static const cJSON *at(const cJSON *object, const char *key) {
    return cJSON_GetObjectItemCaseSensitive(object, key);
}

static enum greatest_test_res merge_fragment(cJSON *into, const char *json, const char *source,
                                             cJSON *owner_of, fr_blockers *blockers) {
    cJSON *fragment = parse(json);
    ASSERT(fragment != NULL);
    fr_error err;
    ASSERT_EQ(FR_OK, fr_migrate_merge(into, fragment, source, owner_of, blockers, &err));
    cJSON_Delete(fragment);
    PASS();
}

TEST disjoint_fragments_merge(void) {
    cJSON *into = cJSON_CreateObject();
    cJSON *owner_of = cJSON_CreateObject();
    fr_blockers blockers = { 0 };
    CHECK_CALL(merge_fragment(into, "{\"toolchains\":{\"a\":{\"v\":\"1\"}}}", "migrator a",
                              owner_of, &blockers));
    CHECK_CALL(merge_fragment(into, "{\"toolchains\":{\"b\":{\"v\":\"2\"}}}", "migrator b",
                              owner_of, &blockers));
    const cJSON *toolchains = at(into, "toolchains");
    ASSERT_STR_EQ("1", at(at(toolchains, "a"), "v")->valuestring);
    ASSERT_STR_EQ("2", at(at(toolchains, "b"), "v")->valuestring);
    ASSERT_STR_EQ("migrator a", at(owner_of, "toolchains.a.v")->valuestring);
    ASSERT_STR_EQ("migrator b", at(owner_of, "toolchains.b")->valuestring);
    ASSERT_EQ(0, blockers.count);
    fr_blockers_free(&blockers);
    cJSON_Delete(owner_of);
    cJSON_Delete(into);
    PASS();
}

TEST an_equal_value_set_twice_is_fine(void) {
    cJSON *into = cJSON_CreateObject();
    cJSON *owner_of = cJSON_CreateObject();
    fr_blockers blockers = { 0 };
    CHECK_CALL(merge_fragment(into, "{\"version\":\"2.0.0\"}", "migrator a", owner_of, &blockers));
    CHECK_CALL(merge_fragment(into, "{\"version\":\"2.0.0\"}", "migrator b", owner_of, &blockers));
    ASSERT_EQ(0, blockers.count);
    ASSERT_STR_EQ("2.0.0", at(into, "version")->valuestring);
    fr_blockers_free(&blockers);
    cJSON_Delete(owner_of);
    cJSON_Delete(into);
    PASS();
}

TEST a_scalar_set_twice_differently_is_a_blocker(void) {
    cJSON *into = cJSON_CreateObject();
    cJSON *owner_of = cJSON_CreateObject();
    fr_blockers blockers = { 0 };
    CHECK_CALL(merge_fragment(into, "{\"version\":\"1.0.0\"}", "migrator a", owner_of, &blockers));
    CHECK_CALL(merge_fragment(into, "{\"version\":\"2.0.0\"}", "migrator b", owner_of, &blockers));
    ASSERT_EQ(1, blockers.count);
    ASSERT_STR_EQ("version", blockers.items[0].where);
    ASSERT_STR_EQ("core", blockers.items[0].source);
    ASSERT_STR_EQ("migrator a sets it to \"1.0.0\" and migrator b to \"2.0.0\"",
                  blockers.items[0].why);
    ASSERT_STR_EQ("1.0.0", at(into, "version")->valuestring);
    fr_blockers_free(&blockers);
    cJSON_Delete(owner_of);
    cJSON_Delete(into);
    PASS();
}

TEST a_nested_collision_names_the_dotted_key(void) {
    cJSON *into = cJSON_CreateObject();
    cJSON *owner_of = cJSON_CreateObject();
    fr_blockers blockers = { 0 };
    CHECK_CALL(merge_fragment(into, "{\"toolchains\":{\"a\":{\"v\":\"1\"}}}", "migrator a",
                              owner_of, &blockers));
    CHECK_CALL(merge_fragment(into, "{\"toolchains\":{\"a\":{\"v\":\"2\",\"w\":\"3\"}}}",
                              "migrator b", owner_of, &blockers));
    ASSERT_EQ(1, blockers.count);
    ASSERT_STR_EQ("toolchains.a.v", blockers.items[0].where);
    ASSERT_STR_EQ("3", at(at(at(into, "toolchains"), "a"), "w")->valuestring);
    ASSERT_STR_EQ("migrator b", at(owner_of, "toolchains.a.w")->valuestring);
    fr_blockers_free(&blockers);
    cJSON_Delete(owner_of);
    cJSON_Delete(into);
    PASS();
}

TEST a_table_against_a_scalar_is_a_blocker(void) {
    cJSON *into = cJSON_CreateObject();
    cJSON *owner_of = cJSON_CreateObject();
    fr_blockers blockers = { 0 };
    CHECK_CALL(merge_fragment(into, "{\"x\":{\"y\":1}}", "migrator a", owner_of, &blockers));
    CHECK_CALL(merge_fragment(into, "{\"x\":3}", "migrator b", owner_of, &blockers));
    ASSERT_EQ(1, blockers.count);
    ASSERT_STR_EQ("x", blockers.items[0].where);
    ASSERT_STR_EQ("migrator a sets it to {\"y\":1} and migrator b to 3", blockers.items[0].why);
    fr_blockers_free(&blockers);
    cJSON_Delete(owner_of);
    cJSON_Delete(into);
    PASS();
}

TEST the_instance_keys_are_written_only_when_needed(void) {
    cJSON *manifest = cJSON_CreateObject();
    fr_error err;
    ASSERT_EQ(FR_OK, fr_migrate_set_instance(manifest, "node", "node", NULL, &err));
    const cJSON *plain = at(at(manifest, "toolchains"), "node");
    ASSERT(cJSON_IsObject(plain));
    ASSERT(at(plain, "toolchain") == NULL);
    ASSERT(at(plain, "dir") == NULL);

    ASSERT_EQ(FR_OK, fr_migrate_set_instance(manifest, "node-web", "node", "web", &err));
    const cJSON *placed = at(at(manifest, "toolchains"), "node-web");
    ASSERT_STR_EQ("node", at(placed, "toolchain")->valuestring);
    ASSERT_STR_EQ("web", at(placed, "dir")->valuestring);

    ASSERT_EQ(FR_OK, fr_migrate_set_instance(manifest, "rooted", "rooted", "", &err));
    ASSERT(at(at(at(manifest, "toolchains"), "rooted"), "dir") == NULL);
    cJSON_Delete(manifest);
    PASS();
}

TEST defaults_fill_only_what_is_absent(void) {
    cJSON *manifest = parse("{\"version\":\"3.1.0\"}");
    fr_error err;
    ASSERT_EQ(FR_OK, fr_migrate_fill_defaults(manifest, "dir-name", &err));
    ASSERT_EQ(1, (int) at(manifest, "schema")->valuedouble);
    ASSERT_STR_EQ("dir-name", at(manifest, "project")->valuestring);
    ASSERT_STR_EQ("3.1.0", at(manifest, "version")->valuestring);
    ASSERT(cJSON_IsObject(at(manifest, "modules")));
    cJSON_Delete(manifest);

    manifest = cJSON_CreateObject();
    ASSERT_EQ(FR_OK, fr_migrate_fill_defaults(manifest, "p", &err));
    ASSERT_STR_EQ("0.1.0", at(manifest, "version")->valuestring);
    cJSON_Delete(manifest);
    PASS();
}

TEST a_plugin_is_recorded_with_its_url_and_hash(void) {
    cJSON *manifest = cJSON_CreateObject();
    fr_error err;
    ASSERT_EQ(FR_OK, fr_migrate_add_plugin(manifest, "node", "https://x.test/n.lua", "ab12", &err));
    const cJSON *entry = at(at(manifest, "plugins"), "node");
    ASSERT_STR_EQ("https://x.test/n.lua", at(entry, "url")->valuestring);
    ASSERT_STR_EQ("ab12", at(entry, "sha256")->valuestring);
    cJSON_Delete(manifest);
    PASS();
}

static const char *PROOF = "\"proof\":{\"task\":\"t\",\"compare\":"
                           "[{\"old\":\"a\",\"new\":\"b\",\"as\":\"bytes\"}]}";

static enum greatest_test_res run_check(const char *json, int expected_status,
                                        fr_blockers *blockers, fr_error *err) {
    cJSON *result = parse(json);
    ASSERT(result != NULL);
    ASSERT_EQ(expected_status, fr_migrate_check_run_result(result, "gradle", blockers, err));
    cJSON_Delete(result);
    PASS();
}

TEST a_well_formed_run_result_is_accepted_and_its_blockers_recorded(void) {
    char json[1024];
    snprintf(json, sizeof json,
             "{\"manifest\":{\"tasks\":{}},\"foreign\":[{\"kind\":\"k\",\"id\":\"i\",\"config\":{}}],"
             "\"blockers\":[{\"construct\":\"task\",\"where\":\"build.gradle\",\"why\":\"w\","
             "\"surface\":\"S.md\"},{\"construct\":\"c\",\"where\":\"x\",\"why\":\"y\"}],"
             "\"owned\":[\"out/a.txt\"],%s}", PROOF);
    fr_blockers blockers = { 0 };
    fr_error err;
    CHECK_CALL(run_check(json, FR_OK, &blockers, &err));
    ASSERT_EQ(2, blockers.count);
    ASSERT_STR_EQ("migrator gradle", blockers.items[0].source);
    ASSERT_STR_EQ("build.gradle", blockers.items[0].where);
    ASSERT_STR_EQ("S.md", blockers.items[0].surface);
    ASSERT(blockers.items[1].surface == NULL);
    fr_blockers_free(&blockers);
    PASS();
}

TEST a_run_result_with_a_plugins_key_is_an_error(void) {
    char json[512];
    snprintf(json, sizeof json, "{\"manifest\":{\"plugins\":{}},%s}", PROOF);
    fr_blockers blockers = { 0 };
    fr_error err;
    CHECK_CALL(run_check(json, FR_ERR, &blockers, &err));
    ASSERT(strstr(err.message, "gradle") != NULL);
    ASSERT(strstr(err.message, "plugins") != NULL);

    snprintf(json, sizeof json, "{\"manifest\":{\"schema\":1},%s}", PROOF);
    CHECK_CALL(run_check(json, FR_ERR, &blockers, &err));
    ASSERT(strstr(err.message, "schema") != NULL);
    fr_blockers_free(&blockers);
    PASS();
}

TEST an_empty_compare_list_is_a_blocker(void) {
    fr_blockers blockers = { 0 };
    fr_error err;
    CHECK_CALL(run_check("{\"proof\":{\"task\":\"t\",\"compare\":[]}}", FR_OK, &blockers, &err));
    ASSERT_EQ(1, blockers.count);
    ASSERT_STR_EQ("migrator gradle", blockers.items[0].source);
    fr_blockers_free(&blockers);
    PASS();
}

TEST lua_empty_tables_arrive_as_objects_and_are_accepted(void) {
    fr_blockers blockers = { 0 };
    fr_error err;
    CHECK_CALL(run_check("{\"foreign\":{},\"blockers\":{},\"owned\":{},"
                         "\"proof\":{\"task\":\"t\",\"compare\":{}}}", FR_OK, &blockers, &err));
    ASSERT_EQ(1, blockers.count);
    fr_blockers_free(&blockers);
    PASS();
}

TEST a_missing_proof_is_a_blocker_and_a_malformed_one_an_error(void) {
    fr_blockers blockers = { 0 };
    fr_error err;
    CHECK_CALL(run_check("{}", FR_OK, &blockers, &err));
    ASSERT_EQ(1, blockers.count);
    fr_blockers_free(&blockers);

    CHECK_CALL(run_check("{\"proof\":{\"compare\":[]}}", FR_ERR, &blockers, &err));
    ASSERT(strstr(err.message, "proof") != NULL);
    PASS();
}

TEST owned_paths_that_escape_are_refused(void) {
    const char *refused[] = { "../x", "/x", "daukle.toml", "daukle.lua", "build/daukle/x",
                              "build/daukle", "build", "Build", "BUILD/daukle/x", "Daukle.toml", "DAUKLE.LUA", ".","a\\\\b", "", "a/../b" };
    for (size_t index = 0; index < sizeof refused / sizeof *refused; index++) {
        char json[512];
        snprintf(json, sizeof json, "{\"owned\":[\"%s\"],%s}", refused[index], PROOF);
        fr_blockers blockers = { 0 };
        fr_error err;
        cJSON *result = parse(json);
        ASSERTm(refused[index], result != NULL);
        int status = fr_migrate_check_run_result(result, "gradle", &blockers, &err);
        ASSERT_EQm(refused[index], FR_ERR, status);
        ASSERTm("the error must name the plugin", strstr(err.message, "gradle") != NULL);
        cJSON_Delete(result);
        fr_blockers_free(&blockers);
    }
    PASS();
}

TEST the_derived_directory_refusal_names_both_cases(void) {
    char json[512];
    snprintf(json, sizeof json, "{\"owned\":[\"build\"],%s}", PROOF);
    fr_blockers blockers = { 0 };
    fr_error err;
    CHECK_CALL(run_check(json, FR_ERR, &blockers, &err));
    ASSERT(strstr(err.message, "\".\"") != NULL);
    ASSERT(strstr(err.message, "build/daukle") != NULL);
    PASS();
}

TEST owned_paths_next_to_the_derived_directory_are_accepted(void) {
    const char *accepted[] = { "build/other", "build/daukle2", "out/a.txt", "buildx" };
    for (size_t index = 0; index < sizeof accepted / sizeof *accepted; index++) {
        char json[512];
        snprintf(json, sizeof json, "{\"owned\":[\"%s\"],%s}", accepted[index], PROOF);
        fr_blockers blockers = { 0 };
        fr_error err;
        CHECK_CALL(run_check(json, FR_OK, &blockers, &err));
        fr_blockers_free(&blockers);
    }
    PASS();
}

TEST malformed_list_entries_are_errors_naming_the_field(void) {
    const char *bad[] = {
        "{\"foreign\":[{\"id\":\"i\"}]}", "{\"foreign\":\"x\"}",
        "{\"blockers\":[{\"construct\":\"c\",\"where\":\"w\"}]}", "{\"blockers\":3}",
        "{\"owned\":[3]}", "{\"manifest\":[]}", "[]",
        "{\"proof\":{\"task\":\"t\",\"compare\":[{\"old\":\"a\",\"new\":\"b\",\"as\":\"zip\"}]}}",
        "{\"proof\":{\"task\":\"t\",\"compare\":[{\"old\":\"../a\",\"new\":\"b\",\"as\":\"bytes\"}]}}",
        "{\"proof\":{\"task\":\"t\",\"compare\":[{\"old\":\"a\",\"new\":\"/b\",\"as\":\"bytes\"}]}}",
        "{\"proof\":{\"task\":\"t\",\"compare\":[{\"old\":\"a\",\"new\":\"b\"}]}}",
    };
    for (size_t index = 0; index < sizeof bad / sizeof *bad; index++) {
        fr_blockers blockers = { 0 };
        fr_error err;
        cJSON *result = parse(bad[index]);
        ASSERTm(bad[index], result != NULL);
        ASSERT_EQm(bad[index], FR_ERR, fr_migrate_check_run_result(result, "gradle", &blockers, &err));
        ASSERTm(bad[index], strstr(err.message, "gradle") != NULL);
        cJSON_Delete(result);
        fr_blockers_free(&blockers);
    }
    PASS();
}

TEST a_compare_entry_naming_one_file_twice_is_an_error(void) {
    const char *same[][2] = { { "dist/app.js", "dist/app.js" }, { "dist/App.js", "DIST/app.JS" } };
    for (size_t index = 0; index < sizeof same / sizeof *same; index++) {
        char json[512];
        snprintf(json, sizeof json,
                 "{\"proof\":{\"task\":\"t\",\"compare\":[{\"old\":\"%s\",\"new\":\"%s\","
                 "\"as\":\"bytes\"}]}}", same[index][0], same[index][1]);
        fr_blockers blockers = { 0 };
        static fr_error err;
        CHECK_CALL(run_check(json, FR_ERR, &blockers, &err));
        ASSERTm(err.message, strstr(err.message, "gradle") != NULL);
        ASSERTm(err.message, strstr(err.message, same[index][0]) != NULL);
        ASSERTm(err.message, strstr(err.message, "the same file") != NULL);
        fr_blockers_free(&blockers);
    }
    fr_blockers blockers = { 0 };
    fr_error err;
    CHECK_CALL(run_check("{\"proof\":{\"task\":\"t\",\"compare\":[{\"old\":\"dist/app.js\","
                         "\"new\":\"dist/app.jsx\",\"as\":\"bytes\"}]}}", FR_OK, &blockers, &err));
    fr_blockers_free(&blockers);
    PASS();
}

TEST a_fragment_setting_an_instance_key_core_owns_is_an_error(void) {
    const char *keys[] = { "toolchain", "dir" };
    for (size_t index = 0; index < sizeof keys / sizeof *keys; index++) {
        char json[512];
        snprintf(json, sizeof json, "{\"manifest\":{\"toolchains\":{\"stub\":{\"%s\":\"x\"}}},%s}",
                 keys[index], PROOF);
        fr_blockers blockers = { 0 };
        static fr_error err;
        CHECK_CALL(run_check(json, FR_ERR, &blockers, &err));
        ASSERTm(err.message, strstr(err.message, "gradle") != NULL);
        ASSERTm(err.message, strstr(err.message, keys[index]) != NULL);
        ASSERTm(err.message, strstr(err.message, "toolchains.stub") != NULL);

        cJSON *translation = parse(json);
        ASSERT(translation != NULL);
        ASSERT_EQ(FR_ERR, fr_migrate_check_translation(translation, "translator gradle/dep",
                                                       &blockers, &err));
        ASSERTm(err.message, strstr(err.message, "translator gradle/dep") != NULL);
        cJSON_Delete(translation);
        fr_blockers_free(&blockers);
    }
    char json[512];
    snprintf(json, sizeof json,
             "{\"manifest\":{\"toolchains\":{\"stub\":{\"version\":\"1\"},\"other\":\"2\"}},%s}",
             PROOF);
    fr_blockers blockers = { 0 };
    fr_error err;
    CHECK_CALL(run_check(json, FR_OK, &blockers, &err));
    fr_blockers_free(&blockers);
    PASS();
}

TEST a_translation_records_blockers_and_refuses_core_keys(void) {
    fr_blockers blockers = { 0 };
    fr_error err;
    cJSON *result = parse("{\"manifest\":{\"x\":1},\"blockers\":[{\"construct\":\"c\","
                          "\"where\":\"w\",\"why\":\"y\"}]}");
    ASSERT_EQ(FR_OK, fr_migrate_check_translation(result, "translator gradle/dep", &blockers, &err));
    ASSERT_EQ(1, blockers.count);
    ASSERT_STR_EQ("translator gradle/dep", blockers.items[0].source);
    cJSON_Delete(result);

    result = parse("{\"manifest\":{\"plugins\":{}}}");
    ASSERT_EQ(FR_ERR, fr_migrate_check_translation(result, "translator gradle/dep", &blockers, &err));
    ASSERT(strstr(err.message, "translator gradle/dep") != NULL);
    cJSON_Delete(result);
    fr_blockers_free(&blockers);
    PASS();
}

static char *print_to_text(const fr_blockers *blockers) {
    char path[1024];
    snprintf(path, sizeof path, "%s/migrate_report_%d.txt", fr_test_temp_base(),
             fr_test_process_id());
    FILE *out = fopen(path, "wb");
    if (out == NULL) return NULL;
    fr_blockers_print(blockers, out);
    fclose(out);

    FILE *in = fopen(path, "rb");
    char *text = calloc(4096, 1);
    if (in != NULL && text != NULL) {
        size_t size = fread(text, 1, 4095, in);
        text[size] = '\0';
    }
    if (in != NULL) fclose(in);
    remove(path);
    return text;
}

TEST the_report_groups_blockers_by_source(void) {
    fr_blockers blockers = { 0 };
    fr_error err;
    ASSERT_EQ(FR_OK, fr_blockers_add(&blockers, "migrator gradle", "task \"gen\"", "build.gradle",
                                     "a doLast body", "GRADLE-SURFACE.md \"doLast\"", &err));
    ASSERT_EQ(FR_OK, fr_blockers_add(&blockers, "core", "com.example.x", "gradle plugins",
                                     "no catalog names a replacement", NULL, &err));
    ASSERT_EQ(FR_OK, fr_blockers_add(&blockers, "migrator gradle", "other", "settings.gradle",
                                     "why", NULL, &err));
    char *text = print_to_text(&blockers);
    ASSERT(text != NULL);
    const char *expected =
        "daukle migrate: nothing was written, because 3 thing(s) cannot be expressed yet:\n"
        "  migrator gradle:\n"
        "    build.gradle: task \"gen\": a doLast body (surface: GRADLE-SURFACE.md \"doLast\")\n"
        "    settings.gradle: other: why (surface: no row yet)\n"
        "  core:\n"
        "    gradle plugins: com.example.x: no catalog names a replacement (surface: no row yet)\n";
    ASSERT_STR_EQ(expected, text);
    free(text);
    fr_blockers_free(&blockers);
    PASS();
}

SUITE(migrate_merge) {
    RUN_TEST(disjoint_fragments_merge);
    RUN_TEST(an_equal_value_set_twice_is_fine);
    RUN_TEST(a_scalar_set_twice_differently_is_a_blocker);
    RUN_TEST(a_nested_collision_names_the_dotted_key);
    RUN_TEST(a_table_against_a_scalar_is_a_blocker);
    RUN_TEST(the_instance_keys_are_written_only_when_needed);
    RUN_TEST(defaults_fill_only_what_is_absent);
    RUN_TEST(a_plugin_is_recorded_with_its_url_and_hash);
    RUN_TEST(a_well_formed_run_result_is_accepted_and_its_blockers_recorded);
    RUN_TEST(a_run_result_with_a_plugins_key_is_an_error);
    RUN_TEST(an_empty_compare_list_is_a_blocker);
    RUN_TEST(lua_empty_tables_arrive_as_objects_and_are_accepted);
    RUN_TEST(a_missing_proof_is_a_blocker_and_a_malformed_one_an_error);
    RUN_TEST(owned_paths_that_escape_are_refused);
    RUN_TEST(the_derived_directory_refusal_names_both_cases);
    RUN_TEST(owned_paths_next_to_the_derived_directory_are_accepted);
    RUN_TEST(malformed_list_entries_are_errors_naming_the_field);
    RUN_TEST(a_compare_entry_naming_one_file_twice_is_an_error);
    RUN_TEST(a_fragment_setting_an_instance_key_core_owns_is_an_error);
    RUN_TEST(a_translation_records_blockers_and_refuses_core_keys);
    RUN_TEST(the_report_groups_blockers_by_source);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(migrate_merge);
    GREATEST_MAIN_END();
}
