#include "greatest.h"

#include "cache/cache.h"
#include "http_server.h"
#include "migrate/migrate.h"
#include "project/derived.h"
#include "project/region.h"
#include "support.h"
#include "util/error.h"
#include "util/sha256.h"

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
        fr_migrate_options options = { catalogs, catalog_count, NULL, 0, 1 };
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
    int explained = printed_holds(&test_case, "the new build differs from the old one");
    close_case(&test_case);

    ASSERT_EQm(last_message, FR_OK, status);
    ASSERT_EQ(FR_MIGRATE_UNPROVED, outcome);
    ASSERT(kept);
    ASSERT(untouched);
    ASSERT(nothing_aside);
    ASSERT(explained);
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

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
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
    GREATEST_MAIN_END();
}
