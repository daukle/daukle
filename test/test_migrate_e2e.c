#include "greatest.h"

#include "cache/cache.h"
#include "http_server.h"
#include "migrate/migrate.h"
#include "project/derived.h"
#include "project/region.h"
#include "support.h"
#include "util/error.h"
#include "util/sha256.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

GREATEST_MAIN_DEFS();

#define FIXTURES "test/fixtures/migrate-stub"
#define CATALOG_SIZE 2048

typedef struct {
    char scratch[512];
    char project[640];
    char cache[640];
    char stderr_path[640];
    char original_dir[1024];
    char catalog_url[256];
    char stub_digest[65];
    char report_path[640];
    fr_test_server *server;
    char *printed;
    int status;
    fr_migrate_outcome outcome;
    fr_error err;
} migrate_case;

static void write_bytes(const char *path, const char *bytes) {
    FILE *file = fopen(path, "wb");
    if (file == NULL) return;
    fwrite(bytes, 1, strlen(bytes), file);
    fclose(file);
}

static void copy_fixture_file(const char *fixture, const char *destination) {
    char *bytes = NULL;
    size_t length = 0;
    fr_error err;
    if (fr_file_read_bytes(fixture, &bytes, &length, &err) != FR_OK) return;
    FILE *file = fopen(destination, "wb");
    if (file != NULL) {
        fwrite(bytes, 1, length, file);
        fclose(file);
    }
    free(bytes);
}

static void project_path(const migrate_case *test_case, const char *relative, char *out,
                         size_t size) {
    snprintf(out, size, "%s/%s", test_case->project, relative);
}

static int project_file_exists(const migrate_case *test_case, const char *relative) {
    char path[1024];
    project_path(test_case, relative, path, sizeof path);
    return fr_cache_file_exists(path);
}

static int project_directory_exists(const migrate_case *test_case, const char *relative) {
    char path[1024];
    project_path(test_case, relative, path, sizeof path);
    return fr_cache_directory_exists(path);
}

static char *project_text(const migrate_case *test_case, const char *relative) {
    char path[1024];
    project_path(test_case, relative, path, sizeof path);
    char *text = NULL;
    fr_error err;
    return fr_file_read_text(path, &text, &err) == FR_OK ? text : NULL;
}

/* The old build's artifact is written here rather than committed, because a
   checkout may turn its line ending into CRLF and the stub task writes LF. */
static void open_case(migrate_case *test_case, const char *label, const char *greeting) {
    memset(test_case, 0, sizeof *test_case);
    snprintf(test_case->scratch, sizeof test_case->scratch, "%s/daukle_test_migrate_%s_%d",
             fr_test_temp_base(), label, fr_test_process_id());
    fr_test_remove_tree(test_case->scratch);
    fr_test_make_directory(test_case->scratch);
    snprintf(test_case->project, sizeof test_case->project, "%s/project", test_case->scratch);
    snprintf(test_case->cache, sizeof test_case->cache, "%s/cache", test_case->scratch);
    snprintf(test_case->stderr_path, sizeof test_case->stderr_path, "%s/stderr.txt",
             test_case->scratch);
    fr_test_make_directory(test_case->project);
    fr_test_make_directory(test_case->cache);

    char path[1024];
    project_path(test_case, "faketool.conf", path, sizeof path);
    copy_fixture_file(FIXTURES "/project/faketool.conf", path);
    if (greeting != NULL) {
        project_path(test_case, "out", path, sizeof path);
        fr_test_make_directory(path);
        project_path(test_case, "out/greeting.txt", path, sizeof path);
        write_bytes(path, greeting);
    }

    test_case->server = fr_test_server_create();
    snprintf(test_case->catalog_url, sizeof test_case->catalog_url,
             "http://127.0.0.1:%d/catalog.json", fr_test_server_port(test_case->server));
}

static void serve_plugin(migrate_case *test_case, const char *route, const char *fixture,
                         char digest[65], char *url, size_t url_size) {
    char *bytes = NULL;
    size_t length = 0;
    fr_error err;
    digest[0] = '\0';
    if (fr_file_read_bytes(fixture, &bytes, &length, &err) != FR_OK) return;
    fr_sha256_hex(bytes, length, digest);
    fr_test_server_add_body_bytes(test_case->server, route, bytes, length);
    free(bytes);
    snprintf(url, url_size, "http://127.0.0.1:%d%s", fr_test_server_port(test_case->server), route);
}

static void serve_stub_catalog_rule(migrate_case *test_case, const char *fixture, const char *rule) {
    char url[256];
    serve_plugin(test_case, "/stub.lua", fixture, test_case->stub_digest, url, sizeof url);
    char catalog[CATALOG_SIZE];
    snprintf(catalog, sizeof catalog,
             "{\"plugins\":[{\"id\":\"stub\",\"asset\":{\"url\":\"%s\",\"sha256\":\"%s\"},"
             "\"detect\":[%s]}]}",
             url, test_case->stub_digest, rule);
    fr_test_server_add_body(test_case->server, "/catalog.json", catalog);
}

/* One catalog naming the stub plugin, detected by rule_file at the root. */
static void serve_stub_catalog(migrate_case *test_case, const char *fixture, const char *rule_file) {
    char rule[256];
    snprintf(rule, sizeof rule, "{\"files\":[\"%s\"]}", rule_file);
    serve_stub_catalog_rule(test_case, fixture, rule);
}

static void run_case_with(migrate_case *test_case, const char *const *catalogs,
                          size_t catalog_count) {
    fr_test_server_start(test_case->server);
    fr_test_get_working_directory(test_case->original_dir, sizeof test_case->original_dir);
    fr_test_set_env("DAUKLE_CACHE_DIR", test_case->cache);
    int captured = fr_test_capture_stderr_begin(test_case->stderr_path) == 0;
    test_case->err.message[0] = '\0';
    if (fr_test_set_working_directory(test_case->project)) {
        fr_migrate_options options = { catalogs, catalog_count, NULL, 0, 1,
                                       test_case->report_path[0] != '\0' ? test_case->report_path
                                                                         : NULL };
        test_case->status = fr_migrate_run(&options, &test_case->outcome, &test_case->err);
        fr_test_set_working_directory(test_case->original_dir);
    } else {
        test_case->status = -1;
    }
    if (captured) fr_test_capture_stderr_end();
    fr_test_server_stop(test_case->server);
    fr_error err;
    if (fr_file_read_text(test_case->stderr_path, &test_case->printed, &err) != FR_OK) {
        test_case->printed = NULL;
    }
}

static void run_case(migrate_case *test_case) {
    const char *catalogs[] = { test_case->catalog_url };
    run_case_with(test_case, catalogs, 1);
}

static int printed_holds(const migrate_case *test_case, const char *needle) {
    return test_case->printed != NULL && strstr(test_case->printed, needle) != NULL;
}

