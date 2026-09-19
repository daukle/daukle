#include "greatest.h"

#include "cJSON.h"
#include "cache.h"
#include "config.h"
#include "config_lua.h"
#include "http.h"
#include "manifest.h"
#include "plugins.h"
#include "region.h"
#include "registry.h"
#include "sha256.h"
#include "support.h"
#include "sync.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cJSON *document_from(const char *json) {
    cJSON *document = cJSON_Parse(json);
    return document;
}

TEST parses_the_string_coordinate_form(void) {
    cJSON *document = document_from("{\"plugins\":{\"npm\":\"daukle/npm@^1.0.0\"}}");
    fr_plugin_entry *entries = NULL; size_t count = 0; fr_error err;

    ASSERT_EQ(FR_OK, fr_plugins_parse(document, &entries, &count, &err));
    ASSERT_EQ(1, (int) count);
    ASSERT_STR_EQ("npm", entries[0].label);
    ASSERT_EQ(FR_PLUGIN_REMOTE, entries[0].kind);
    ASSERT_STR_EQ("daukle/npm", entries[0].repo);
    ASSERT_STR_EQ("^1.0.0", entries[0].version);
    ASSERT(entries[0].sha256 == NULL);

    fr_plugins_free(entries, count);
    cJSON_Delete(document);
    PASS();
}

TEST parses_the_table_form_with_a_pin(void) {
    cJSON *document = document_from(
        "{\"plugins\":{\"gradle\":{\"repo\":\"daukle/gradle\",\"version\":\"^2.0.0\","
        "\"sha256\":\"abc123\"}}}");
    fr_plugin_entry *entries = NULL; size_t count = 0; fr_error err;

    ASSERT_EQ(FR_OK, fr_plugins_parse(document, &entries, &count, &err));
    ASSERT_EQ(1, (int) count);
    ASSERT_EQ(FR_PLUGIN_REMOTE, entries[0].kind);
    ASSERT_STR_EQ("daukle/gradle", entries[0].repo);
    ASSERT_STR_EQ("abc123", entries[0].sha256);

    fr_plugins_free(entries, count);
    cJSON_Delete(document);
    PASS();
}

TEST parses_a_local_path(void) {
    cJSON *document = document_from("{\"plugins\":{\"mine\":\"./plugins/mine.lua\"}}");
    fr_plugin_entry *entries = NULL; size_t count = 0; fr_error err;

    ASSERT_EQ(FR_OK, fr_plugins_parse(document, &entries, &count, &err));
    ASSERT_EQ(FR_PLUGIN_LOCAL, entries[0].kind);
    ASSERT_STR_EQ("./plugins/mine.lua", entries[0].path);

    fr_plugins_free(entries, count);
    cJSON_Delete(document);
    PASS();
}

TEST a_table_entry_naming_both_path_and_repo_is_refused(void) {
    cJSON *document = document_from(
        "{\"plugins\":{\"both\":{\"path\":\"./x.lua\",\"repo\":\"daukle/x\",\"version\":\"^1.0.0\"}}}");
    fr_plugin_entry *entries = NULL; size_t count = 0; fr_error err;

    ASSERT_EQ(FR_ERR, fr_plugins_parse(document, &entries, &count, &err));
    ASSERT(strstr(err.message, "both") != NULL);
    ASSERT(strstr(err.message, "path") != NULL);
    ASSERT(strstr(err.message, "repo") != NULL);

    cJSON_Delete(document);
    PASS();
}

TEST a_table_entry_naming_neither_path_nor_repo_is_refused(void) {
    cJSON *document = document_from("{\"plugins\":{\"neither\":{\"version\":\"^1.0.0\"}}}");
    fr_plugin_entry *entries = NULL; size_t count = 0; fr_error err;

    ASSERT_EQ(FR_ERR, fr_plugins_parse(document, &entries, &count, &err));
    ASSERT(strstr(err.message, "neither") != NULL);
    ASSERT(strstr(err.message, "path") != NULL);
    ASSERT(strstr(err.message, "repo") != NULL);

    cJSON_Delete(document);
    PASS();
}

TEST an_absent_plugins_table_yields_no_entries(void) {
    cJSON *document = document_from("{\"schema\":1}");
    fr_plugin_entry *entries = NULL; size_t count = 0; fr_error err;

    ASSERT_EQ(FR_OK, fr_plugins_parse(document, &entries, &count, &err));
    ASSERT_EQ(0, (int) count);

    fr_plugins_free(entries, count);
    cJSON_Delete(document);
    PASS();
}

