#include "greatest.h"

#include "cJSON.h"
#include "cache.h"
#include "config.h"
#include "config_lua.h"
#include "config_toml.h"
#include "error.h"
#include "http.h"
#include "manifest.h"
#include "plugin_fetch.h"
#include "plugins.h"
#include "region.h"
#include "registry.h"
#include "resolvers.h"
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

TEST parses_the_string_url_form(void) {
    cJSON *document = document_from("{\"plugins\":{\"remote\":\"https://example.invalid/p.lua\"}}");
    fr_plugin_entry *entries = NULL; size_t count = 0; fr_error err;

    ASSERT_EQ(FR_OK, fr_plugins_parse(document, NULL, 0, &entries, &count, &err));
    ASSERT_EQ(1, (int) count);
    ASSERT_STR_EQ("remote", entries[0].label);
    ASSERT_EQ(FR_PLUGIN_URL, entries[0].kind);
    ASSERT_STR_EQ("https://example.invalid/p.lua", entries[0].url);
    ASSERT(entries[0].sha256 == NULL);

    fr_plugins_free(entries, count);
    cJSON_Delete(document);
    PASS();
}

TEST parses_the_table_form_with_a_pin(void) {
    cJSON *document = document_from(
        "{\"plugins\":{\"gradle\":{\"url\":\"https://example.invalid/g.lua\","
        "\"sha256\":\"abc123\"}}}");
    fr_plugin_entry *entries = NULL; size_t count = 0; fr_error err;

    ASSERT_EQ(FR_OK, fr_plugins_parse(document, NULL, 0, &entries, &count, &err));
    ASSERT_EQ(1, (int) count);
    ASSERT_EQ(FR_PLUGIN_URL, entries[0].kind);
    ASSERT_STR_EQ("https://example.invalid/g.lua", entries[0].url);
    ASSERT_STR_EQ("abc123", entries[0].sha256);

    fr_plugins_free(entries, count);
    cJSON_Delete(document);
    PASS();
}

TEST parses_the_table_form_naming_a_resolver_and_a_coordinate(void) {
    cJSON *document = document_from(
        "{\"plugins\":{\"g\":{\"resolver\":\"maven\",\"coordinate\":\"org.example:widget\"}}}");
    fr_plugin_entry *entries = NULL; size_t count = 0; fr_error err;

    ASSERT_EQ(FR_OK, fr_plugins_parse(document, NULL, 0, &entries, &count, &err));
    ASSERT_EQ(1, (int) count);
    ASSERT_EQ(FR_PLUGIN_RESOLVED, entries[0].kind);
    ASSERT_STR_EQ("maven", entries[0].resolver);
    ASSERT_STR_EQ("org.example:widget", entries[0].coordinate);

    fr_plugins_free(entries, count);
    cJSON_Delete(document);
    PASS();
}

TEST parses_a_local_path(void) {
    cJSON *document = document_from("{\"plugins\":{\"mine\":\"./plugins/mine.lua\"}}");
    fr_plugin_entry *entries = NULL; size_t count = 0; fr_error err;

    ASSERT_EQ(FR_OK, fr_plugins_parse(document, NULL, 0, &entries, &count, &err));
    ASSERT_EQ(FR_PLUGIN_PATH, entries[0].kind);
    ASSERT_STR_EQ("./plugins/mine.lua", entries[0].path);

    fr_plugins_free(entries, count);
    cJSON_Delete(document);
    PASS();
}

TEST a_table_entry_naming_two_forms_is_refused(void) {
    cJSON *document = document_from(
        "{\"plugins\":{\"two\":{\"path\":\"./x.lua\",\"url\":\"https://example.invalid/x.lua\"}}}");
    fr_plugin_entry *entries = NULL; size_t count = 0; fr_error err;

    ASSERT_EQ(FR_ERR, fr_plugins_parse(document, NULL, 0, &entries, &count, &err));
    ASSERT(strstr(err.message, "not several") != NULL);

    cJSON_Delete(document);
    PASS();
}

TEST a_table_entry_naming_no_form_is_refused(void) {
    cJSON *document = document_from("{\"plugins\":{\"none\":{\"sha256\":\"abc123\"}}}");
    fr_plugin_entry *entries = NULL; size_t count = 0; fr_error err;

    ASSERT_EQ(FR_ERR, fr_plugins_parse(document, NULL, 0, &entries, &count, &err));
    ASSERT(strstr(err.message, "needs one of") != NULL);

    cJSON_Delete(document);
    PASS();
}

TEST a_table_entry_naming_a_resolver_without_a_coordinate_is_refused(void) {
    cJSON *document = document_from("{\"plugins\":{\"half\":{\"resolver\":\"maven\"}}}");
    fr_plugin_entry *entries = NULL; size_t count = 0; fr_error err;

    ASSERT_EQ(FR_ERR, fr_plugins_parse(document, NULL, 0, &entries, &count, &err));
    ASSERT(strstr(err.message, "\"coordinate\" is missing") != NULL);

    cJSON_Delete(document);
    PASS();
}

TEST an_absent_plugins_table_yields_no_entries(void) {
    cJSON *document = document_from("{\"schema\":1}");
    fr_plugin_entry *entries = NULL; size_t count = 0; fr_error err;

    ASSERT_EQ(FR_OK, fr_plugins_parse(document, NULL, 0, &entries, &count, &err));
    ASSERT_EQ(0, (int) count);

    fr_plugins_free(entries, count);
    cJSON_Delete(document);
    PASS();
}

TEST the_old_bare_coordinate_form_names_the_fix(void) {
    fr_error err;
    fr_plugin_entry *entries = NULL;
    size_t count = 0;
    cJSON *document = cJSON_Parse("{\"plugins\":{\"npm\":\"daukle/npm@^1.0.0\"}}");
    int status = fr_plugins_parse(document, NULL, 0, &entries, &count, &err);
    char message[512];
    snprintf(message, sizeof message, "%s", err.message);
    cJSON_Delete(document);
    fr_plugins_free(entries, count);

    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "names no resolver") != NULL);
    ASSERT(strstr(message, "Declared resolvers: none") != NULL);
    PASS();
}

TEST the_declared_resolvers_are_named_when_a_value_names_none(void) {
    fr_error err;
    fr_plugin_entry *entries = NULL;
    size_t count = 0;
    cJSON *document = cJSON_Parse("{\"plugins\":{\"npm\":\"daukle/npm@^1.0.0\"}}");
    fr_resolver_entry resolvers[] = { { (char *) "maven", NULL, (char *) "./r.lua", NULL, NULL },
                                      { (char *) "forge", NULL, (char *) "./f.lua", NULL, NULL } };
    int status = fr_plugins_parse(document, resolvers, 2, &entries, &count, &err);
    char message[512];
    snprintf(message, sizeof message, "%s", err.message);
    cJSON_Delete(document);
    fr_plugins_free(entries, count);

    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "Declared resolvers: maven, forge") != NULL);
    PASS();
}

