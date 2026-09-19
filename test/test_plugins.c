#include "greatest.h"

#include "cJSON.h"
#include "cache.h"
#include "config.h"
#include "config_lua.h"
#include "http.h"
#include "manifest.h"
#include "plugins.h"
#include "registry.h"
#include "support.h"
#include "sync.h"

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
    } else {
        *out_body = copy_body("daukle.plugin{ api = 1, uses = {} }\n"
                              "daukle.language{ name = 'remote', apply = function() return '' end }\n",
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
    ASSERT(fr_registry_language(registry, "daukle.language/remote") != NULL);

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
    ASSERT(fr_registry_language(second, "daukle.language/remote") != NULL);

    cJSON_Delete(document);
    fr_registry_destroy(second);
    fr_lua_runtime_shutdown();
    fr_http_set_backend(previous);
    remove_plugin_cache(owner);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(parses_the_string_coordinate_form);
    RUN_TEST(parses_the_table_form_with_a_pin);
    RUN_TEST(parses_a_local_path);
    RUN_TEST(an_absent_plugins_table_yields_no_entries);
    RUN_TEST(rejects_a_coordinate_with_no_version);
    RUN_TEST(rejects_a_plugins_member_that_is_not_a_table);
    RUN_TEST(rejects_a_coordinate_with_an_empty_half);
    RUN_TEST(a_local_plugin_registers_its_language);
    RUN_TEST(a_verb_a_plugin_declared_is_there_when_it_runs);
    RUN_TEST(a_manifest_declaring_no_plugins_opens_no_lua_state);
    RUN_TEST(a_verb_uses_does_not_know_is_refused);
    RUN_TEST(a_plugin_written_against_a_later_api_says_which);
    RUN_TEST(a_uses_entry_that_is_not_a_string_is_refused);
    RUN_TEST(a_verb_used_before_the_declaration_says_so);
    RUN_TEST(a_remote_coordinate_picks_the_highest_release_in_range);
    RUN_TEST(a_cached_plugin_is_used_without_touching_the_network);
    GREATEST_MAIN_END();
}