TEST rejects_a_coordinate_with_no_version(void) {
    cJSON *document = document_from("{\"plugins\":{\"npm\":\"daukle/npm\"}}");
    fr_plugin_entry *entries = NULL; size_t count = 0; fr_error err;

    ASSERT_EQ(FR_ERR, fr_plugins_parse(document, &entries, &count, &err));
    ASSERT(strstr(err.message, "npm") != NULL);

    cJSON_Delete(document);
    PASS();
}

TEST rejects_a_plugins_member_that_is_not_a_table(void) {
    cJSON *document = document_from("{\"plugins\":[1,2]}");
    fr_plugin_entry *entries = NULL; size_t count = 0; fr_error err;

    ASSERT_EQ(FR_ERR, fr_plugins_parse(document, &entries, &count, &err));
    ASSERT(strstr(err.message, "plugins") != NULL);

    cJSON_Delete(document);
    PASS();
}

TEST rejects_a_coordinate_with_an_empty_half(void) {
    const char *bad[] = { "{\"plugins\":{\"a\":\"@\"}}",
                          "{\"plugins\":{\"a\":\"owner@\"}}",
                          "{\"plugins\":{\"a\":\"@1.0.0\"}}" };
    for (size_t index = 0; index < 3; index++) {
        cJSON *document = document_from(bad[index]);
        fr_plugin_entry *entries = NULL; size_t count = 0; fr_error err;
        ASSERT_EQ(FR_ERR, fr_plugins_parse(document, &entries, &count, &err));
        ASSERT(strstr(err.message, "a") != NULL);
        cJSON_Delete(document);
    }
    PASS();
}

TEST a_fetched_manifest_may_not_declare_plugins(void) {
    cJSON *document = document_from(
        "{\"schema\":1,\"project\":\"forebay/evil\",\"version\":\"1.0.0\","
        "\"plugins\":{\"x\":\"someone/x@^1.0.0\"}}");
    fr_error err;

    ASSERT_EQ(FR_ERR, fr_plugins_reject_in_fetched(document, "forebay/evil", &err));
    ASSERT(strstr(err.message, "forebay/evil") != NULL);
    ASSERT(strstr(err.message, "plugins") != NULL);

    cJSON_Delete(document);
    PASS();
}

TEST a_fetched_manifest_without_plugins_is_accepted(void) {
    cJSON *document = document_from("{\"schema\":1,\"project\":\"forebay/ok\",\"version\":\"1.0.0\"}");
    fr_error err;
    ASSERT_EQ(FR_OK, fr_plugins_reject_in_fetched(document, "forebay/ok", &err));
    cJSON_Delete(document);
    PASS();
}

/* Proves the check is wired into fr_project_parse itself, not only reachable
   by calling fr_plugins_reject_in_fetched directly: deleting the call site
   would still leave the two tests above passing. */
TEST fr_project_parse_refuses_a_fetched_manifest_declaring_plugins(void) {
    fr_project project;
    fr_error err;
    const char *text = "{\"schema\":1,\"project\":\"forebay/evil\",\"version\":\"1.0.0\","
                       "\"plugins\":{\"x\":\"someone/x@^1.0.0\"}}";

    ASSERT_EQ(FR_ERR, fr_project_parse(text, "forebay/evil", &project, &err));
    ASSERT(strstr(err.message, "forebay/evil") != NULL);
    ASSERT(strstr(err.message, "plugins") != NULL);

    PASS();
}

TEST a_local_plugin_registers_its_language(void) {
    fr_error err;
    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));

    fr_manifest manifest;
    ASSERT_EQ(FR_OK, fr_config_load_file("test/fixtures/plugin-local/daukle.toml",
                                         registry, &manifest, &err));

    ASSERT(fr_registry_language(registry, "daukle.language/hello") != NULL);

    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST a_verb_a_plugin_declared_is_there_when_it_runs(void) {
    fr_error err;
    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));

    fr_manifest manifest;
    ASSERT_EQ(FR_OK, fr_config_load_file("test/fixtures/plugin-declared-verb/daukle.toml",
                                         registry, &manifest, &err));

    ASSERT(fr_registry_language(registry, "daukle.language/brewfile") != NULL);

    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST a_manifest_declaring_no_plugins_opens_no_lua_state(void) {
    fr_error err;
    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));

    fr_manifest manifest;
    ASSERT_EQ(FR_OK, fr_config_load_file("test/fixtures/consumer/daukle.json",
                                         registry, &manifest, &err));

    ASSERT(fr_lua_runtime_state() == NULL);

    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    PASS();
}