static int error_holds(const migrate_case *test_case, const char *needle) {
    return strstr(test_case->err.message, needle) != NULL;
}

/* greatest prints a message after the case returns, so it cannot point into the case. */
static char last_message[2048];

static void close_case(migrate_case *test_case) {
    snprintf(last_message, sizeof last_message, "%s | stderr: %s", test_case->err.message,
             test_case->printed != NULL ? test_case->printed : "");
    fr_test_server_free(test_case->server);
    free(test_case->printed);
    fr_test_remove_tree(test_case->scratch);
}

static int old_files_still_in_place(const migrate_case *test_case) {
    return project_file_exists(test_case, "faketool.conf")
        && project_file_exists(test_case, "out/greeting.txt");
}

TEST a_clean_migration_writes_proves_and_moves_aside(void) {
    migrate_case test_case;
    open_case(&test_case, "clean", "hello");
    serve_stub_catalog(&test_case, FIXTURES "/plugin.lua", "faketool.conf");
    run_case(&test_case);

    char *manifest = project_text(&test_case, "daukle.toml");
    char stub_url[128];
    snprintf(stub_url, sizeof stub_url, "http://127.0.0.1:%d/stub.lua",
             fr_test_server_port(test_case.server));
    int names_plugin = manifest != NULL && strstr(manifest, "[plugins.stub]") != NULL;
    int names_url = manifest != NULL && strstr(manifest, stub_url) != NULL;
    int names_digest = manifest != NULL && test_case.stub_digest[0] != '\0'
                       && strstr(manifest, test_case.stub_digest) != NULL;
    int names_toolchain = manifest != NULL && strstr(manifest, "[toolchains.stub]") != NULL;
    int conf_gone = !project_file_exists(&test_case, "faketool.conf");
    int out_gone = !project_directory_exists(&test_case, "out");
    int conf_kept = project_file_exists(&test_case, "build/daukle/migrate/old/faketool.conf");
    int out_kept = project_file_exists(&test_case, "build/daukle/migrate/old/out/greeting.txt");
    free(manifest);
    int status = test_case.status;
    fr_migrate_outcome outcome = test_case.outcome;
    close_case(&test_case);

    ASSERT_EQm(last_message, FR_OK, status);
    ASSERT_EQ(FR_MIGRATE_DONE, outcome);
    ASSERT(names_plugin);
    ASSERT(names_url);
    ASSERT(names_digest);
    ASSERT(names_toolchain);
    ASSERT(conf_gone);
    ASSERT(out_gone);
    ASSERT(conf_kept);
    ASSERT(out_kept);
    PASS();
}

static int refuses_with_manifests(const char *label, const char *const *files, size_t file_count,
                                  const char *needle) {
    migrate_case test_case;
    open_case(&test_case, label, "hello");
    serve_stub_catalog(&test_case, FIXTURES "/plugin.lua", "faketool.conf");
    for (size_t index = 0; index < file_count; index++) {
        char path[1024];
        project_path(&test_case, files[index], path, sizeof path);
        write_bytes(path, "schema = 1\n");
    }
    run_case(&test_case);
    int refused = test_case.status == FR_ERR && error_holds(&test_case, needle);
    int fetched = fr_test_server_was_requested(test_case.server, "/catalog.json");
    int untouched = old_files_still_in_place(&test_case);
    close_case(&test_case);
    return refused && !fetched && untouched;
}

TEST migrate_refuses_a_project_that_has_a_manifest(void) {
    const char *toml[] = { "daukle.toml" };
    const char *lua[] = { "daukle.lua" };
    const char *both[] = { "daukle.toml", "daukle.lua" };
    ASSERT(refuses_with_manifests("manifest", toml, 1, "already holds"));
    ASSERT(refuses_with_manifests("manifest-toml", toml, 1, "daukle.toml"));
    ASSERT(refuses_with_manifests("manifest-lua", lua, 1, "already holds daukle.lua;"));
    ASSERT(refuses_with_manifests("both", both, 2, "daukle.toml and daukle.lua"));
    PASS();
}

TEST no_catalog_is_refused_naming_both_inputs(void) {
    migrate_case test_case;
    open_case(&test_case, "nocatalog", "hello");
    run_case_with(&test_case, NULL, 0);
    int status = test_case.status;
    int names_flag = error_holds(&test_case, "--catalog");
    int names_variable = error_holds(&test_case, "DAUKLE_CATALOGS");
    int names_wrapper = error_holds(&test_case, "wrapper.toml");
    int names_no_url = !error_holds(&test_case, "http");
    close_case(&test_case);

    ASSERT_EQ(FR_ERR, status);
    ASSERT(names_flag);
    ASSERT(names_variable);
    ASSERT(names_wrapper);
    ASSERT(names_no_url);
    PASS();
}

TEST nothing_detected_says_so_and_writes_nothing(void) {
    migrate_case test_case;
    open_case(&test_case, "nothing", "hello");
    serve_stub_catalog(&test_case, FIXTURES "/plugin.lua", "absent.conf");
    run_case(&test_case);
    int status = test_case.status;
    int says_so = error_holds(&test_case, "no catalog plugin recognised this project");
    int wrote = project_file_exists(&test_case, "daukle.toml");
    close_case(&test_case);

    ASSERT_EQ(FR_ERR, status);
    ASSERT(says_so);
    ASSERT_FALSE(wrote);
    PASS();
}

TEST a_blocker_writes_nothing(void) {
    migrate_case test_case;
    open_case(&test_case, "blocker", "hello");
    serve_stub_catalog(&test_case, FIXTURES "/plugin-blocker.lua", "faketool.conf");
    run_case(&test_case);
    int status = test_case.status;
    fr_migrate_outcome outcome = test_case.outcome;
    int wrote = project_file_exists(&test_case, "daukle.toml");
    int untouched = old_files_still_in_place(&test_case);
    int reported = printed_holds(&test_case, "faketool's magic has no daukle form");
    close_case(&test_case);

    ASSERT_EQm(last_message, FR_OK, status);
    ASSERT_EQ(FR_MIGRATE_BLOCKED, outcome);
    ASSERT_FALSE(wrote);
    ASSERT(untouched);
    ASSERT(reported);
    PASS();
}

TEST an_unclaimed_foreign_plugin_is_a_blocker(void) {
    migrate_case test_case;
    open_case(&test_case, "unclaimed", "hello");
    serve_stub_catalog(&test_case, FIXTURES "/plugin-foreign-unknown.lua", "faketool.conf");
    run_case(&test_case);
    int status = test_case.status;
    fr_migrate_outcome outcome = test_case.outcome;
    int named = printed_holds(&test_case, "org.example.unknown");
    int explained = printed_holds(&test_case, "no catalog names a replacement for it");
    int wrote = project_file_exists(&test_case, "daukle.toml");
    close_case(&test_case);

    ASSERT_EQm(last_message, FR_OK, status);
    ASSERT_EQ(FR_MIGRATE_BLOCKED, outcome);
    ASSERT(named);
    ASSERT(explained);
    ASSERT_FALSE(wrote);
    PASS();
}