/* The list is bounded, so a manifest with many resolvers has to be cut. It is
   cut after a whole label and says so, because a name ending mid-word reads as
   a resolver the reader does not have rather than as a list that ran out. */
TEST an_overlong_resolver_list_is_cut_after_a_whole_label(void) {
    fr_error err;
    fr_plugin_entry *entries = NULL;
    size_t count = 0;
    char labels[40][16];
    fr_resolver_entry resolvers[40];
    memset(resolvers, 0, sizeof resolvers);
    for (size_t index = 0; index < 40; index++) {
        snprintf(labels[index], sizeof labels[index], "resolver-%02zu", index);
        resolvers[index].label = labels[index];
    }

    cJSON *document = cJSON_Parse("{\"plugins\":{\"npm\":\"daukle/npm@^1.0.0\"}}");
    int status = fr_plugins_parse(document, resolvers, 40, &entries, &count, &err);
    char message[512];
    snprintf(message, sizeof message, "%s", err.message);
    cJSON_Delete(document);
    fr_plugins_free(entries, count);

    const char *list = strstr(message, "Declared resolvers: ");
    size_t list_length = list != NULL ? strlen(list) : 0;
    /* Every label ends in a digit, so the character before the cut marker is a
       digit only when a whole label survived. */
    char before_the_cut = list_length >= 6 ? list[list_length - 6] : '\0';

    ASSERT_EQ(FR_ERR, status);
    ASSERT(list != NULL);
    ASSERT(strstr(list, "resolver-00, resolver-01") != NULL);
    ASSERT_STR_EQ(", ...", list + list_length - 5);
    ASSERT(isdigit((unsigned char) before_the_cut));
    PASS();
}

TEST rejects_a_plugins_member_that_is_not_a_table(void) {
    cJSON *document = document_from("{\"plugins\":[1,2]}");
    fr_plugin_entry *entries = NULL; size_t count = 0; fr_error err;

    ASSERT_EQ(FR_ERR, fr_plugins_parse(document, NULL, 0, &entries, &count, &err));
    ASSERT(strstr(err.message, "plugins") != NULL);

    cJSON_Delete(document);
    PASS();
}

TEST rejects_a_string_with_an_empty_half_around_the_colon(void) {
    const char *bad[] = { "{\"plugins\":{\"a\":\":\"}}",
                          "{\"plugins\":{\"a\":\"maven:\"}}",
                          "{\"plugins\":{\"a\":\":org.example\"}}" };
    for (size_t index = 0; index < 3; index++) {
        cJSON *document = document_from(bad[index]);
        fr_plugin_entry *entries = NULL; size_t count = 0; fr_error err;
        ASSERT_EQ(FR_ERR, fr_plugins_parse(document, NULL, 0, &entries, &count, &err));
        ASSERT(strstr(err.message, "names no resolver") != NULL);
        cJSON_Delete(document);
    }
    PASS();
}

TEST a_windows_drive_letter_string_is_a_path(void) {
    fr_error err;
    fr_plugin_entry *entries = NULL;
    size_t count = 0;
    cJSON *document = cJSON_Parse("{\"plugins\":{\"p\":\"C:/plugins/mine.lua\"}}");
    int status = fr_plugins_parse(document, NULL, 0, &entries, &count, &err);
    int kind = (status == FR_OK && count == 1) ? (int) entries[0].kind : -1;
    char path[256] = "";
    if (status == FR_OK && count == 1 && entries[0].path != NULL) {
        snprintf(path, sizeof path, "%s", entries[0].path);
    }
    cJSON_Delete(document);
    fr_plugins_free(entries, count);

    ASSERT_EQ(FR_OK, status);
    ASSERT_EQ((int) FR_PLUGIN_PATH, kind);
    ASSERT_STR_EQ("C:/plugins/mine.lua", path);
    PASS();
}

TEST a_coordinate_containing_colons_reaches_the_resolver_whole(void) {
    fr_error err;
    fr_plugin_entry *entries = NULL;
    size_t count = 0;
    cJSON *document = cJSON_Parse("{\"plugins\":{\"p\":\"maven:org.example:widget:1.2.3\"}}");
    fr_resolver_entry resolver = { (char *) "maven", NULL, (char *) "./r.lua", NULL, NULL };
    int status = fr_plugins_parse(document, &resolver, 1, &entries, &count, &err);
    char label[64] = "";
    char coordinate[128] = "";
    if (status == FR_OK && count == 1 && entries[0].kind == FR_PLUGIN_RESOLVED) {
        snprintf(label, sizeof label, "%s", entries[0].resolver);
        snprintf(coordinate, sizeof coordinate, "%s", entries[0].coordinate);
    }
    cJSON_Delete(document);
    fr_plugins_free(entries, count);

    ASSERT_EQ(FR_OK, status);
    ASSERT_STR_EQ("maven", label);
    ASSERT_STR_EQ("org.example:widget:1.2.3", coordinate);
    PASS();
}

TEST a_fetched_manifest_may_not_declare_plugins(void) {
    cJSON *document = document_from(
        "{\"schema\":1,\"project\":\"forebay/evil\",\"version\":\"1.0.0\","
        "\"plugins\":{\"x\":\"https://example.invalid/x.lua\"}}");
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
                       "\"plugins\":{\"x\":\"https://example.invalid/x.lua\"}}";

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

/* Registration into the language table is not emission through it: this runs
   the whole sync pass and reads what the plugin's apply actually wrote. */
TEST sync_writes_through_a_plugin_registered_language(void) {
    fr_error err;
    fr_sync_report report;
    ASSERT_EQ(FR_OK, fr_sync("test/fixtures/plugin-local/daukle.toml", 1, 1, &report, &err));
    fr_sync_report_free(&report);

    char *written = NULL;
    ASSERT_EQ(FR_OK, fr_file_read_text("test/fixtures/plugin-local/hello.txt", &written, &err));
    ASSERT_STR_EQ("forebay/basekit/contracts\nforebay/basekit/ir\n", written);
    free(written);
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
    fr_lua_runtime_shutdown();

    fr_error err;
    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));

    fr_manifest manifest;
    ASSERT_EQ(FR_OK, fr_config_load_file("test/fixtures/consumer/daukle-unknown-source.toml",
                                         registry, &manifest, &err));

    ASSERT(fr_lua_runtime_state() == NULL);

    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST a_language_plugin_may_not_declare_exec(void) {
    fr_error err;
    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));

    fr_manifest manifest;
    ASSERT_EQ(FR_ERR, fr_config_load_file("test/fixtures/plugin-exec-refused/daukle.toml",
                                          registry, &manifest, &err));
    ASSERT(strstr(err.message, "daukle.exec is available only to a toolchain plugin") != NULL);

    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST a_source_plugin_may_not_declare_exec(void) {
    fr_error err;
    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));

    fr_manifest manifest;
    ASSERT_EQ(FR_ERR, fr_config_load_file("test/fixtures/plugin-exec-refused-source/daukle.toml",
                                          registry, &manifest, &err));
    ASSERT(strstr(err.message, "daukle.exec is available only to a toolchain plugin") != NULL);

    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