static int load_manifest(const char *file_path, fr_error *err) {
    fr_registry *registry = NULL;
    if (fr_build_registry(&registry, err) != FR_OK) return FR_ERR;

    fr_manifest manifest;
    int status = fr_config_load_file(file_path, registry, &manifest, err);
    if (status == FR_OK) fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    return status;
}

TEST a_verb_uses_does_not_know_is_refused(void) {
    fr_error err;
    ASSERT_EQ(FR_ERR, load_manifest("test/fixtures/plugin-unknown-verb/daukle.toml", &err));
    ASSERT(strstr(err.message, "npm") != NULL);
    ASSERT(strstr(err.message, "teleport") != NULL);
    PASS();
}

TEST a_plugin_written_against_a_later_api_says_which(void) {
    fr_error err;
    ASSERT_EQ(FR_ERR, load_manifest("test/fixtures/plugin-future-api/daukle.toml", &err));
    ASSERT(strstr(err.message, "gradle") != NULL);
    ASSERT(strstr(err.message, "needs daukle api 3, this daukle provides 1") != NULL);
    PASS();
}

TEST a_uses_entry_that_is_not_a_string_is_refused(void) {
    fr_error err;
    ASSERT_EQ(FR_ERR, load_manifest("test/fixtures/plugin-bad-uses/daukle.toml", &err));
    ASSERT(strstr(err.message, "cargo") != NULL);
    ASSERT(strstr(err.message, "every name in uses must be a string") != NULL);
    PASS();
}

TEST a_verb_used_before_the_declaration_says_so(void) {
    fr_error err;
    ASSERT_EQ(FR_ERR, load_manifest("test/fixtures/plugin-verb-before-declaration/daukle.toml", &err));
    ASSERT(strstr(err.message, "early") != NULL);
    ASSERT(strstr(err.message, "daukle.plugin must be the first call") != NULL);
    PASS();
}

static int release_requests;

/* malloc + memcpy rather than strdup: strdup is not C11 and MSVC's /W4 /WX
   treats its own deprecation warning for it as an error. */
static char *copy_body(const char *text, size_t *out_length) {
    size_t length = strlen(text);
    char *copy = malloc(length + 1);
    if (copy != NULL) memcpy(copy, text, length + 1);
    *out_length = length;
    return copy;
}

/* p.lua (the 1.2.0 asset, in range for "^1.0.0") and q.lua (the 2.0.0 asset,
   out of range) register differently-named languages, so a test can tell
   which one was actually fetched rather than only that loading succeeded. A
   selection bug that ignored fr_range_satisfies and always took the highest
   tag would register remote-2-0-0 instead, which the positive assertion
   alone would not catch. */
static int stub_releases(const char *url, const fr_http_header *headers, size_t header_count,
                         char **out_body, size_t *out_length, fr_error *err) {
    (void) headers; (void) header_count; (void) err;
    release_requests++;
    if (strstr(url, "/releases") != NULL) {
        *out_body = copy_body("[{\"tag_name\":\"1.2.0\",\"assets\":"
                              "[{\"name\":\"plugin.lua\",\"browser_download_url\":\"https://x/p.lua\"}]},"
                              "{\"tag_name\":\"2.0.0\",\"assets\":"
                              "[{\"name\":\"plugin.lua\",\"browser_download_url\":\"https://x/q.lua\"}]}]",
                              out_length);
    } else if (strstr(url, "/p.lua") != NULL) {
        *out_body = copy_body("daukle.plugin{ api = 1, uses = {} }\n"
                              "daukle.language{ name = 'remote-1-2-0', apply = function() return '' end }\n",
                              out_length);
    } else {
        *out_body = copy_body("daukle.plugin{ api = 1, uses = {} }\n"
                              "daukle.language{ name = 'remote-2-0-0', apply = function() return '' end }\n",
                              out_length);
    }
    return FR_OK;
}

static void remote_coordinate(char *out, size_t out_size, const char *owner, const char *name) {
    snprintf(out, out_size, "{\"plugins\":{\"r\":\"%s/%s@^1.0.0\"}}", owner, name);
}

/* Owner is unique to this test AND this process (fr_test_process_id), so
   neither a developer's real cache nor a previous test run can already hold
   an entry that would satisfy the resolve for the wrong reason. Removing the
   whole owner directory, not just the name below it, leaves nothing behind:
   the owner is unique to this test alone, so nothing else could be living
   next to it under the same owner. */
static void remove_plugin_cache(const char *owner) {
    fr_error ignored;
    char root[1024];
    if (fr_cache_root(root, sizeof root, &ignored) != FR_OK) return;
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/plugins/%s", root, owner);
    fr_test_remove_tree(dir);
}