/* The stub, detected, and "releaser", detected by nothing and claiming the
   foreign plugin org.example.release in its replaces. */
static void serve_releaser_catalog(migrate_case *test_case, const char *stub_fixture,
                                   const char *releaser_fixture) {
    char stub_url[256];
    char releaser_digest[65];
    char releaser_url[256];
    serve_plugin(test_case, "/stub.lua", stub_fixture, test_case->stub_digest, stub_url,
                 sizeof stub_url);
    serve_plugin(test_case, "/releaser.lua", releaser_fixture, releaser_digest, releaser_url,
                 sizeof releaser_url);
    char catalog[CATALOG_SIZE];
    snprintf(catalog, sizeof catalog,
             "{\"plugins\":["
             "{\"id\":\"stub\",\"asset\":{\"url\":\"%s\",\"sha256\":\"%s\"},"
             "\"detect\":[{\"files\":[\"faketool.conf\"]}]},"
             "{\"id\":\"releaser\",\"asset\":{\"url\":\"%s\",\"sha256\":\"%s\"},"
             "\"replaces\":{\"faketool\":{\"plugins\":[\"org.example.release\"]}}}]}",
             stub_url, test_case->stub_digest, releaser_url, releaser_digest);
    fr_test_server_add_body(test_case->server, "/catalog.json", catalog);
}

TEST a_translator_carries_a_foreign_plugin_over(void) {
    migrate_case test_case;
    open_case(&test_case, "translator", "hello");
    serve_releaser_catalog(&test_case, FIXTURES "/plugin-foreign.lua", FIXTURES "/releaser.lua");
    run_case(&test_case);

    char *manifest = project_text(&test_case, "daukle.toml");
    int has_publish = manifest != NULL && strstr(manifest, "[publish.release]") != NULL;
    int from_instance = manifest != NULL && strstr(manifest, "from = \"stub\"") != NULL;
    int has_releaser = manifest != NULL && strstr(manifest, "[plugins.releaser]") != NULL;
    free(manifest);
    int status = test_case.status;
    fr_migrate_outcome outcome = test_case.outcome;
    close_case(&test_case);

    ASSERT_EQm(last_message, FR_OK, status);
    ASSERT_EQ(FR_MIGRATE_DONE, outcome);
    ASSERT(has_publish);
    ASSERT(from_instance);
    ASSERT(has_releaser);
    PASS();
}

TEST a_translator_from_a_plugin_other_than_the_replacer_does_not_answer(void) {
    migrate_case test_case;
    open_case(&test_case, "foreigntranslator", "hello");
    serve_releaser_catalog(&test_case, FIXTURES "/plugin-foreign-self.lua",
                           FIXTURES "/releaser-bare.lua");
    run_case(&test_case);
    int status = test_case.status;
    fr_migrate_outcome outcome = test_case.outcome;
    int explained = printed_holds(&test_case, "releaser claims faketool org.example.release in"
                                              " its catalog but declares no translator for it");
    int wrote = project_file_exists(&test_case, "daukle.toml");
    close_case(&test_case);

    ASSERT_EQm(last_message, FR_OK, status);
    ASSERT_EQm(last_message, FR_MIGRATE_BLOCKED, outcome);
    ASSERTm(last_message, explained);
    ASSERT_FALSE(wrote);
    PASS();
}

/* A translator may carry generated files for the toolchain instances its own
   manifest sets, as a migrator's are confined to its instance's: daukle/npm
   writes the pins of the dependencies daukle/node's migrator hands it. */
TEST a_translator_writes_files_for_an_instance_it_sets(void) {
    migrate_case test_case;
    open_case(&test_case, "translatorfiles", "hello");
    serve_releaser_catalog(&test_case, FIXTURES "/plugin-foreign.lua", FIXTURES "/releaser-files.lua");
    run_case(&test_case);
    char *pins = project_text(&test_case, "daukle/stub/pins.txt");
    int written = pins != NULL && strcmp(pins, "pinned, resolving false") == 0;
    free(pins);
    int status = test_case.status;
    fr_migrate_outcome outcome = test_case.outcome;
    close_case(&test_case);

    ASSERT_EQm(last_message, FR_OK, status);
    ASSERT_EQm(last_message, FR_MIGRATE_DONE, outcome);
    ASSERT(written);
    PASS();
}

TEST a_translator_file_for_an_instance_it_does_not_set_is_refused(void) {
    migrate_case test_case;
    open_case(&test_case, "translatorelsewhere", "hello");
    serve_releaser_catalog(&test_case, FIXTURES "/plugin-foreign.lua",
                           FIXTURES "/releaser-files-elsewhere.lua");
    run_case(&test_case);
    int status = test_case.status;
    int named = strstr(test_case.err.message, "\"daukle/stub/pins.txt\" must be a relative file path below"
                                     " daukle/<instance>/ of a toolchain instance its manifest sets")
                != NULL;
    int wrote = project_file_exists(&test_case, "daukle/stub/pins.txt");
    close_case(&test_case);

    ASSERT_EQ(FR_ERR, status);
    ASSERTm(last_message, named);
    ASSERT_FALSE(wrote);
    PASS();
}

typedef struct {
    int status;
    fr_migrate_outcome outcome;
    int wrote;
    int untouched;
    int explained;
} owned_answer;

/* occupied names a file to plant in build/daukle/migrate/old first, or NULL. */
static owned_answer run_owned_case(const char *label, const char *fixture, const char *occupied,
                                   const char *needle) {
    migrate_case test_case;
    open_case(&test_case, label, "hello");
    serve_stub_catalog(&test_case, fixture, "faketool.conf");
    if (occupied != NULL) {
        const char *directories[] = { "build", "build/daukle", "build/daukle/migrate",
                                      "build/daukle/migrate/old" };
        char path[1024];
        for (size_t index = 0; index < sizeof directories / sizeof *directories; index++) {
            project_path(&test_case, directories[index], path, sizeof path);
            fr_test_make_directory(path);
        }
        project_path(&test_case, occupied, path, sizeof path);
        write_bytes(path, "an earlier copy\n");
    }
    run_case(&test_case);
    owned_answer answer;
    answer.status = test_case.status;
    answer.outcome = test_case.outcome;
    answer.wrote = project_file_exists(&test_case, "daukle.toml");
    answer.untouched = old_files_still_in_place(&test_case);
    answer.explained = printed_holds(&test_case, needle);
    close_case(&test_case);
    return answer;
}

TEST an_owned_path_that_does_not_exist_writes_nothing(void) {
    owned_answer answer = run_owned_case("ownedmissing", FIXTURES "/plugin-owned-missing.lua",
                                         NULL, "absent.txt");
    ASSERT_EQm(last_message, FR_OK, answer.status);
    ASSERT_EQm(last_message, FR_MIGRATE_BLOCKED, answer.outcome);
    ASSERT_FALSE(answer.wrote);
    ASSERT(answer.untouched);
    ASSERTm(last_message, answer.explained);
    PASS();
}