/* The gate at daukle.language and daukle.source fires only once a plugin says
   what kind it is, by which time a plugin that execs at the top of its chunk
   has already run the program. The refusal has to reach the call itself. */
TEST a_plugin_execing_before_it_declares_is_refused_at_the_call(void) {
    fr_error err;
    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));

    fr_manifest manifest;
    ASSERT_EQ(FR_ERR, fr_config_load_file("test/fixtures/plugin-exec-before-declaring/daukle.toml",
                                          registry, &manifest, &err));
    ASSERT(strstr(err.message, "daukle.exec is available only to a toolchain plugin") != NULL);
    ASSERT(strstr(err.message, "must be a tool handle") == NULL);

    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

/* daukle.tool alone must still load: the refusal is exec's alone, not tool's. */
TEST a_language_plugin_declaring_tool_but_not_exec_still_loads(void) {
    fr_error err;
    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));

    fr_manifest manifest;
    ASSERT_EQ(FR_OK, fr_config_load_file("test/fixtures/plugin-tool-only/daukle.toml",
                                         registry, &manifest, &err));
    ASSERT(fr_registry_language(registry, "daukle.language/ok") != NULL);

    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
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

/* read_declaration's first line touches the lua_State it is given, so a caller
   reaching fr_plugins_read_uses with no runtime open (resolvers.c's acquire,
   before fr_lua_plugin_load, is the one that matters) needs a real error back,
   not a crash. No runtime is open here between tests, so this needs no setup. */
TEST fr_plugins_read_uses_rejects_a_closed_runtime(void) {
    fr_error err;
    char **uses = NULL;
    size_t uses_count = 0;
    int status = fr_plugins_read_uses("daukle.plugin{ api = 1 }", "test.lua", "resolver", "t",
                                      &uses, &uses_count, &err);

    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(err.message, "no lua runtime is open") != NULL);
    ASSERT(uses == NULL);
    ASSERT_EQ(0u, uses_count);
    PASS();
}

static char AUTH_VALUE_SEEN[256];
static int AUTH_HEADER_PRESENT = 0;

static int stub_release_asset(const char *url, const fr_http_header *headers, size_t header_count,
                              char **out_body, size_t *out_length, fr_error *err) {
    (void) url;
    AUTH_HEADER_PRESENT = 0;
    AUTH_VALUE_SEEN[0] = '\0';
    for (size_t index = 0; index < header_count; index++) {
        if (strcmp(headers[index].name, "Authorization") != 0) continue;
        AUTH_HEADER_PRESENT = 1;
        snprintf(AUTH_VALUE_SEEN, sizeof AUTH_VALUE_SEEN, "%s", headers[index].value);
    }

    char *text = NULL;
    if (fr_file_read_text("test/fixtures/consumer/producer/daukle.toml", &text, err) != FR_OK) {
        return FR_ERR;
    }
    *out_length = strlen(text);
    *out_body = text;
    return FR_OK;
}

static const char *GITHUB_BLOCK =
    "{\"kind\":\"github-releases\",\"repo\":\"forebay/basekit\",\"version\":\"5.0.0\"}";

/* Loads the shipped github plugin from the consumer fixture's copy and resolves
   one source block through it. The toml format is registered because the plugin
   hands the body it fetched to daukle.parse, and the cache is off so every call
   reaches the stub instead of the entry the previous call would have written. */
static int github_source_load(const char *block_json, fr_error *err) {
    int cache_was_enabled = fr_cache_enabled();
    fr_cache_set_enabled(0);
    fr_http_fn previous = fr_http_set_backend(stub_release_asset);

    fr_registry *registry = fr_registry_create();
    cJSON *document = cJSON_Parse("{\"plugins\":{\"github\":\"./plugins/github.lua\"}}");
    int status = FR_ERR;
    if (fr_registry_add_config(registry, &FR_CONFIG_TOML, err) == FR_OK
        && fr_lua_runtime_begin("test/fixtures/consumer", registry, err) == FR_OK
        && fr_plugins_load(registry, document, "test/fixtures/consumer", err) == FR_OK) {
        const fr_source_plugin *source =
            fr_registry_source(registry, "daukle.source/github-releases");
        cJSON *block = cJSON_Parse(block_json);
        fr_project resolved;
        if (source != NULL) {
            status = source->load(source->state, "forebay/basekit", block, ".", &resolved, err);
            if (status == FR_OK) fr_project_free(&resolved);
        } else {
            fr_error_set(err, "capability \"daukle.source/github-releases\" not found");
        }
        cJSON_Delete(block);
    }
    cJSON_Delete(document);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    fr_plugins_report_clear();

    fr_http_set_backend(previous);
    fr_cache_set_enabled(cache_was_enabled);
    return status;
}

static int github_release_fetch(const char *daukle_token, const char *github_token, fr_error *err) {
    fr_test_set_env("DAUKLE_TOKEN", daukle_token);
    fr_test_set_env("GITHUB_TOKEN", github_token);
    int status = github_source_load(GITHUB_BLOCK, err);
    fr_test_set_env("DAUKLE_TOKEN", NULL);
    fr_test_set_env("GITHUB_TOKEN", NULL);
    return status;
}

/* Both token variables are read in the plugin rather than in C now, so only
   what reaches the request proves the precedence, and that neither being set
   sends no header at all instead of an empty one. */
TEST the_github_plugin_authenticates_from_either_token_variable(void) {
    fr_error err;

    ASSERT_EQm(err.message, FR_OK, github_release_fetch("daukle-token", "github-token", &err));
    ASSERT_STR_EQ("Bearer daukle-token", AUTH_VALUE_SEEN);

    ASSERT_EQm(err.message, FR_OK, github_release_fetch(NULL, "github-token", &err));
    ASSERT_STR_EQ("Bearer github-token", AUTH_VALUE_SEEN);

    ASSERT_EQm(err.message, FR_OK, github_release_fetch(NULL, NULL, &err));
    ASSERT_FALSE(AUTH_HEADER_PRESENT);
    PASS();
}