TEST a_remote_coordinate_picks_the_highest_release_in_range(void) {
    fr_error err;
    char owner[64];
    snprintf(owner, sizeof owner, "daukle-test-%d-highest", fr_test_process_id());
    const char *name = "remote";
    remove_plugin_cache(owner);

    release_requests = 0;
    fr_http_fn previous = fr_http_set_backend(stub_releases);

    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));

    char coordinate[256];
    remote_coordinate(coordinate, sizeof coordinate, owner, name);
    cJSON *document = cJSON_Parse(coordinate);

    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    ASSERT_EQ(FR_OK, fr_plugins_load(registry, document, ".", &err));
    ASSERT(fr_registry_language(registry, "daukle.language/remote-1-2-0") != NULL);
    ASSERT(fr_registry_language(registry, "daukle.language/remote-2-0-0") == NULL);

    cJSON_Delete(document);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    fr_http_set_backend(previous);
    remove_plugin_cache(owner);
    PASS();
}

TEST a_cached_plugin_is_used_without_touching_the_network(void) {
    fr_error err;
    char owner[64];
    snprintf(owner, sizeof owner, "daukle-test-%d-cache", fr_test_process_id());
    const char *name = "remote";
    remove_plugin_cache(owner);

    char coordinate[256];
    remote_coordinate(coordinate, sizeof coordinate, owner, name);
    cJSON *document = cJSON_Parse(coordinate);

    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));

    fr_http_fn previous = fr_http_set_backend(stub_releases);
    release_requests = 0;
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    ASSERT_EQ(FR_OK, fr_plugins_load(registry, document, ".", &err));
    ASSERT(release_requests > 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();

    fr_registry *second = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&second, &err));
    release_requests = 0;
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", second, &err));
    ASSERT_EQ(FR_OK, fr_plugins_load(second, document, ".", &err));
    ASSERT_EQ(0, release_requests);
    ASSERT(fr_registry_language(second, "daukle.language/remote-1-2-0") != NULL);

    cJSON_Delete(document);
    fr_registry_destroy(second);
    fr_lua_runtime_shutdown();
    fr_http_set_backend(previous);
    remove_plugin_cache(owner);
    PASS();
}

/* Its own stub rather than reusing stub_releases above: a single release with
   a single asset keeps the fetched body exactly this literal, so
   fr_sha256_hex over it is provably the digest the pin check will compute,
   with nothing else contributing to what gets hashed. */
static int stub_pin_release(const char *url, const fr_http_header *headers, size_t header_count,
                            char **out_body, size_t *out_length, fr_error *err) {
    (void) headers; (void) header_count; (void) err;
    if (strstr(url, "/releases") != NULL) {
        *out_body = copy_body("[{\"tag_name\":\"1.0.0\",\"assets\":"
                              "[{\"name\":\"plugin.lua\",\"browser_download_url\":\"https://x/plugin.lua\"}]}]",
                              out_length);
    } else {
        *out_body = copy_body("daukle.plugin{ api = 1, uses = {} }\n"
                              "daukle.language{ name = 'remote', apply = function() return '' end }\n",
                              out_length);
    }
    return FR_OK;
}

TEST a_matching_pin_loads_and_a_mismatched_one_fails_naming_both_digests(void) {
    fr_error err;
    fr_http_fn previous = fr_http_set_backend(stub_pin_release);

    const char *body = "daukle.plugin{ api = 1, uses = {} }\n"
                       "daukle.language{ name = 'remote', apply = function() return '' end }\n";
    char expected[65];
    fr_sha256_hex(body, strlen(body), expected);

    char owner_good[64];
    snprintf(owner_good, sizeof owner_good, "daukle-test-%d-pin-good", fr_test_process_id());
    remove_plugin_cache(owner_good);

    char good[320];
    snprintf(good, sizeof good,
             "{\"plugins\":{\"r\":{\"repo\":\"%s/remote\",\"version\":\"^1.0.0\","
             "\"sha256\":\"%s\"}}}", owner_good, expected);

    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));
    cJSON *ok_document = cJSON_Parse(good);
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    ASSERT_EQ(FR_OK, fr_plugins_load(registry, ok_document, ".", &err));
    cJSON_Delete(ok_document);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    remove_plugin_cache(owner_good);

    char owner_bad[64];
    snprintf(owner_bad, sizeof owner_bad, "daukle-test-%d-pin-bad", fr_test_process_id());
    remove_plugin_cache(owner_bad);

    const char *wrong = "0000000000000000000000000000000000000000000000000000000000000000";
    char bad[320];
    snprintf(bad, sizeof bad,
             "{\"plugins\":{\"r\":{\"repo\":\"%s/remote\",\"version\":\"^1.0.0\","
             "\"sha256\":\"%s\"}}}", owner_bad, wrong);

    fr_registry *second = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&second, &err));
    cJSON *bad_document = cJSON_Parse(bad);
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", second, &err));
    ASSERT_EQ(FR_ERR, fr_plugins_load(second, bad_document, ".", &err));
    ASSERT(strstr(err.message, expected) != NULL);
    ASSERT(strstr(err.message, "0000000000") != NULL);
    ASSERT(strstr(err.message, "r") != NULL);
    cJSON_Delete(bad_document);
    fr_registry_destroy(second);
    fr_lua_runtime_shutdown();
    remove_plugin_cache(owner_bad);

    fr_http_set_backend(previous);
    PASS();
}