TEST nested_owned_paths_write_nothing(void) {
    owned_answer answer = run_owned_case("ownednested", FIXTURES "/plugin-owned-nested.lua",
                                         NULL, "out/greeting.txt");
    ASSERT_EQm(last_message, FR_OK, answer.status);
    ASSERT_EQm(last_message, FR_MIGRATE_BLOCKED, answer.outcome);
    ASSERT_FALSE(answer.wrote);
    ASSERT(answer.untouched);
    ASSERTm(last_message, answer.explained);
    PASS();
}

TEST an_occupied_place_in_the_old_directory_writes_nothing(void) {
    owned_answer answer = run_owned_case("ownedoccupied", FIXTURES "/plugin.lua",
                                         "build/daukle/migrate/old/faketool.conf",
                                         "an earlier migration's copy");
    ASSERT_EQm(last_message, FR_OK, answer.status);
    ASSERT_EQm(last_message, FR_MIGRATE_BLOCKED, answer.outcome);
    ASSERT_FALSE(answer.wrote);
    ASSERT(answer.untouched);
    ASSERTm(last_message, answer.explained);
    PASS();
}

TEST a_candidate_with_no_asset_is_a_blocker(void) {
    migrate_case test_case;
    open_case(&test_case, "noasset", "hello");
    fr_test_server_add_body(test_case.server, "/catalog.json",
                            "{\"plugins\":[{\"id\":\"stub\",\"asset\":null,"
                            "\"detect\":[{\"files\":[\"faketool.conf\"]}]}]}");
    run_case(&test_case);
    int status = test_case.status;
    fr_migrate_outcome outcome = test_case.outcome;
    int named = printed_holds(&test_case, "stub");
    int explained = printed_holds(&test_case, "no published release");
    int wrote = project_file_exists(&test_case, "daukle.toml");
    close_case(&test_case);

    ASSERT_EQm(last_message, FR_OK, status);
    ASSERT_EQ(FR_MIGRATE_BLOCKED, outcome);
    ASSERT(named);
    ASSERT(explained);
    ASSERT_FALSE(wrote);
    PASS();
}

TEST a_failed_proof_keeps_the_manifest_and_moves_nothing(void) {
    migrate_case test_case;
    open_case(&test_case, "unproved", "goodbye");
    serve_stub_catalog(&test_case, FIXTURES "/plugin.lua", "faketool.conf");
    run_case(&test_case);
    int status = test_case.status;
    fr_migrate_outcome outcome = test_case.outcome;
    int kept = project_file_exists(&test_case, "daukle.toml");
    int untouched = old_files_still_in_place(&test_case);
    int nothing_aside = !project_directory_exists(&test_case, "build/daukle/migrate/old");
    int explained = printed_holds(&test_case, "daukle migrate: the new build differs from the old one;"
                                              " daukle.toml is kept for inspection and nothing was removed");
    int names_no_files = !printed_holds(&test_case, "migrator's files");
    close_case(&test_case);

    ASSERT_EQm(last_message, FR_OK, status);
    ASSERT_EQ(FR_MIGRATE_UNPROVED, outcome);
    ASSERT(kept);
    ASSERT(untouched);
    ASSERT(nothing_aside);
    ASSERT(explained);
    ASSERT(names_no_files);
    PASS();
}

typedef struct {
    int status;
    fr_migrate_outcome outcome;
    cJSON *report;
    int printed_unproved;
} reported_run;

/* The report goes to the scratch directory, outside the project, so the run
   itself never sees it. */
static reported_run run_reported(const char *label, const char *greeting, const char *fixture,
                                 const char *rule_file) {
    reported_run result = { 0 };
    migrate_case test_case;
    open_case(&test_case, label, greeting);
    snprintf(test_case.report_path, sizeof test_case.report_path, "%s/report.json",
             test_case.scratch);
    serve_stub_catalog(&test_case, fixture, rule_file);
    run_case(&test_case);
    result.status = test_case.status;
    result.outcome = test_case.outcome;
    result.printed_unproved = printed_holds(&test_case, "daukle migrate: the new build differs");
    char *text = NULL;
    fr_error err;
    if (fr_file_read_text(test_case.report_path, &text, &err) == FR_OK) {
        result.report = cJSON_Parse(text);
        free(text);
    }
    close_case(&test_case);
    return result;
}

static const char *report_string(const cJSON *object, const char *name) {
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(field) ? field->valuestring : "";
}

static const cJSON *first_comparison(const cJSON *report) {
    const cJSON *proof = cJSON_GetObjectItemCaseSensitive(report, "proof");
    return cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(proof, "compared"), 0);
}

TEST a_report_records_a_migration(void) {
    reported_run run = run_reported("report-clean", "hello", FIXTURES "/plugin.lua", "faketool.conf");
    ASSERT_EQ(FR_OK, run.status);
    ASSERT(run.report != NULL);
    ASSERT_STR_EQ("migrated", report_string(run.report, "outcome"));
    const cJSON *detected = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(run.report, "detected"), 0);
    ASSERT_STR_EQ("stub", report_string(detected, "plugin"));
    const cJSON *comparison = first_comparison(run.report);
    ASSERT_STR_EQ("stub", report_string(comparison, "instance"));
    ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(comparison, "same")));
    cJSON_Delete(run.report);
    PASS();
}

TEST a_note_is_printed_whatever_the_outcome(void) {
    migrate_case test_case;
    open_case(&test_case, "notes-printed", "hello");
    serve_stub_catalog(&test_case, FIXTURES "/plugin-notes.lua", "faketool.conf");
    run_case(&test_case);
    int status = test_case.status;
    fr_migrate_outcome outcome = test_case.outcome;
    int frozen = printed_holds(&test_case, "daukle migrate: migrator stub: frozen WITH_TOOLS=ON");
    int install = printed_holds(&test_case, "not migrated: install rules in CMakeLists.txt");
    close_case(&test_case);

    ASSERT_EQm(last_message, FR_OK, status);
    ASSERT_EQ(FR_MIGRATE_DONE, outcome);
    ASSERTm(last_message, frozen);
    ASSERT(install);
    PASS();
}

TEST a_report_records_every_note(void) {
    reported_run run = run_reported("report-notes", "hello", FIXTURES "/plugin-notes.lua", "faketool.conf");
    ASSERT_EQ(FR_OK, run.status);
    ASSERT(run.report != NULL);
    const cJSON *note = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(run.report, "notes"), 0);
    ASSERT_STR_EQ("migrator stub", report_string(note, "source"));
    ASSERT_STR_EQ("frozen WITH_TOOLS=ON", report_string(note, "note"));
    cJSON_Delete(run.report);
    PASS();
}