/* "asset" and "tag" are optional, so an absent key is not an error and a
   non-string one is easy to leave unchecked. Left unchecked its only symptom is
   a concatenation failure inside the plugin, naming neither the field nor the
   manifest that wrote it, so the assertion is on the message and not merely on
   the refusal. */
TEST the_github_plugin_names_an_optional_field_that_is_not_a_string(void) {
    fr_error err;

    ASSERT_EQ(FR_ERR, github_source_load(
        "{\"kind\":\"github-releases\",\"repo\":\"forebay/basekit\",\"version\":\"5.0.0\","
        "\"asset\":3}", &err));
    ASSERT(strstr(err.message, "sources.forebay/basekit.asset") != NULL);
    ASSERT(strstr(err.message, "must be a string") != NULL);

    ASSERT_EQ(FR_ERR, github_source_load(
        "{\"kind\":\"github-releases\",\"repo\":\"forebay/basekit\",\"version\":\"5.0.0\","
        "\"tag\":{}}", &err));
    ASSERT(strstr(err.message, "sources.forebay/basekit.tag") != NULL);
    ASSERT(strstr(err.message, "must be a string") != NULL);
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

/* A resolver that answers from the coordinate alone, so a test needs no
   fixture file on disk to exercise the resolved form. */
static const char *INLINE_RESOLVER =
    "daukle.plugin{ api = 1, uses = {} }\n"
    "daukle.resolver{ resolve = function(c)\n"
    "  return { url = 'https://x/' .. c .. '.lua', resolved = c }\n"
    "end }\n";

/* The sha256 of INLINE_RESOLVER, computed over those exact bytes rather than
   guessed, the same arrangement test_resolvers.c uses for its fixture. */
#define INLINE_DIGEST "b01452e414d56fd9900067c3a9d1724fa4300fc35632addf8cb2c03052e76acd"

static const char *ARTIFACT_BODY =
    "daukle.plugin{ api = 1, uses = {} }\n"
    "daukle.language{ name = 'from-url', apply = function() return '' end }\n";

/* Which url was asked for, so a test can assert that the url the resolver
   named is the url that was fetched. Without it the stub answers every url
   alike and a fetch of the raw coordinate, or of a stale origin, would load
   just as happily. */
static char LAST_FETCHED_URL[256];

static int stub_inline(const char *url, const fr_http_header *headers, size_t header_count,
                       char **out_body, size_t *out_length, fr_error *err) {
    (void) headers; (void) header_count; (void) err;
    release_requests++;
    snprintf(LAST_FETCHED_URL, sizeof LAST_FETCHED_URL, "%s", url);
    *out_body = copy_body(strstr(url, "/resolver.lua") != NULL ? INLINE_RESOLVER : ARTIFACT_BODY,
                          out_length);
    return FR_OK;
}

/* Loads document_json with stub_inline installed, reports the load's status
   through out_status and returns whether the language a fetched artifact
   registers arrived. Every caller below needs the same setup and the same
   teardown, and a helper is how the teardown stops being the thing a new test
   forgets. */
static int loads_from_url(const char *document_json, int *out_status, fr_error *err) {
    fr_http_fn previous = fr_http_set_backend(stub_inline);
    fr_registry *registry = NULL;
    int registered = 0;
    *out_status = FR_ERR;

    if (fr_build_registry(&registry, err) == FR_OK) {
        cJSON *document = cJSON_Parse(document_json);
        if (fr_lua_runtime_begin(".", registry, err) == FR_OK) {
            *out_status = fr_plugins_load(registry, document, ".", err);
            registered = fr_registry_language(registry, "daukle.language/from-url") != NULL;
        }
        cJSON_Delete(document);
        fr_registry_destroy(registry);
        fr_lua_runtime_shutdown();
    }

    fr_plugins_report_clear();
    fr_resolvers_clear();
    fr_http_set_backend(previous);
    return registered;
}

/* The fetch cache is keyed by url and survives between runs, so a test that
   asserts WHICH url was fetched has to make every fetch reach the stub or
   LAST_FETCHED_URL is whatever the run before it happened to leave. */
static int loads_reaching_the_network(const char *document_json, int *out_status, fr_error *err) {
    int cache_was_enabled = fr_cache_enabled();
    fr_cache_set_enabled(0);
    LAST_FETCHED_URL[0] = '\0';
    int registered = loads_from_url(document_json, out_status, err);
    fr_cache_set_enabled(cache_was_enabled);
    return registered;
}

TEST a_url_entry_fetches_and_loads(void) {
    fr_error err;
    int status = FR_ERR;
    int registered = loads_reaching_the_network("{\"plugins\":{\"p\":\"https://x/plain.lua\"}}",
                                                &status, &err);
    ASSERT_EQm(err.message, FR_OK, status);
    ASSERT(registered);
    ASSERT_STR_EQ("https://x/plain.lua", LAST_FETCHED_URL);
    PASS();
}

/* The resolver returns "https://x/" .. coordinate .. ".lua", so the url that
   reaches the network is the one assertion that separates "the resolver ran"
   from "what the resolver named is what was fetched". */
TEST a_resolved_entry_loads_what_its_resolver_names(void) {
    fr_error err;
    int status = FR_ERR;
    int registered = loads_reaching_the_network(
        "{\"resolvers\":{\"t\":{\"url\":\"https://x/resolver.lua\",\"sha256\":\"" INLINE_DIGEST
        "\"}},\"plugins\":{\"p\":\"t:a/b\"}}", &status, &err);
    ASSERT_EQm(err.message, FR_OK, status);
    ASSERT(registered);
    ASSERT_STR_EQ("https://x/a/b.lua", LAST_FETCHED_URL);
    PASS();
}

/* Three releases, two of them in range for "^1.0.0": o.lua (1.1.0, in range
   but not the highest), p.lua (1.2.0, in range and the highest), and q.lua
   (2.0.0, out of range). Listing them out of numeric order (1.1.0, 2.0.0,
   1.2.0) means neither "take the first in-range entry" nor "take the last
   entry in the array" would accidentally land on the right answer; only
   comparing every in-range candidate with greater() does. Each registers a
   differently named language so the test can tell which one was fetched
   rather than only that loading succeeded. */
static int stub_releases_index(const char *url, const fr_http_header *headers, size_t header_count,
                               char **out_body, size_t *out_length, fr_error *err) {
    (void) headers; (void) header_count; (void) err;
    release_requests++;
    if (strstr(url, "/releases") != NULL) {
        *out_body = copy_body("[{\"tag_name\":\"1.1.0\",\"assets\":"
                              "[{\"name\":\"plugin.lua\",\"browser_download_url\":\"https://x/o.lua\"}]},"
                              "{\"tag_name\":\"2.0.0\",\"assets\":"
                              "[{\"name\":\"plugin.lua\",\"browser_download_url\":\"https://x/q.lua\"}]},"
                              "{\"tag_name\":\"1.2.0\",\"assets\":"
                              "[{\"name\":\"plugin.lua\",\"browser_download_url\":\"https://x/p.lua\"}]}]",
                              out_length);
    } else if (strstr(url, "/o.lua") != NULL) {
        *out_body = copy_body("daukle.plugin{ api = 1, uses = {} }\n"
                              "daukle.language{ name = 'remote-1-1-0', apply = function() return '' end }\n",
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

/* Both daukle.cache (the releases index, keyed by repo/range) and
   fr_plugin_fetch (the winning asset, keyed by its url) persist to disk by
   default, so an unisolated run here would leave entries in the developer's
   real cache root and, worse, keep serving them if the stub bodies above ever
   change. Isolated the way plugin_update_re_resolves_and_takes_the_new_url
   isolates its own persistent cache writes. */
TEST the_github_resolver_picks_the_highest_release_in_range(void) {
    fr_error err;
    const char *document_json =
        "{\"resolvers\":{\"gh\":{\"path\":\"./github-releases.lua\"}},"
        "\"plugins\":{\"r\":\"gh:daukle/remote@^1.0.0\"}}";

    char cache_dir[512];
    snprintf(cache_dir, sizeof cache_dir, "%s/daukle_test_github_resolver_%d",
            fr_test_temp_base(), fr_test_process_id());
    fr_test_set_env("DAUKLE_CACHE_DIR", cache_dir);

    release_requests = 0;
    fr_http_fn previous = fr_http_set_backend(stub_releases_index);
    fr_registry *registry = NULL;
    int built = fr_build_registry(&registry, &err) == FR_OK;
    cJSON *document = cJSON_Parse(document_json);

    int began = fr_lua_runtime_begin("test/fixtures/github-resolver", registry, &err) == FR_OK;
    int loaded = fr_plugins_load(registry, document, "test/fixtures/github-resolver", &err) == FR_OK;
    int highest_in_range = fr_registry_language(registry, "daukle.language/remote-1-2-0") != NULL;
    int lower_in_range = fr_registry_language(registry, "daukle.language/remote-1-1-0") != NULL;
    int out_of_range = fr_registry_language(registry, "daukle.language/remote-2-0-0") != NULL;
    int requests = release_requests;

    cJSON_Delete(document);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    fr_plugins_report_clear();
    fr_resolvers_clear();
    fr_http_set_backend(previous);
    fr_test_set_env("DAUKLE_CACHE_DIR", NULL);

    ASSERT(built);
    ASSERT(began);
    ASSERT(loaded);
    ASSERT(highest_in_range);
    ASSERT_FALSE(lower_in_range);
    ASSERT_FALSE(out_of_range);
    ASSERT_EQ(2, requests);
    PASS();
}

TEST an_undeclared_resolver_is_refused_naming_it(void) {
    fr_error err;
    int status = FR_OK;
    loads_from_url("{\"resolvers\":{\"known\":{\"path\":\"./r.lua\"}},"
                   "\"plugins\":{\"p\":\"missing:a/b\"}}", &status, &err);
    char message[512];
    snprintf(message, sizeof message, "%s", err.message);

    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "no resolver \"missing\" is declared") != NULL);
    ASSERT(strstr(message, "known") != NULL);
    PASS();
}

TEST a_matching_pin_loads_and_a_mismatched_one_fails_naming_both_digests(void) {
    fr_error err;
    char expected[65];
    fr_sha256_hex(ARTIFACT_BODY, strlen(ARTIFACT_BODY), expected);

    char good[320];
    snprintf(good, sizeof good,
             "{\"plugins\":{\"r\":{\"url\":\"https://x/pin-good.lua\",\"sha256\":\"%s\"}}}",
             expected);
    int good_status = FR_ERR;
    int registered = loads_from_url(good, &good_status, &err);

    const char *wrong = "0000000000000000000000000000000000000000000000000000000000000000";
    char bad[320];
    snprintf(bad, sizeof bad,
             "{\"plugins\":{\"r\":{\"url\":\"https://x/pin-bad.lua\",\"sha256\":\"%s\"}}}", wrong);
    int bad_status = FR_OK;
    loads_from_url(bad, &bad_status, &err);
    char message[512];
    snprintf(message, sizeof message, "%s", err.message);

    ASSERT_EQ(FR_OK, good_status);
    ASSERT(registered);
    ASSERT_EQ(FR_ERR, bad_status);
    ASSERT(strstr(message, expected) != NULL);
    ASSERT(strstr(message, wrong) != NULL);
    PASS();
}

/* PowerShell's Get-FileHash emits uppercase hex by default, and this project's
   primary toolchain is Windows, so an uppercase pin over the same bytes must
   still match the lowercase digest fr_sha256_hex computes. */
TEST an_uppercase_pin_still_matches_the_lowercase_digest(void) {
    fr_error err;
    char expected[65];
    fr_sha256_hex(ARTIFACT_BODY, strlen(ARTIFACT_BODY), expected);

    char uppercase[65];
    for (size_t index = 0; index < 64; index++) {
        uppercase[index] = (char) toupper((unsigned char) expected[index]);
    }
    uppercase[64] = '\0';

    char good[320];
    snprintf(good, sizeof good,
             "{\"plugins\":{\"r\":{\"url\":\"https://x/pin-upper.lua\",\"sha256\":\"%s\"}}}",
             uppercase);
    int status = FR_ERR;
    int registered = loads_from_url(good, &status, &err);

    ASSERT_EQm(err.message, FR_OK, status);
    ASSERT(registered);
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
    ASSERT_EQ(2, (int) report->count);
    ASSERT_STR_EQ("hello", report->entries[0].label);
    ASSERT_EQ(64, (int) strlen(report->entries[0].sha256));
    ASSERT_STR_EQ("path", report->entries[1].label);
    ASSERT_EQ(64, (int) strlen(report->entries[1].sha256));

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
    ASSERT_EQ(FR_PLUGIN_PATH, report->entries[0].kind);
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
    ASSERT_EQ(2, (int) report->count);
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

/* The report shows what the resolver answered, not the url it answered with:
   a coordinate is what the manifest wrote and what a reader recognises, while
   the url is wherever that resolver happens to keep its artifacts. */
TEST the_report_names_what_the_resolver_resolved(void) {
    fr_error err;
    fr_http_fn previous = fr_http_set_backend(stub_inline);
    fr_registry *registry = NULL;
    int built = fr_build_registry(&registry, &err) == FR_OK;
    cJSON *document = cJSON_Parse(
        "{\"resolvers\":{\"t\":{\"url\":\"https://x/resolver.lua\",\"sha256\":\"" INLINE_DIGEST
        "\"}},\"plugins\":{\"p\":\"t:a/b\"}}");
    int began = fr_lua_runtime_begin(".", registry, &err) == FR_OK;
    int status = fr_plugins_load(registry, document, ".", &err);

    const fr_plugin_report *report = fr_plugins_report();
    int count = (int) report->count;
    int kind = count == 1 ? (int) report->entries[0].kind : -1;
    char resolved[128] = "";
    if (count == 1 && report->entries[0].resolved != NULL) {
        snprintf(resolved, sizeof resolved, "%s", report->entries[0].resolved);
    }

    cJSON_Delete(document);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    fr_plugins_report_clear();
    fr_resolvers_clear();
    fr_http_set_backend(previous);

    ASSERT(built && began);
    ASSERT_EQm(err.message, FR_OK, status);
    ASSERT_EQ(1, count);
    ASSERT_EQ((int) FR_PLUGIN_RESOLVED, kind);
    ASSERT_STR_EQ("a/b", resolved);
    PASS();
}

static int stub_refuses_every_request(const char *url, const fr_http_header *headers,
                                      size_t header_count, char **out_body, size_t *out_length,
                                      fr_error *err) {
    (void) url; (void) headers; (void) header_count; (void) out_body; (void) out_length;
    fr_error_set(err, "the test backend made no request");
    return FR_ERR;
}

/* The cached artifact must be gone after a pin fails, or the next run is served
   the bytes that failed their integrity check. Asserting the load failed proves
   nothing about that: the discard is only visible in what the cache holds
   afterwards, which is why this test reaches for the cache directly. */
TEST a_failed_pin_discards_the_cached_artifact(void) {
    fr_error err;
    const char *url = "https://x/discarded.lua";
    char document_json[256];
    snprintf(document_json, sizeof document_json,
             "{\"plugins\":{\"p\":{\"url\":\"%s\",\"sha256\":\"%s\"}}}", url,
             "0000000000000000000000000000000000000000000000000000000000000000");

    fr_plugin_fetch_discard(url);
    int status = FR_OK;
    loads_from_url(document_json, &status, &err);

    fr_http_fn previous = fr_http_set_backend(stub_refuses_every_request);
    char *after = NULL;
    int served_from_cache = fr_plugin_fetch(url, &after, &err) == FR_OK;
    free(after);

    fr_http_set_backend(previous);
    fr_plugin_fetch_discard(url);

    ASSERT_EQ(FR_ERR, status);
    ASSERT_FALSE(served_from_cache);
    PASS();
}

/* Follows the shape of ARTIFACT_BODY above, but the language name encodes which
   url was fetched, so a test can tell "target-one" (the resolver's first,
   cached answer) apart from "target-two" (the answer only an update that
   disables the cache can reach). */
static int stub_named_artifacts(const char *url, const fr_http_header *headers,
                                size_t header_count, char **out_body, size_t *out_length,
                                fr_error *err) {
    (void) headers; (void) header_count; (void) err;
    const char *name = strstr(url, "/one.lua") != NULL ? "target-one" : "target-two";
    char body[256];
    snprintf(body, sizeof body,
            "daukle.plugin{ api = 1, uses = {} }\n"
            "daukle.language{ name = '%s', apply = function() return '' end }\n", name);
    *out_body = copy_body(body, out_length);
    return FR_OK;
}

/* Fetches url once and discards the text, so a cached artifact for it exists
   for a later discard to find: without this, "the artifact is gone" would be
   true whether or not the discard ever ran. */
static int seed_cached_artifact(const char *url) {
    fr_error err;
    char *text = NULL;
    int status = fr_plugin_fetch(url, &text, &err) == FR_OK;
    free(text);
    return status;
}

/* Swaps in a backend that refuses every request, fetches url, and restores
   restore: true only if url is still served, since a cache miss here has
   nowhere else to come from and fails outright. */
static int artifact_survived(const char *url, fr_http_fn restore) {
    fr_error err;
    fr_http_set_backend(stub_refuses_every_request);
    char *text = NULL;
    int served = fr_plugin_fetch(url, &text, &err) == FR_OK;
    free(text);
    fr_http_set_backend(restore);
    return served;
}

/* The resolver answers from an environment variable, so the test changes what a
   coordinate means without touching the manifest. That is what separates the
   two things being tested: an ordinary run must keep serving the old answer
   from the resolver's cache, and only the update may go past it.
   fr_plugins_update_cache runs the resolve step with reads bypassed but
   writes kept on, so the resolver's own coordinate-to-url mapping is
   overwritten, not just ignored for the one call: that is why a THIRD,
   ordinary load after the update is expected to see the new target, not just
   the update's own internal resolve. */
TEST plugin_update_re_resolves_and_takes_the_new_url(void) {
    fr_error err;
    const char *document_json =
        "{\"resolvers\":{\"t\":{\"path\":\"./test/fixtures/resolver/from-env.lua\"}},"
        "\"plugins\":{\"p\":\"t:a/b\"}}";
    const char *fresh_url = "https://example.invalid/two.lua";

    /* Isolated from the real cache root, the way test_cache.c and test_e2e.c
       isolate theirs, since this test writes a persistent on-disk mapping. */
    char cache_dir[512];
    snprintf(cache_dir, sizeof cache_dir, "%s/daukle_test_plugin_update_cache_%d",
            fr_test_temp_base(), fr_test_process_id());
    fr_test_set_env("DAUKLE_CACHE_DIR", cache_dir);

    fr_plugin_fetch_discard(fresh_url);
    fr_http_fn previous = fr_http_set_backend(stub_named_artifacts);
    fr_test_set_env("RESOLVER_TARGET", "one");

    fr_registry *first_registry = NULL;
    int built_first = fr_build_registry(&first_registry, &err) == FR_OK;
    cJSON *document = cJSON_Parse(document_json);
    int first_load = fr_lua_runtime_begin(".", first_registry, &err) == FR_OK
                  && fr_plugins_load(first_registry, document, ".", &err) == FR_OK;
    int got_one = fr_registry_language(first_registry, "daukle.language/target-one") != NULL;
    fr_registry_destroy(first_registry);
    fr_lua_runtime_shutdown();
    fr_plugins_report_clear();
    fr_resolvers_clear();

    fr_test_set_env("RESOLVER_TARGET", "two");

    fr_registry *second_registry = NULL;
    int built_second = fr_build_registry(&second_registry, &err) == FR_OK;
    int second_load = fr_lua_runtime_begin(".", second_registry, &err) == FR_OK
                   && fr_plugins_load(second_registry, document, ".", &err) == FR_OK;
    int still_one = fr_registry_language(second_registry, "daukle.language/target-one") != NULL;
    fr_registry_destroy(second_registry);
    fr_lua_runtime_shutdown();
    fr_plugins_report_clear();
    fr_resolvers_clear();

    int seeded = seed_cached_artifact(fresh_url);

    fr_plugin_entry *entries = NULL;
    size_t count = 0;
    fr_resolver_entry *resolvers = NULL;
    size_t resolver_count = 0;
    fr_resolvers_parse(document, &resolvers, &resolver_count, &err);
    fr_plugins_parse(document, resolvers, resolver_count, &entries, &count, &err);

    fr_registry *update_registry = NULL;
    int update_began = fr_build_registry(&update_registry, &err) == FR_OK
                    && fr_lua_runtime_begin(".", update_registry, &err) == FR_OK;
    size_t removed = 0;
    int updated = update_began
              && fr_plugins_update_cache(entries, count, resolvers, resolver_count, NULL,
                                        &removed, &err) == FR_OK;
    fr_lua_runtime_shutdown();
    fr_registry_destroy(update_registry);
    fr_plugins_free(entries, count);
    fr_resolvers_free(resolvers, resolver_count);
    fr_resolvers_clear();

    int served_stale = artifact_survived(fresh_url, stub_named_artifacts);

    fr_registry *third_registry = NULL;
    int built_third = fr_build_registry(&third_registry, &err) == FR_OK;
    int third_load = fr_lua_runtime_begin(".", third_registry, &err) == FR_OK
                  && fr_plugins_load(third_registry, document, ".", &err) == FR_OK;
    int got_two = fr_registry_language(third_registry, "daukle.language/target-two") != NULL;
    fr_registry_destroy(third_registry);
    fr_lua_runtime_shutdown();
    fr_plugins_report_clear();
    fr_resolvers_clear();

    fr_http_set_backend(previous);
    fr_plugin_fetch_discard(fresh_url);
    cJSON_Delete(document);
    fr_test_set_env("RESOLVER_TARGET", NULL);
    fr_test_set_env("DAUKLE_CACHE_DIR", NULL);

    ASSERT(built_first && built_second && built_third);
    ASSERT(first_load && second_load && third_load);
    ASSERT(seeded);
    ASSERT(update_began);
    ASSERT(got_one);
    ASSERT(still_one);
    ASSERT(updated);
    ASSERT_EQ(1u, removed);
    ASSERT_FALSE(served_stale);
    ASSERT(got_two);
    PASS();
}

/* This is the property "daukle plugin update" with no label must have: the plugin
   cache root is shared across every project on the machine, so discarding the
   artifacts of entries a manifest declares must never reach a url it does not,
   the way the earlier fr_plugins_remove_all_cache implementation did. "o" here
   stands in for a plugin some OTHER project cached, never named in the
   manifest entries this call is given. */
TEST fr_plugins_update_cache_with_no_label_touches_only_the_given_entries(void) {
    fr_error err;
    char declared_url[128];
    char other_url[128];
    snprintf(declared_url, sizeof declared_url, "https://x/%d-declared.lua", fr_test_process_id());
    snprintf(other_url, sizeof other_url, "https://x/%d-other.lua", fr_test_process_id());
    fr_plugin_fetch_discard(declared_url);
    fr_plugin_fetch_discard(other_url);

    char declared_only[256];
    char other_only[256];
    snprintf(declared_only, sizeof declared_only, "{\"plugins\":{\"d\":\"%s\"}}", declared_url);
    snprintf(other_only, sizeof other_only, "{\"plugins\":{\"o\":\"%s\"}}", other_url);

    /* Seeded with two separate loads, each its own registry: both fetch the
       same stub body, which registers one fixed language name, and loading
       them together in one call would collide on that name. */
    int declared_seeded = FR_ERR;
    int other_seeded = FR_ERR;
    loads_from_url(declared_only, &declared_seeded, &err);
    loads_from_url(other_only, &other_seeded, &err);

    cJSON *declared_document = cJSON_Parse(declared_only);
    fr_plugin_entry *entries = NULL;
    size_t count = 0;
    int parsed = fr_plugins_parse(declared_document, NULL, 0, &entries, &count, &err);
    size_t removed_count = 0;
    int updated = fr_plugins_update_cache(entries, count, NULL, 0, NULL, &removed_count, &err);
    fr_plugins_free(entries, count);
    cJSON_Delete(declared_document);

    /* "d" was in the entries fr_plugins_update_cache was given: its artifact
       must be gone, so loading it again reaches the network. */
    int declared_reloaded = FR_ERR;
    release_requests = 0;
    loads_from_url(declared_only, &declared_reloaded, &err);
    int declared_requests = release_requests;

    /* "o" was never in those entries: its artifact must survive, so loading it
       again makes zero requests. Both reloads are asserted to have SUCCEEDED,
       or a zero request count would also be what a reload that failed before
       asking for anything produces. */
    int other_reloaded = FR_ERR;
    release_requests = 0;
    loads_from_url(other_only, &other_reloaded, &err);
    int other_requests = release_requests;

    fr_plugin_fetch_discard(declared_url);
    fr_plugin_fetch_discard(other_url);

    ASSERT_EQm(err.message, FR_OK, declared_seeded);
    ASSERT_EQ(FR_OK, other_seeded);
    ASSERT_EQ(FR_OK, parsed);
    ASSERT_EQ(FR_OK, updated);
    ASSERT_EQ(1, (int) removed_count);
    ASSERT_EQ(FR_OK, declared_reloaded);
    ASSERT_EQ(FR_OK, other_reloaded);
    ASSERT(declared_requests > 0);
    ASSERT_EQ(0, other_requests);
    PASS();
}

/* The other half of the honesty fix: a label matching a REAL fetched entry must
   report removed_count 1, not just the local no-op case reporting 0. */
TEST fr_plugins_update_cache_with_a_label_matching_a_url_entry_removes_it(void) {
    fr_error err;
    char url[128];
    snprintf(url, sizeof url, "https://x/%d-labelled.lua", fr_test_process_id());
    fr_plugin_fetch_discard(url);

    char document_json[256];
    snprintf(document_json, sizeof document_json, "{\"plugins\":{\"r\":\"%s\"}}", url);

    int seeded = FR_ERR;
    loads_from_url(document_json, &seeded, &err);

    cJSON *document = cJSON_Parse(document_json);
    fr_plugin_entry *entries = NULL;
    size_t count = 0;
    int parsed = fr_plugins_parse(document, NULL, 0, &entries, &count, &err);
    size_t removed_count = 0;
    int updated = fr_plugins_update_cache(entries, count, NULL, 0, "r", &removed_count, &err);
    fr_plugins_free(entries, count);
    cJSON_Delete(document);

    int reloaded = FR_ERR;
    release_requests = 0;
    loads_from_url(document_json, &reloaded, &err);
    int requests_after = release_requests;
    fr_plugin_fetch_discard(url);

    ASSERT_EQm(err.message, FR_OK, seeded);
    ASSERT_EQ(FR_OK, parsed);
    ASSERT_EQ(FR_OK, updated);
    ASSERT_EQ(1, (int) removed_count);
    ASSERT_EQ(FR_OK, reloaded);
    ASSERT(requests_after > 0);
    PASS();
}

TEST fr_plugins_update_cache_with_an_unknown_label_errors_naming_it(void) {
    fr_error err;
    cJSON *document = cJSON_Parse("{\"plugins\":{\"npm\":\"https://example.invalid/npm.lua\"}}");
    fr_plugin_entry *entries = NULL;
    size_t count = 0;
    ASSERT_EQ(FR_OK, fr_plugins_parse(document, NULL, 0, &entries, &count, &err));

    size_t removed_count = 0;
    ASSERT_EQ(FR_ERR,
              fr_plugins_update_cache(entries, count, NULL, 0, "gradle", &removed_count, &err));
    ASSERT(strstr(err.message, "gradle") != NULL);
    ASSERT_EQ(0, (int) removed_count);

    fr_plugins_free(entries, count);
    cJSON_Delete(document);
    PASS();
}

/* A path match has nothing cached, so the removal is a no-op: *out_removed_count
   must come back 0, which is what lets main.c's plugin_update tell this case
   apart from a real removal and report honestly rather than claiming to have
   cleared a cache that never existed. The same entries, all paths, also pin the
   no-label case: a manifest with nothing but local plugins must report 0
   removed there too, not the unconditional "cleared" main.c used to print
   regardless of what fr_plugins_update_cache actually did. */
TEST fr_plugins_update_cache_with_a_label_matching_a_local_entry_is_not_an_error(void) {
    fr_error err;
    cJSON *document = cJSON_Parse("{\"plugins\":{\"hello\":\"./plugins/hello.lua\"}}");
    fr_plugin_entry *entries = NULL;
    size_t count = 0;
    ASSERT_EQ(FR_OK, fr_plugins_parse(document, NULL, 0, &entries, &count, &err));

    size_t removed_count = 1;
    ASSERT_EQ(FR_OK,
              fr_plugins_update_cache(entries, count, NULL, 0, "hello", &removed_count, &err));
    ASSERT_EQ(0, (int) removed_count);

    removed_count = 1;
    ASSERT_EQ(FR_OK,
              fr_plugins_update_cache(entries, count, NULL, 0, NULL, &removed_count, &err));
    ASSERT_EQ(0, (int) removed_count);

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
    RUN_TEST(parses_the_string_url_form);
    RUN_TEST(parses_the_table_form_with_a_pin);
    RUN_TEST(parses_the_table_form_naming_a_resolver_and_a_coordinate);
    RUN_TEST(parses_a_local_path);
    RUN_TEST(a_table_entry_naming_two_forms_is_refused);
    RUN_TEST(a_table_entry_naming_no_form_is_refused);
    RUN_TEST(a_table_entry_naming_a_resolver_without_a_coordinate_is_refused);
    RUN_TEST(an_absent_plugins_table_yields_no_entries);
    RUN_TEST(the_old_bare_coordinate_form_names_the_fix);
    RUN_TEST(the_declared_resolvers_are_named_when_a_value_names_none);
    RUN_TEST(an_overlong_resolver_list_is_cut_after_a_whole_label);
    RUN_TEST(rejects_a_plugins_member_that_is_not_a_table);
    RUN_TEST(rejects_a_string_with_an_empty_half_around_the_colon);
    RUN_TEST(a_windows_drive_letter_string_is_a_path);
    RUN_TEST(a_coordinate_containing_colons_reaches_the_resolver_whole);
    RUN_TEST(a_fetched_manifest_may_not_declare_plugins);
    RUN_TEST(a_fetched_manifest_without_plugins_is_accepted);
    RUN_TEST(fr_project_parse_refuses_a_fetched_manifest_declaring_plugins);
    RUN_TEST(a_local_plugin_registers_its_language);
    RUN_TEST(sync_writes_through_a_plugin_registered_language);
    RUN_TEST(a_verb_a_plugin_declared_is_there_when_it_runs);
    RUN_TEST(a_manifest_declaring_no_plugins_opens_no_lua_state);
    RUN_TEST(a_language_plugin_may_not_declare_exec);
    RUN_TEST(a_source_plugin_may_not_declare_exec);
    RUN_TEST(a_plugin_execing_before_it_declares_is_refused_at_the_call);
    RUN_TEST(a_language_plugin_declaring_tool_but_not_exec_still_loads);
    RUN_TEST(a_verb_uses_does_not_know_is_refused);
    RUN_TEST(a_plugin_written_against_a_later_api_says_which);
    RUN_TEST(a_uses_entry_that_is_not_a_string_is_refused);
    RUN_TEST(a_verb_used_before_the_declaration_says_so);
    RUN_TEST(fr_plugins_read_uses_rejects_a_closed_runtime);
    RUN_TEST(the_github_plugin_authenticates_from_either_token_variable);
    RUN_TEST(the_github_plugin_names_an_optional_field_that_is_not_a_string);
    RUN_TEST(a_url_entry_fetches_and_loads);
    RUN_TEST(a_resolved_entry_loads_what_its_resolver_names);
    RUN_TEST(the_github_resolver_picks_the_highest_release_in_range);
    RUN_TEST(an_undeclared_resolver_is_refused_naming_it);
    RUN_TEST(a_matching_pin_loads_and_a_mismatched_one_fails_naming_both_digests);
    RUN_TEST(an_uppercase_pin_still_matches_the_lowercase_digest);
    RUN_TEST(config_print_reports_each_plugin_with_its_verbs_and_digest);
    RUN_TEST(the_report_names_a_plugins_declared_verbs);
    RUN_TEST(the_report_survives_the_entries_it_describes_being_freed);
    RUN_TEST(fr_plugins_report_clear_empties_the_report);
    RUN_TEST(the_report_names_what_the_resolver_resolved);
    RUN_TEST(a_failed_pin_discards_the_cached_artifact);
    RUN_TEST(plugin_update_re_resolves_and_takes_the_new_url);
    RUN_TEST(fr_plugins_update_cache_with_no_label_touches_only_the_given_entries);
    RUN_TEST(fr_plugins_update_cache_with_a_label_matching_a_url_entry_removes_it);
    RUN_TEST(fr_plugins_update_cache_with_an_unknown_label_errors_naming_it);
    RUN_TEST(fr_plugins_update_cache_with_a_label_matching_a_local_entry_is_not_an_error);
    RUN_TEST(a_local_plugin_with_a_matching_pin_loads_and_a_wrong_one_fails_naming_both_digests);
    GREATEST_MAIN_END();
}