/* PowerShell's Get-FileHash emits uppercase hex by default, and this project's
   primary toolchain is Windows, so an uppercase pin over the same bytes must
   still match the lowercase digest fr_sha256_hex computes. */
TEST an_uppercase_pin_still_matches_the_lowercase_digest(void) {
    fr_error err;
    fr_http_fn previous = fr_http_set_backend(stub_pin_release);

    const char *body = "daukle.plugin{ api = 1, uses = {} }\n"
                       "daukle.language{ name = 'remote', apply = function() return '' end }\n";
    char expected[65];
    fr_sha256_hex(body, strlen(body), expected);

    char uppercase[65];
    for (size_t index = 0; index < 64; index++) {
        uppercase[index] = (char) toupper((unsigned char) expected[index]);
    }
    uppercase[64] = '\0';

    char owner[64];
    snprintf(owner, sizeof owner, "daukle-test-%d-pin-upper", fr_test_process_id());
    remove_plugin_cache(owner);

    char good[320];
    snprintf(good, sizeof good,
             "{\"plugins\":{\"r\":{\"repo\":\"%s/remote\",\"version\":\"^1.0.0\","
             "\"sha256\":\"%s\"}}}", owner, uppercase);

    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));
    cJSON *document = cJSON_Parse(good);
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    ASSERT_EQ(FR_OK, fr_plugins_load(registry, document, ".", &err));
    ASSERT(fr_registry_language(registry, "daukle.language/remote") != NULL);

    cJSON_Delete(document);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    remove_plugin_cache(owner);
    fr_http_set_backend(previous);
    PASS();
}

TEST config_print_reports_each_plugin_with_its_verbs_and_digest(void) {
    fr_error err;
    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));

    fr_manifest manifest;
    ASSERT_EQ(FR_OK, fr_config_load_file("test/fixtures/plugin-local/daukle.toml",
                                         registry, &manifest, &err));

    const fr_plugin_report *report = fr_plugins_report();
    ASSERT(report != NULL);
    ASSERT_EQ(1, (int) report->count);
    ASSERT_STR_EQ("hello", report->entries[0].label);
    ASSERT_EQ(64, (int) strlen(report->entries[0].sha256));

    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    fr_plugins_report_clear();
    PASS();
}

TEST the_report_names_a_plugins_declared_verbs(void) {
    fr_error err;
    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));

    fr_manifest manifest;
    ASSERT_EQ(FR_OK, fr_config_load_file("test/fixtures/plugin-declared-verb/daukle.toml",
                                         registry, &manifest, &err));

    const fr_plugin_report *report = fr_plugins_report();
    ASSERT_EQ(1, (int) report->count);
    ASSERT_STR_EQ("brew", report->entries[0].label);
    ASSERT_EQ(FR_PLUGIN_LOCAL, report->entries[0].kind);
    ASSERT_EQ(1, (int) report->entries[0].uses_count);
    ASSERT_STR_EQ("env", report->entries[0].uses[0]);

    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    fr_plugins_report_clear();
    PASS();
}

/* The report is read only after the entries fr_plugins_load parsed, the
   manifest built from them, the registry and the lua runtime are all freed:
   under a sanitizer this is exactly the sequence that would surface a report
   holding borrowed pointers rather than its own copies. */