TEST a_note_that_is_not_a_string_is_malformed(void) {
    migrate_case test_case;
    open_case(&test_case, "notes-malformed", "hello");
    serve_stub_catalog(&test_case, FIXTURES "/plugin-notes-malformed.lua", "faketool.conf");
    run_case(&test_case);
    int status = test_case.status;
    int named = error_holds(&test_case, "migrator stub returned a malformed result: notes must be a list of strings");
    close_case(&test_case);

    ASSERT_EQ(FR_ERR, status);
    ASSERTm(last_message, named);
    PASS();
}

TEST a_report_records_every_blocker(void) {
    reported_run run = run_reported("report-blocked", "hello", FIXTURES "/plugin-blocker.lua",
                                    "faketool.conf");
    ASSERT_EQ(FR_MIGRATE_BLOCKED, run.outcome);
    ASSERT(run.report != NULL);
    ASSERT_STR_EQ("blocked", report_string(run.report, "outcome"));
    const cJSON *blocker = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(run.report, "blockers"), 0);
    ASSERT_STR_EQ("faketool.magic", report_string(blocker, "construct"));
    ASSERT_STR_EQ("faketool's magic has no daukle form", report_string(blocker, "why"));
    ASSERT(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(blocker, "surface")));
    cJSON_Delete(run.report);
    PASS();
}

TEST a_report_records_what_the_proof_found_different(void) {
    reported_run run = run_reported("report-unproved", "goodbye", FIXTURES "/plugin.lua",
                                    "faketool.conf");
    ASSERT_EQ(FR_MIGRATE_UNPROVED, run.outcome);
    ASSERT(run.printed_unproved);
    ASSERT(run.report != NULL);
    ASSERT_STR_EQ("unproved", report_string(run.report, "outcome"));
    const cJSON *comparison = first_comparison(run.report);
    ASSERT(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(comparison, "same")));
    const cJSON *difference =
        cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(comparison, "differences"), 0);
    ASSERT_STR_EQ("bytes", report_string(difference, "kind"));
    ASSERT_EQ(64u, strlen(report_string(difference, "oldSha256")));
    cJSON_Delete(run.report);
    PASS();
}

TEST a_report_records_a_failed_proof_task(void) {
    reported_run run = run_reported("report-task", "hello", FIXTURES "/plugin-failing-task.lua",
                                    "faketool.conf");
    ASSERT_EQ(FR_MIGRATE_UNPROVED, run.outcome);
    ASSERT(run.report != NULL);
    ASSERT_STR_EQ("unproved", report_string(run.report, "outcome"));
    const cJSON *proof = cJSON_GetObjectItemCaseSensitive(run.report, "proof");
    ASSERT(strstr(report_string(proof, "failure"), "the stub build broke") != NULL);
    cJSON_Delete(run.report);
    PASS();
}

TEST a_report_tells_nothing_detected_from_an_error(void) {
    reported_run run = run_reported("report-undetected", "hello", FIXTURES "/plugin.lua",
                                    "absent.conf");
    ASSERT_EQ(FR_ERR, run.status);
    ASSERT(run.report != NULL);
    ASSERT_STR_EQ("undetected", report_string(run.report, "outcome"));
    ASSERT(strstr(report_string(run.report, "error"), "no catalog plugin recognised") != NULL);
    cJSON_Delete(run.report);

    run = run_reported("report-error", "hello", FIXTURES "/plugin-broken.lua", "faketool.conf");
    ASSERT_EQ(FR_ERR, run.status);
    ASSERT(run.report != NULL);
    ASSERT_STR_EQ("error", report_string(run.report, "outcome"));
    ASSERT(strstr(report_string(run.report, "error"), "the stub plugin is broken") != NULL);
    cJSON_Delete(run.report);
    PASS();
}

TEST an_unwritable_report_stops_before_anything_is_migrated(void) {
    migrate_case test_case;
    open_case(&test_case, "report-unwritable", "hello");
    snprintf(test_case.report_path, sizeof test_case.report_path, "%s/absent/report.json",
             test_case.scratch);
    serve_stub_catalog(&test_case, FIXTURES "/plugin.lua", "faketool.conf");
    run_case(&test_case);
    int status = test_case.status;
    int wrote = project_file_exists(&test_case, "daukle.toml");
    int untouched = old_files_still_in_place(&test_case);
    close_case(&test_case);

    ASSERT_EQ(FR_ERR, status);
    ASSERT_FALSE(wrote);
    ASSERT(untouched);
    PASS();
}

TEST migrate_files_written_before_the_proof(void) {
    migrate_case test_case;
    open_case(&test_case, "files", "hello");
    serve_stub_catalog(&test_case, FIXTURES "/plugin-files.lua", "faketool.conf");
    run_case(&test_case);
    char *carried = project_text(&test_case, "daukle/stub/carried.txt");
    int carried_matches = carried != NULL && strcmp(carried, "hello") == 0;
    free(carried);
    int status = test_case.status;
    fr_migrate_outcome outcome = test_case.outcome;
    close_case(&test_case);

    ASSERT_EQm(last_message, FR_OK, status);
    ASSERT_EQm(last_message, FR_MIGRATE_DONE, outcome);
    ASSERT(carried_matches);
    PASS();
}

TEST migrate_files_kept_when_the_proof_fails(void) {
    migrate_case test_case;
    open_case(&test_case, "files-unproved", "goodbye");
    serve_stub_catalog(&test_case, FIXTURES "/plugin-files.lua", "faketool.conf");
    run_case(&test_case);
    int carried = project_file_exists(&test_case, "daukle/stub/carried.txt");
    int manifest_kept = project_file_exists(&test_case, "daukle.toml");
    int untouched = old_files_still_in_place(&test_case);
    int explained = printed_holds(&test_case,
                                  "daukle.toml and the migrator's files are kept for inspection");
    int status = test_case.status;
    fr_migrate_outcome outcome = test_case.outcome;
    close_case(&test_case);

    ASSERT_EQm(last_message, FR_OK, status);
    ASSERT_EQ(FR_MIGRATE_UNPROVED, outcome);
    ASSERT(carried);
    ASSERT(manifest_kept);
    ASSERT(untouched);
    ASSERT(explained);
    PASS();
}

TEST clean_refuses_while_a_migration_is_unfinished(void) {
    migrate_case test_case;
    open_case(&test_case, "unfinished", "hello");
    serve_stub_catalog(&test_case, FIXTURES "/plugin.lua", "faketool.conf");
    run_case(&test_case);
    int migrated = test_case.status == FR_OK && test_case.outcome == FR_MIGRATE_DONE;

    fr_error clean_err;
    clean_err.message[0] = '\0';
    int refused = fr_derived_clean(test_case.project, &clean_err) == FR_ERR;
    int named = strstr(clean_err.message, "daukle migrate --done") != NULL;
    int still_kept = project_file_exists(&test_case, "build/daukle/migrate/old/faketool.conf");

    static fr_error done_err;
    done_err.message[0] = '\0';
    int done_status = FR_ERR;
    if (fr_test_set_working_directory(test_case.project)) {
        done_status = fr_migrate_done(&done_err);
        fr_test_set_working_directory(test_case.original_dir);
    }
    int old_gone = !project_directory_exists(&test_case, "build/daukle/migrate/old");
    int cleaned = fr_derived_clean(test_case.project, &clean_err) == FR_OK;
    close_case(&test_case);

    ASSERT(migrated);
    ASSERT(refused);
    ASSERT(named);
    ASSERT(still_kept);
    ASSERT_EQm(done_err.message, FR_OK, done_status);
    ASSERT(old_gone);
    ASSERT(cleaned);
    PASS();
}

TEST done_with_nothing_to_finish_says_so(void) {
    migrate_case test_case;
    open_case(&test_case, "nothingtofinish", NULL);
    static fr_error err;
    err.message[0] = '\0';
    fr_test_get_working_directory(test_case.original_dir, sizeof test_case.original_dir);
    int status = FR_OK;
    if (fr_test_set_working_directory(test_case.project)) {
        status = fr_migrate_done(&err);
        fr_test_set_working_directory(test_case.original_dir);
    }
    close_case(&test_case);

    ASSERT_EQ(FR_ERR, status);
    ASSERTm(err.message, strstr(err.message, "there is no unfinished migration here") != NULL);
    PASS();
}

TEST a_chunk_that_gates_on_the_manifest_name_loads_during_migrate(void) {
    migrate_case test_case;
    open_case(&test_case, "gated", "hello");
    serve_stub_catalog(&test_case, FIXTURES "/plugin-gated.lua", "faketool.conf");
    run_case(&test_case);
    int status = test_case.status;
    fr_migrate_outcome outcome = test_case.outcome;
    close_case(&test_case);

    ASSERT_EQm(last_message, FR_OK, status);
    ASSERT_EQ(FR_MIGRATE_DONE, outcome);
    PASS();
}

static void make_project_directories(const migrate_case *test_case, const char *const *relatives,
                                     size_t count) {
    char path[1024];
    for (size_t index = 0; index < count; index++) {
        project_path(test_case, relatives[index], path, sizeof path);
        fr_test_make_directory(path);
    }
}

/* A junction on Windows, where an unprivileged process can make no other
   directory link, and a symlink elsewhere; it worked only if the marker
   below the target reads through it. */
static int link_directory(const char *link_path, const char *target_path, const char *marker) {
#ifdef _WIN32
    char command[2400];
    snprintf(command, sizeof command, "cmd /c mklink /J \"%s\" \"%s\" >nul 2>&1", link_path,
             target_path);
    if (system(command) != 0) return 0;
#else
    if (symlink(target_path, link_path) != 0) return 0;
#endif
    char probe[1200];
    snprintf(probe, sizeof probe, "%s/%s", link_path, marker);
    return fr_cache_file_exists(probe);
}

static void unlink_directory(const char *link_path) {
#ifdef _WIN32
    _rmdir(link_path);
#else
    remove(link_path);
#endif
}

typedef struct {
    char link[1024];
    char outside[640];
    char marker[1024];
    int linked;
} planted_link;

/* Plants link_at inside the project as a link to a directory outside it that
   holds marker_below, then the marker itself. */
static void plant_link(const migrate_case *test_case, const char *link_at,
                       const char *const *target_directories, size_t directory_count,
                       const char *marker_below, planted_link *out) {
    snprintf(out->outside, sizeof out->outside, "%s/outside", test_case->scratch);
    fr_test_make_directory(out->outside);
    char path[1024];
    for (size_t index = 0; index < directory_count; index++) {
        snprintf(path, sizeof path, "%s/%s", out->outside, target_directories[index]);
        fr_test_make_directory(path);
    }
    snprintf(out->marker, sizeof out->marker, "%s/%s", out->outside, marker_below);
    write_bytes(out->marker, "precious\n");
    project_path(test_case, link_at, out->link, sizeof out->link);
    out->linked = link_directory(out->link, out->outside, marker_below);
}

static int done_through(const char *label, const char *link_at, const char *const *parents,
                        size_t parent_count, const char *const *targets, size_t target_count,
                        const char *marker_below, int *out_linked, char *message,
                        size_t message_size) {
    migrate_case test_case;
    open_case(&test_case, label, NULL);
    make_project_directories(&test_case, parents, parent_count);
    planted_link planted;
    plant_link(&test_case, link_at, targets, target_count, marker_below, &planted);
    *out_linked = planted.linked;
    int status = FR_OK;
    fr_error err;
    err.message[0] = '\0';
    fr_test_get_working_directory(test_case.original_dir, sizeof test_case.original_dir);
    if (planted.linked && fr_test_set_working_directory(test_case.project)) {
        status = fr_migrate_done(&err);
        fr_test_set_working_directory(test_case.original_dir);
    }
    int survived = fr_cache_file_exists(planted.marker);
    snprintf(message, message_size, "%s", err.message);
    unlink_directory(planted.link);
    close_case(&test_case);
    return status == FR_ERR && survived && strstr(message, "refusing") != NULL;
}

TEST done_refuses_an_old_directory_that_is_a_link(void) {
    const char *parents[] = { "build", "build/daukle", "build/daukle/migrate" };
    int linked = 0;
    static char message[512];
    int refused = done_through("donelink", "build/daukle/migrate/old", parents, 3, NULL, 0,
                               "marker.txt", &linked, message, sizeof message);
    if (!linked) SKIPm("could not create a junction or symlink on this machine");
    ASSERTm(message, refused);
    ASSERTm(message, strstr(message, "build/daukle/migrate/old") != NULL);
    ASSERTm(message, strstr(message, "link") != NULL);
    PASS();
}

TEST done_refuses_an_old_directory_that_resolves_outside_the_project(void) {
    const char *targets[] = { "daukle", "daukle/migrate", "daukle/migrate/old" };
    int linked = 0;
    static char message[512];
    int refused = done_through("doneescape", "build", NULL, 0, targets, 3,
                               "daukle/migrate/old/marker.txt", &linked, message, sizeof message);
    if (!linked) SKIPm("could not create a junction or symlink on this machine");
    ASSERTm(message, refused);
    ASSERTm(message, strstr(message, "outside") != NULL);
    PASS();
}