TEST the_report_survives_the_entries_it_describes_being_freed(void) {
    fr_error err;
    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));

    fr_manifest manifest;
    ASSERT_EQ(FR_OK, fr_config_load_file("test/fixtures/plugin-local/daukle.toml",
                                         registry, &manifest, &err));

    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();

    const fr_plugin_report *report = fr_plugins_report();
    ASSERT_EQ(1, (int) report->count);
    ASSERT_STR_EQ("hello", report->entries[0].label);
    ASSERT(report->entries[0].resolved != NULL);
    ASSERT_EQ(64, (int) strlen(report->entries[0].sha256));

    fr_plugins_report_clear();
    PASS();
}

TEST fr_plugins_report_clear_empties_the_report(void) {
    fr_error err;
    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));

    fr_manifest manifest;
    ASSERT_EQ(FR_OK, fr_config_load_file("test/fixtures/plugin-local/daukle.toml",
                                         registry, &manifest, &err));
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();

    fr_plugins_report_clear();
    const fr_plugin_report *report = fr_plugins_report();
    ASSERT(report != NULL);
    ASSERT_EQ(0, (int) report->count);
    PASS();
}

TEST the_report_names_the_resolved_version_for_a_remote_plugin(void) {
    fr_error err;
    char owner[64];
    snprintf(owner, sizeof owner, "daukle-test-%d-report", fr_test_process_id());
    const char *name = "remote";
    remove_plugin_cache(owner);

    release_requests = 0;
    fr_http_fn previous = fr_http_set_backend(stub_releases);

    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));

    char coordinate[256];
    remote_coordinate(coordinate, sizeof coordinate, owner, name);
    cJSON *document = cJSON_Parse(coordinate);

    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    ASSERT_EQ(FR_OK, fr_plugins_load(registry, document, ".", &err));

    const fr_plugin_report *report = fr_plugins_report();
    ASSERT_EQ(1, (int) report->count);
    ASSERT_EQ(FR_PLUGIN_REMOTE, report->entries[0].kind);
    ASSERT_STR_EQ("1.2.0", report->entries[0].resolved);

    cJSON_Delete(document);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    fr_http_set_backend(previous);
    remove_plugin_cache(owner);
    fr_plugins_report_clear();
    PASS();
}

TEST fr_plugins_remove_cache_of_an_uncached_repo_is_not_an_error(void) {
    fr_error err;
    ASSERT_EQ(FR_OK, fr_plugins_remove_cache("daukle-test-nobody/nothing-here-ever", &err));
    PASS();
}

TEST fr_plugins_remove_cache_forces_the_next_resolve_to_refetch(void) {
    fr_error err;
    char owner[64];
    snprintf(owner, sizeof owner, "daukle-test-%d-remove-cache", fr_test_process_id());
    const char *name = "remote";
    remove_plugin_cache(owner);

    char coordinate[256];
    remote_coordinate(coordinate, sizeof coordinate, owner, name);
    cJSON *document = cJSON_Parse(coordinate);

    fr_http_fn previous = fr_http_set_backend(stub_releases);

    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));
    release_requests = 0;
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    ASSERT_EQ(FR_OK, fr_plugins_load(registry, document, ".", &err));
    ASSERT(release_requests > 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    fr_plugins_report_clear();

    char repo[128];
    snprintf(repo, sizeof repo, "%s/%s", owner, name);
    ASSERT_EQ(FR_OK, fr_plugins_remove_cache(repo, &err));

    fr_registry *second = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&second, &err));
    release_requests = 0;
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", second, &err));
    ASSERT_EQ(FR_OK, fr_plugins_load(second, document, ".", &err));
    ASSERT(release_requests > 0);

    cJSON_Delete(document);
    fr_registry_destroy(second);
    fr_lua_runtime_shutdown();
    fr_http_set_backend(previous);
    remove_plugin_cache(owner);
    fr_plugins_report_clear();
    PASS();
}

/* This is the property "daukle plugin update" with no label must have: the plugin
   cache root is shared across every project on the machine, so removing the
   cache for entries a manifest declares must never reach a repo it does not,
   the way the earlier fr_plugins_remove_all_cache implementation did. "o" here
   stands in for a plugin some OTHER project cached, never named in the
   manifest entries this call is given. */