TEST a_linked_old_directory_blocks_before_anything_is_written(void) {
    migrate_case test_case;
    open_case(&test_case, "ownedlink", "hello");
    serve_stub_catalog(&test_case, FIXTURES "/plugin.lua", "faketool.conf");
    const char *parents[] = { "build", "build/daukle", "build/daukle/migrate" };
    make_project_directories(&test_case, parents, 3);
    planted_link planted;
    plant_link(&test_case, "build/daukle/migrate/old", NULL, 0, "marker.txt", &planted);
    if (planted.linked) run_case(&test_case);
    char moved_out[1024];
    snprintf(moved_out, sizeof moved_out, "%s/faketool.conf", planted.outside);
    int status = test_case.status;
    fr_migrate_outcome outcome = test_case.outcome;
    int wrote = project_file_exists(&test_case, "daukle.toml");
    int untouched = old_files_still_in_place(&test_case);
    int leaked = fr_cache_file_exists(moved_out);
    int explained = printed_holds(&test_case, "is a link");
    unlink_directory(planted.link);
    close_case(&test_case);

    if (!planted.linked) SKIPm("could not create a junction or symlink on this machine");
    ASSERT_EQm(last_message, FR_OK, status);
    ASSERT_EQm(last_message, FR_MIGRATE_BLOCKED, outcome);
    ASSERT_FALSE(wrote);
    ASSERT(untouched);
    ASSERT_FALSE(leaked);
    ASSERTm(last_message, explained);
    PASS();
}

TEST a_detection_in_a_subdirectory_migrates_as_its_own_instance(void) {
    migrate_case test_case;
    open_case(&test_case, "subdir", NULL);
    char path[1024];
    project_path(&test_case, "faketool.conf", path, sizeof path);
    remove(path);
    const char *directories[] = { "web", "web/out" };
    make_project_directories(&test_case, directories, 2);
    project_path(&test_case, "web/faketool.conf", path, sizeof path);
    copy_fixture_file(FIXTURES "/project/faketool.conf", path);
    project_path(&test_case, "web/out/greeting.txt", path, sizeof path);
    write_bytes(path, "hello");
    serve_stub_catalog_rule(&test_case, FIXTURES "/plugin-subdir.lua",
                            "{\"files\":[\"faketool.conf\"],\"scope\":\"tree\"}");
    run_case(&test_case);

    char *manifest = project_text(&test_case, "daukle.toml");
    int names_instance = manifest != NULL && strstr(manifest, "[toolchains.stub-web]") != NULL;
    int names_toolchain = manifest != NULL && strstr(manifest, "toolchain = \"stub\"") != NULL;
    int names_dir = manifest != NULL && strstr(manifest, "dir = \"web\"") != NULL;
    free(manifest);
    char *root = project_text(&test_case, "build/daukle/stub-web/root.txt");
    int saw_root = root != NULL && strcmp(root, "../../../web") == 0;
    free(root);
    int conf_gone = !project_file_exists(&test_case, "web/faketool.conf");
    int out_gone = !project_directory_exists(&test_case, "web/out");
    int conf_kept = project_file_exists(&test_case, "build/daukle/migrate/old/web/faketool.conf");
    int out_kept = project_file_exists(&test_case,
                                       "build/daukle/migrate/old/web/out/greeting.txt");
    int status = test_case.status;
    fr_migrate_outcome outcome = test_case.outcome;
    close_case(&test_case);

    ASSERT_EQm(last_message, FR_OK, status);
    ASSERT_EQm(last_message, FR_MIGRATE_DONE, outcome);
    ASSERT(names_instance);
    ASSERT(names_toolchain);
    ASSERT(names_dir);
    ASSERT(saw_root);
    ASSERT(conf_gone);
    ASSERT(out_gone);
    ASSERT(conf_kept);
    ASSERT(out_kept);
    PASS();
}

/* A root detection and one in "web", which detection collapses into the root's; the root
   migrator hands back what root_instances lists. */
static void open_instances_case(migrate_case *test_case, const char *label,
                                const char *root_instances) {
    open_case(test_case, label, "hello");
    const char *directories[] = { "web", "web/out" };
    make_project_directories(test_case, directories, 2);
    char path[1024];
    project_path(test_case, "instances.txt", path, sizeof path);
    write_bytes(path, root_instances);
    project_path(test_case, "web/instances.txt", path, sizeof path);
    write_bytes(path, "");
    project_path(test_case, "web/faketool.conf", path, sizeof path);
    copy_fixture_file(FIXTURES "/project/faketool.conf", path);
    project_path(test_case, "web/out/greeting.txt", path, sizeof path);
    write_bytes(path, "hello");
    serve_stub_catalog_rule(test_case, FIXTURES "/plugin-instances.lua",
                            "{\"files\":[\"faketool.conf\"],\"scope\":\"tree\"}");
}

TEST a_handed_back_instance_migrates_as_its_own(void) {
    migrate_case test_case;
    open_instances_case(&test_case, "handed", "web\n");
    run_case(&test_case);

    char *manifest = project_text(&test_case, "daukle.toml");
    int names_root = manifest != NULL && strstr(manifest, "[toolchains.stub]") != NULL;
    int names_web = manifest != NULL && strstr(manifest, "[toolchains.stub-web]") != NULL;
    int names_dir = manifest != NULL && strstr(manifest, "dir = \"web\"") != NULL;
    free(manifest);
    int root_moved = project_file_exists(&test_case, "build/daukle/migrate/old/faketool.conf");
    int web_moved = project_file_exists(&test_case, "build/daukle/migrate/old/web/faketool.conf");
    int status = test_case.status;
    fr_migrate_outcome outcome = test_case.outcome;
    close_case(&test_case);

    ASSERT_EQm(last_message, FR_OK, status);
    ASSERT_EQm(last_message, FR_MIGRATE_DONE, outcome);
    ASSERT(names_root);
    ASSERT(names_web);
    ASSERT(names_dir);
    ASSERT(root_moved);
    ASSERT(web_moved);
    PASS();
}

static void add_instance_directory(migrate_case *test_case, const char *dir) {
    char out_dir[512], file[1024];
    snprintf(out_dir, sizeof out_dir, "%s/out", dir);
    const char *directories[] = { dir, out_dir };
    make_project_directories(test_case, directories, 2);
    snprintf(file, sizeof file, "%s/instances.txt", dir);
    char path[1024];
    project_path(test_case, file, path, sizeof path);
    write_bytes(path, "");
    snprintf(file, sizeof file, "%s/faketool.conf", dir);
    project_path(test_case, file, path, sizeof path);
    copy_fixture_file(FIXTURES "/project/faketool.conf", path);
    snprintf(file, sizeof file, "%s/out/greeting.txt", dir);
    project_path(test_case, file, path, sizeof path);
    write_bytes(path, "hello");
}

TEST a_directory_a_task_name_refuses_is_escaped_and_stays_its_own(void) {
    migrate_case test_case;
    open_case(&test_case, "escaped", "hello");
    const char *parents[] = { "@acme", "acme", "Upper" };
    make_project_directories(&test_case, parents, 3);
    add_instance_directory(&test_case, "@acme/web");
    add_instance_directory(&test_case, "acme/web");
    add_instance_directory(&test_case, "Upper/web");
    char path[1024];
    project_path(&test_case, "instances.txt", path, sizeof path);
    write_bytes(path, "@acme/web\nacme/web\nUpper/web\n");
    serve_stub_catalog_rule(&test_case, FIXTURES "/plugin-instances.lua",
                            "{\"files\":[\"faketool.conf\"],\"scope\":\"tree\"}");
    run_case(&test_case);

    char *manifest = project_text(&test_case, "daukle.toml");
    int names_scoped = manifest != NULL && strstr(manifest, "[toolchains.stub-_40acme-web]") != NULL;
    int names_plain = manifest != NULL && strstr(manifest, "[toolchains.stub-acme-web]") != NULL;
    int names_upper = manifest != NULL && strstr(manifest, "[toolchains.stub-_55pper-web]") != NULL;
    int keeps_dir = manifest != NULL && strstr(manifest, "dir = \"@acme/web\"") != NULL;
    free(manifest);
    int status = test_case.status;
    fr_migrate_outcome outcome = test_case.outcome;
    close_case(&test_case);

    ASSERT_EQm(last_message, FR_OK, status);
    ASSERT_EQm(last_message, FR_MIGRATE_DONE, outcome);
    ASSERT(names_scoped);
    ASSERT(names_plain);
    ASSERT(names_upper);
    ASSERT(keeps_dir);
    PASS();
}

TEST a_collapsed_detection_stays_the_outer_ones_without_instances(void) {
    migrate_case test_case;
    open_instances_case(&test_case, "collapsed", "");
    run_case(&test_case);

    char *manifest = project_text(&test_case, "daukle.toml");
    int names_web = manifest != NULL && strstr(manifest, "stub-web") != NULL;
    free(manifest);
    int web_kept_in_place = project_file_exists(&test_case, "web/faketool.conf");
    int status = test_case.status;
    close_case(&test_case);

    ASSERT_EQm(last_message, FR_OK, status);
    ASSERT_FALSE(names_web);
    ASSERT(web_kept_in_place);
    PASS();
}

static int refuses_handed_instances(const char *label, const char *root_instances,
                                    const char *needle) {
    migrate_case test_case;
    open_instances_case(&test_case, label, root_instances);
    run_case(&test_case);
    int wrote = project_file_exists(&test_case, "daukle.toml");
    int explained = strstr(test_case.err.message, needle) != NULL;
    snprintf(last_message, sizeof last_message, "%s", test_case.err.message);
    int status = test_case.status;
    close_case(&test_case);
    return status == FR_ERR && !wrote && explained;
}

TEST an_instance_inside_the_derived_tree_is_a_malformed_result(void) {
    ASSERTm(last_message, refuses_handed_instances("derived", "build/daukle/x\n",
                                                   "instance \"build/daukle/x\" must be a relative"));
    PASS();
}

TEST an_instance_that_climbs_out_is_a_malformed_result(void) {
    ASSERTm(last_message, refuses_handed_instances("climbs", "../elsewhere\n",
                                                   "instance \"../elsewhere\" must be a relative"));
    PASS();
}

TEST an_instance_outside_the_asking_directory_is_a_malformed_result(void) {
    migrate_case test_case;
    open_instances_case(&test_case, "sibling", "web\n");
    const char *directories[] = { "docs" };
    make_project_directories(&test_case, directories, 1);
    char path[1024];
    project_path(&test_case, "web/instances.txt", path, sizeof path);
    write_bytes(path, "docs\n");
    run_case(&test_case);
    int wrote = project_file_exists(&test_case, "daukle.toml");
    int explained = strstr(test_case.err.message,
                           "instance \"docs\" is not a directory below \"web\"") != NULL;
    snprintf(last_message, sizeof last_message, "%s", test_case.err.message);
    int status = test_case.status;
    close_case(&test_case);

    ASSERT_EQm(last_message, FR_ERR, status);
    ASSERT_FALSE(wrote);
    ASSERTm(last_message, explained);
    PASS();
}

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(a_handed_back_instance_migrates_as_its_own);
    RUN_TEST(an_instance_outside_the_asking_directory_is_a_malformed_result);
    RUN_TEST(a_directory_a_task_name_refuses_is_escaped_and_stays_its_own);
    RUN_TEST(a_collapsed_detection_stays_the_outer_ones_without_instances);
    RUN_TEST(an_instance_inside_the_derived_tree_is_a_malformed_result);
    RUN_TEST(an_instance_that_climbs_out_is_a_malformed_result);
    RUN_TEST(done_refuses_an_old_directory_that_is_a_link);
    RUN_TEST(done_refuses_an_old_directory_that_resolves_outside_the_project);
    RUN_TEST(a_linked_old_directory_blocks_before_anything_is_written);
    RUN_TEST(a_detection_in_a_subdirectory_migrates_as_its_own_instance);
    RUN_TEST(a_clean_migration_writes_proves_and_moves_aside);
    RUN_TEST(migrate_refuses_a_project_that_has_a_manifest);
    RUN_TEST(no_catalog_is_refused_naming_both_inputs);
    RUN_TEST(nothing_detected_says_so_and_writes_nothing);
    RUN_TEST(a_blocker_writes_nothing);
    RUN_TEST(an_unclaimed_foreign_plugin_is_a_blocker);
    RUN_TEST(a_translator_carries_a_foreign_plugin_over);
    RUN_TEST(a_translator_from_a_plugin_other_than_the_replacer_does_not_answer);
    RUN_TEST(a_translator_writes_files_for_an_instance_it_sets);
    RUN_TEST(a_translator_file_for_an_instance_it_does_not_set_is_refused);
    RUN_TEST(an_owned_path_that_does_not_exist_writes_nothing);
    RUN_TEST(nested_owned_paths_write_nothing);
    RUN_TEST(an_occupied_place_in_the_old_directory_writes_nothing);
    RUN_TEST(a_candidate_with_no_asset_is_a_blocker);
    RUN_TEST(a_failed_proof_keeps_the_manifest_and_moves_nothing);
    RUN_TEST(migrate_files_written_before_the_proof);
    RUN_TEST(migrate_files_kept_when_the_proof_fails);
    RUN_TEST(clean_refuses_while_a_migration_is_unfinished);
    RUN_TEST(done_with_nothing_to_finish_says_so);
    RUN_TEST(a_chunk_that_gates_on_the_manifest_name_loads_during_migrate);
    RUN_TEST(a_report_records_a_migration);
    RUN_TEST(a_note_is_printed_whatever_the_outcome);
    RUN_TEST(a_report_records_every_note);
    RUN_TEST(a_note_that_is_not_a_string_is_malformed);
    RUN_TEST(a_report_records_every_blocker);
    RUN_TEST(a_report_records_what_the_proof_found_different);
    RUN_TEST(a_report_records_a_failed_proof_task);
    RUN_TEST(a_report_tells_nothing_detected_from_an_error);
    RUN_TEST(an_unwritable_report_stops_before_anything_is_migrated);
    GREATEST_MAIN_END();
}