TEST fr_plugins_update_cache_with_no_label_touches_only_the_given_entries(void) {
    fr_error err;
    char owner[64];
    snprintf(owner, sizeof owner, "daukle-test-%d-scope", fr_test_process_id());
    remove_plugin_cache(owner);

    fr_http_fn previous = fr_http_set_backend(stub_releases);

    /* Cached with two separate fr_plugins_load calls, each its own registry:
       both fetch the same stub body, which registers one fixed language name,
       and loading them together in one call would collide on that name. Two
       calls sidestep that and are still enough to cache both repos. */
    char declared_only[256];
    snprintf(declared_only, sizeof declared_only, "{\"plugins\":{\"d\":\"%s/declared@^1.0.0\"}}",
            owner);
    char other_only[256];
    snprintf(other_only, sizeof other_only, "{\"plugins\":{\"o\":\"%s/other@^1.0.0\"}}", owner);

    cJSON *seed_declared = cJSON_Parse(declared_only);
    fr_registry *seed_registry_d = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&seed_registry_d, &err));
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", seed_registry_d, &err));
    ASSERT_EQ(FR_OK, fr_plugins_load(seed_registry_d, seed_declared, ".", &err));
    cJSON_Delete(seed_declared);
    fr_registry_destroy(seed_registry_d);
    fr_lua_runtime_shutdown();
    fr_plugins_report_clear();

    cJSON *seed_other = cJSON_Parse(other_only);
    fr_registry *seed_registry_o = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&seed_registry_o, &err));
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", seed_registry_o, &err));
    ASSERT_EQ(FR_OK, fr_plugins_load(seed_registry_o, seed_other, ".", &err));
    cJSON_Delete(seed_other);
    fr_registry_destroy(seed_registry_o);
    fr_lua_runtime_shutdown();
    fr_plugins_report_clear();

    cJSON *declared_document = cJSON_Parse(declared_only);
    fr_plugin_entry *entries = NULL;
    size_t count = 0;
    ASSERT_EQ(FR_OK, fr_plugins_parse(declared_document, &entries, &count, &err));
    ASSERT_EQ(1, (int) count);

    ASSERT_EQ(FR_OK, fr_plugins_update_cache(entries, count, NULL, &err));

    fr_plugins_free(entries, count);
    cJSON_Delete(declared_document);

    /* "d" was in the entries fr_plugins_update_cache was given: its cache must
       be gone, so loading it again reaches the network. */
    cJSON *reload_declared = cJSON_Parse(declared_only);
    fr_registry *second = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&second, &err));
    release_requests = 0;
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", second, &err));
    ASSERT_EQ(FR_OK, fr_plugins_load(second, reload_declared, ".", &err));
    ASSERT(release_requests > 0);
    cJSON_Delete(reload_declared);
    fr_registry_destroy(second);
    fr_lua_runtime_shutdown();
    fr_plugins_report_clear();

    /* "o" was never in the entries fr_plugins_update_cache was given: its cache
       must survive, so loading it again makes zero requests. */
    cJSON *reload_other = cJSON_Parse(other_only);
    fr_registry *third = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&third, &err));
    release_requests = 0;
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", third, &err));
    ASSERT_EQ(FR_OK, fr_plugins_load(third, reload_other, ".", &err));
    ASSERT_EQ(0, release_requests);
    cJSON_Delete(reload_other);
    fr_registry_destroy(third);
    fr_lua_runtime_shutdown();
    fr_plugins_report_clear();

    fr_http_set_backend(previous);
    remove_plugin_cache(owner);
    PASS();
}

TEST fr_plugins_update_cache_with_an_unknown_label_errors_naming_it(void) {
    fr_error err;
    cJSON *document = cJSON_Parse("{\"plugins\":{\"npm\":\"daukle/npm@^1.0.0\"}}");
    fr_plugin_entry *entries = NULL;
    size_t count = 0;
    ASSERT_EQ(FR_OK, fr_plugins_parse(document, &entries, &count, &err));

    ASSERT_EQ(FR_ERR, fr_plugins_update_cache(entries, count, "gradle", &err));
    ASSERT(strstr(err.message, "gradle") != NULL);

    fr_plugins_free(entries, count);
    cJSON_Delete(document);
    PASS();
}

TEST fr_plugins_update_cache_with_a_label_matching_a_local_entry_is_not_an_error(void) {
    fr_error err;
    cJSON *document = cJSON_Parse("{\"plugins\":{\"hello\":\"./plugins/hello.lua\"}}");
    fr_plugin_entry *entries = NULL;
    size_t count = 0;
    ASSERT_EQ(FR_OK, fr_plugins_parse(document, &entries, &count, &err));

    ASSERT_EQ(FR_OK, fr_plugins_update_cache(entries, count, "hello", &err));

    fr_plugins_free(entries, count);
    cJSON_Delete(document);
    PASS();
}

TEST a_local_plugin_with_a_matching_pin_loads_and_a_wrong_one_fails_naming_both_digests(void) {
    fr_error err;
    char *fixture_text = NULL;
    ASSERT_EQ(FR_OK, fr_file_read_text("test/fixtures/plugin-local/plugins/hello.lua",
                                       &fixture_text, &err));
    char expected[65];
    fr_sha256_hex(fixture_text, strlen(fixture_text), expected);
    free(fixture_text);

    char good[256];
    snprintf(good, sizeof good,
             "{\"plugins\":{\"hello\":{\"path\":\"./plugins/hello.lua\",\"sha256\":\"%s\"}}}",
             expected);

    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));
    cJSON *ok_document = cJSON_Parse(good);
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin("test/fixtures/plugin-local", registry, &err));
    ASSERT_EQ(FR_OK, fr_plugins_load(registry, ok_document, "test/fixtures/plugin-local", &err));
    ASSERT(fr_registry_language(registry, "daukle.language/hello") != NULL);
    cJSON_Delete(ok_document);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();

    const char *wrong = "1111111111111111111111111111111111111111111111111111111111111111";
    char bad[256];
    snprintf(bad, sizeof bad,
             "{\"plugins\":{\"hello\":{\"path\":\"./plugins/hello.lua\",\"sha256\":\"%s\"}}}",
             wrong);

    fr_registry *second = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&second, &err));
    cJSON *bad_document = cJSON_Parse(bad);
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin("test/fixtures/plugin-local", second, &err));
    ASSERT_EQ(FR_ERR, fr_plugins_load(second, bad_document, "test/fixtures/plugin-local", &err));
    ASSERT(strstr(err.message, expected) != NULL);
    ASSERT(strstr(err.message, wrong) != NULL);
    ASSERT(strstr(err.message, "hello") != NULL);
    cJSON_Delete(bad_document);
    fr_registry_destroy(second);
    fr_lua_runtime_shutdown();
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(parses_the_string_coordinate_form);
    RUN_TEST(parses_the_table_form_with_a_pin);
    RUN_TEST(parses_a_local_path);
    RUN_TEST(a_table_entry_naming_both_path_and_repo_is_refused);
    RUN_TEST(a_table_entry_naming_neither_path_nor_repo_is_refused);
    RUN_TEST(an_absent_plugins_table_yields_no_entries);
    RUN_TEST(rejects_a_coordinate_with_no_version);
    RUN_TEST(rejects_a_plugins_member_that_is_not_a_table);
    RUN_TEST(rejects_a_coordinate_with_an_empty_half);
    RUN_TEST(a_fetched_manifest_may_not_declare_plugins);
    RUN_TEST(a_fetched_manifest_without_plugins_is_accepted);
    RUN_TEST(fr_project_parse_refuses_a_fetched_manifest_declaring_plugins);
    RUN_TEST(a_local_plugin_registers_its_language);
    RUN_TEST(a_verb_a_plugin_declared_is_there_when_it_runs);
    RUN_TEST(a_manifest_declaring_no_plugins_opens_no_lua_state);
    RUN_TEST(a_verb_uses_does_not_know_is_refused);
    RUN_TEST(a_plugin_written_against_a_later_api_says_which);
    RUN_TEST(a_uses_entry_that_is_not_a_string_is_refused);
    RUN_TEST(a_verb_used_before_the_declaration_says_so);
    RUN_TEST(a_remote_coordinate_picks_the_highest_release_in_range);
    RUN_TEST(a_cached_plugin_is_used_without_touching_the_network);
    RUN_TEST(a_matching_pin_loads_and_a_mismatched_one_fails_naming_both_digests);
    RUN_TEST(an_uppercase_pin_still_matches_the_lowercase_digest);
    RUN_TEST(config_print_reports_each_plugin_with_its_verbs_and_digest);
    RUN_TEST(the_report_names_a_plugins_declared_verbs);
    RUN_TEST(the_report_survives_the_entries_it_describes_being_freed);
    RUN_TEST(fr_plugins_report_clear_empties_the_report);
    RUN_TEST(the_report_names_the_resolved_version_for_a_remote_plugin);
    RUN_TEST(fr_plugins_remove_cache_of_an_uncached_repo_is_not_an_error);
    RUN_TEST(fr_plugins_remove_cache_forces_the_next_resolve_to_refetch);
    RUN_TEST(fr_plugins_update_cache_with_no_label_touches_only_the_given_entries);
    RUN_TEST(fr_plugins_update_cache_with_an_unknown_label_errors_naming_it);
    RUN_TEST(fr_plugins_update_cache_with_a_label_matching_a_local_entry_is_not_an_error);
    RUN_TEST(a_local_plugin_with_a_matching_pin_loads_and_a_wrong_one_fails_naming_both_digests);
    GREATEST_MAIN_END();
}
