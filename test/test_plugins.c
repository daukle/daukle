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

TEST a_table_entrys_requires_key_becomes_its_overrides(void) {
    cJSON *document = document_from(
        "{\"plugins\":{\"gradle\":{\"resolver\":\"github\",\"coordinate\":\"daukle/gradle@^2.0.0\","
        "\"requires\":{\"java\":\"./plugins/java\"}}}}");
    fr_plugin_entry *entries = NULL; size_t count = 0; fr_error err;

    ASSERT_EQ(FR_OK, fr_plugins_parse(document, NULL, 0, &entries, &count, &err));
    ASSERT_EQ(1, (int) count);
    ASSERT(entries[0].overrides != NULL);
    const cJSON *java = cJSON_GetObjectItemCaseSensitive(entries[0].overrides, "java");
    ASSERT(java != NULL);
    ASSERT(cJSON_IsString(java));
    ASSERT_STR_EQ("./plugins/java", java->valuestring);

    fr_plugins_free(entries, count);
    cJSON_Delete(document);
    PASS();
}

TEST a_table_entry_without_requires_has_no_overrides(void) {
    cJSON *document = document_from("{\"plugins\":{\"gradle\":{\"url\":\"https://example.invalid/g.lua\"}}}");
    fr_plugin_entry *entries = NULL; size_t count = 0; fr_error err;

    ASSERT_EQ(FR_OK, fr_plugins_parse(document, NULL, 0, &entries, &count, &err));
    ASSERT_EQ(1, (int) count);
    ASSERT(entries[0].overrides == NULL);

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
   reaching fr_plugins_read_declaration with no runtime open (resolvers.c's
   acquire, before fr_lua_plugin_load, is the one that matters) needs a real
   error back, not a crash. No runtime is open here between tests, so this
   needs no setup. */
TEST fr_plugins_read_declaration_rejects_a_closed_runtime(void) {
    fr_error err;
    fr_plugin_declaration declaration;
    memset(&declaration, 0, sizeof declaration);
    static const char chunk[] = "daukle.plugin{ api = 1 }";
    int status = fr_plugins_read_declaration(chunk, sizeof chunk - 1, "test.lua", "resolver", "t",
                                             &declaration, &err);

    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(err.message, "no lua runtime is open") != NULL);
    ASSERT_EQ(0u, declaration.uses_count);
    ASSERT_EQ(0u, declaration.requires_count);
    PASS();
}

/* Reads one chunk's declaration and reports the message rather than the status,
   because every refusal below is about which clause came back. */
static int read_declaration_of(const char *source, fr_plugin_declaration *out, char *message,
                               size_t size) {
    fr_error err;
    message[0] = '\0';
    memset(out, 0, sizeof *out);
    fr_registry *registry = NULL;
    int began = fr_build_registry(&registry, &err) == FR_OK
              && fr_lua_runtime_begin(".", registry, &err) == FR_OK;
    int status = began ? fr_plugins_read_declaration(source, strlen(source), "@test", "plugin",
                                                     "p", out, &err)
                       : FR_ERR;
    if (status != FR_OK) snprintf(message, size, "%s", err.message);
    fr_lua_runtime_shutdown();
    return status;
}

TEST a_declaration_records_its_requires(void) {
    static char message[512];
    fr_plugin_declaration declaration;
    int status = read_declaration_of(
        "daukle.plugin{ api = 1, requires = { java = { url = \"https://x/java.tar\","
        " sha256 = \"9f86d0\" } } }",
        &declaration, message, sizeof message);
    size_t count = declaration.requires_count;
    char alias[32] = "";
    char url[64] = "";
    if (count == 1) {
        snprintf(alias, sizeof alias, "%s", declaration.requires[0].alias);
        snprintf(url, sizeof url, "%s", declaration.requires[0].url);
    }
    fr_plugins_free_declaration(&declaration);

    ASSERT_EQm(message, FR_OK, status);
    ASSERT_EQ(1, (int) count);
    ASSERT_STR_EQ("java", alias);
    ASSERT_STR_EQ("https://x/java.tar", url);
    PASS();
}

TEST a_requires_entry_without_a_sha256_is_refused(void) {
    static char message[512];
    fr_plugin_declaration declaration;
    int status = read_declaration_of(
        "daukle.plugin{ api = 1, requires = { java = { url = \"https://x/java.tar\" } } }",
        &declaration, message, sizeof message);
    fr_plugins_free_declaration(&declaration);

    ASSERT_EQ(FR_ERR, status);
    ASSERTm(message, strstr(message, "is always pinned") != NULL);
    PASS();
}

TEST a_requires_entry_naming_a_path_is_refused(void) {
    static char message[512];
    fr_plugin_declaration declaration;
    int status = read_declaration_of(
        "daukle.plugin{ api = 1, requires = { java = { path = \"./java.lua\" } } }",
        &declaration, message, sizeof message);
    fr_plugins_free_declaration(&declaration);

    ASSERT_EQ(FR_ERR, status);
    ASSERTm(message, strstr(message, "acquired by url") != NULL);
    ASSERTm(message, strstr(message, "[plugins.p].requires") != NULL);
    PASS();
}

TEST an_alias_with_a_reserved_character_is_refused(void) {
    static char message[512];
    fr_plugin_declaration declaration;
    int status = read_declaration_of(
        "daukle.plugin{ api = 1, requires = { [\"ja.va\"] = { url = \"https://x/j.tar\","
        " sha256 = \"9f86d0\" } } }",
        &declaration, message, sizeof message);
    fr_plugins_free_declaration(&declaration);

    ASSERT_EQ(FR_ERR, status);
    ASSERTm(message, strstr(message, "letters, digits") != NULL);
    PASS();
}

TEST a_resolver_may_not_require_a_plugin(void) {
    static char message[512];
    fr_plugin_declaration declaration;
    fr_error err;
    memset(&declaration, 0, sizeof declaration);
    const char *source = "daukle.plugin{ api = 1, requires = { java = { url = \"https://x/j.tar\","
                         " sha256 = \"9f86d0\" } } }";
    fr_registry *registry = NULL;
    int began = fr_build_registry(&registry, &err) == FR_OK
              && fr_lua_runtime_begin(".", registry, &err) == FR_OK;
    int status = began ? fr_plugins_read_declaration(source, strlen(source), "@test", "resolver",
                                                     "gh", &declaration, &err)
                       : FR_ERR;
    if (status != FR_OK) snprintf(message, sizeof message, "%s", err.message);
    fr_lua_runtime_shutdown();
    fr_plugins_free_declaration(&declaration);

    ASSERT(began);
    ASSERT_EQ(FR_ERR, status);
    ASSERTm(message, strstr(message, "acquired by the floor only") != NULL);
    PASS();
}

/* setmetatable is a reachable base global here too, so a requires entry can
   carry a metatable whose __index would answer for the "url" this raw table
   never sets, raising unconditionally on any key so a fall-through to a
   metamethod-honouring read is detectable no matter which field is checked
   first. If raw_has_field or raw_string_field ever read through lua_getfield
   instead of lua_rawget, the metamethod would fire before "names no url"
   could and its marker text would land in err.message; with the raw read in
   place __index is never consulted, so the raw table's missing "url" is
   refused plainly and "boom" never appears. */
TEST a_hostile_index_metatable_on_a_requires_entry_is_never_consulted(void) {
    static char message[512];
    fr_plugin_declaration declaration;
    int status = read_declaration_of(
        "daukle.plugin{ api = 1, requires = { java = setmetatable("
        "{ sha256 = \"9f86d0\" }, { __index = function() error(\"boom\") end }) } }",
        &declaration, message, sizeof message);
    fr_plugins_free_declaration(&declaration);

    ASSERT_EQ(FR_ERR, status);
    ASSERTm(message, strstr(message, "names no url") != NULL);
    ASSERTm(message, strstr(message, "boom") == NULL);
    PASS();
}

TEST a_declaration_records_its_exports(void) {
    static char message[512];
    fr_plugin_declaration declaration;
    int status = read_declaration_of(
        "daukle.plugin{ api = 1, exports = { \"lib/coords\" } }",
        &declaration, message, sizeof message);
    size_t count = declaration.exports_count;
    char first[64] = "";
    if (count == 1) snprintf(first, sizeof first, "%s", declaration.exports[0]);
    fr_plugins_free_declaration(&declaration);

    ASSERT_EQm(message, FR_OK, status);
    ASSERT_EQ(1, (int) count);
    ASSERT_STR_EQ("lib/coords", first);
    PASS();
}

TEST an_export_written_as_a_file_name_is_refused(void) {
    static char message[512];
    fr_plugin_declaration declaration;
    int status = read_declaration_of(
        "daukle.plugin{ api = 1, exports = { \"lib/coords.lua\" } }",
        &declaration, message, sizeof message);
    fr_plugins_free_declaration(&declaration);

    ASSERT_EQ(FR_ERR, status);
    ASSERTm(message, strstr(message, "without the \".lua\"") != NULL);
    PASS();
}

TEST an_export_that_climbs_out_is_refused(void) {
    static char message[512];
    fr_plugin_declaration declaration;
    int status = read_declaration_of(
        "daukle.plugin{ api = 1, exports = { \"../secrets\" } }",
        &declaration, message, sizeof message);
    fr_plugins_free_declaration(&declaration);

    ASSERT_EQ(FR_ERR, status);
    ASSERTm(message, strstr(message, "may not leave the plugin") != NULL);
    PASS();
}

TEST a_resolver_may_not_export(void) {
    static char message[512];
    fr_plugin_declaration declaration;
    fr_error err;
    memset(&declaration, 0, sizeof declaration);
    const char *source = "daukle.plugin{ api = 1, exports = { \"lib/x\" } }";
    fr_registry *registry = NULL;
    int began = fr_build_registry(&registry, &err) == FR_OK
              && fr_lua_runtime_begin(".", registry, &err) == FR_OK;
    int status = began ? fr_plugins_read_declaration(source, strlen(source), "@test", "resolver",
                                                     "gh", &declaration, &err)
                       : FR_ERR;
    if (status != FR_OK) snprintf(message, sizeof message, "%s", err.message);
    fr_lua_runtime_shutdown();
    fr_plugins_free_declaration(&declaration);

    ASSERT(began);
    ASSERT_EQ(FR_ERR, status);
    ASSERTm(message, strstr(message, "has no dependents") != NULL);
    PASS();
}

/* exports is a sequence, walked with lua_rawgeti/lua_rawlen rather than
   lua_next, so the requires-entry hostile-metatable trick above (a hash key
   that is simply absent raw) does not exercise the same risk here: a
   metatable on a table that already holds every element raw proves nothing,
   because lua_rawgeti and a metamethod-honouring lua_geti would return the
   same value at every index. A table literal's array part is sized to its
   listed element count regardless of which of them are nil, and the border
   search behind lua_rawlen trusts a non-nil top slot without checking the
   slots below it, so { nil, "lib/coords" } reports length 2 while index 1
   is a genuine raw hole: absent under lua_rawgeti, answered by __index under
   lua_geti. That is the one place a raw read and a metamethod-honouring read
   of this exact table diverge. */
TEST a_hostile_index_metatable_on_exports_is_never_consulted(void) {
    static char message[512];
    fr_plugin_declaration declaration;
    int status = read_declaration_of(
        "daukle.plugin{ api = 1, exports = setmetatable({ nil, \"lib/coords\" },"
        " { __index = function() error(\"boom\") end }) }",
        &declaration, message, sizeof message);
    fr_plugins_free_declaration(&declaration);

    ASSERT_EQ(FR_ERR, status);
    ASSERTm(message, strstr(message, "must be a string") != NULL);
    ASSERTm(message, strstr(message, "boom") == NULL);
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

static char FETCH_LOG[1024];

static int stub_logs_every_url(const char *url, const fr_http_header *headers,
                               size_t header_count, char **out_body, size_t *out_length,
                               fr_error *err) {
    (void) headers; (void) header_count; (void) err;
    size_t used = strlen(FETCH_LOG);
    snprintf(FETCH_LOG + used, sizeof FETCH_LOG - used, "%s\n", url);
    *out_body = copy_body("daukle.plugin{ api = 1, uses = {} }\n", out_length);
    return FR_OK;
}

/* A resolver governs every unpinned acquisition, so it is the one thing a
   manifest must pin; an ordinary plugin entry may be an unpinned local path.
   "evil" here is exactly that: a path-form plugin whose chunk declares a
   resolver. It is loaded between two entries that name the resolver "t", so
   if its declaration were accepted the later entry would resolve through it
   while skipping acquisition entirely, and an unpinned chunk would decide
   where a pinned one's plugins come from. */
TEST a_plugin_chunk_may_not_declare_a_resolver(void) {
    fr_error err;
    const char *document_json =
        "{\"resolvers\":{\"t\":{\"path\":\"./test/fixtures/resolver/resolver.lua\"}},"
        "\"plugins\":{\"a\":\"t:x/y\",\"evil\":\"./test/fixtures/resolver/hijacks.lua\","
        "\"b\":\"t:z/w\"}}";

    int cache_was_enabled = fr_cache_enabled();
    fr_cache_set_enabled(0);
    FETCH_LOG[0] = '\0';
    fr_http_fn previous = fr_http_set_backend(stub_logs_every_url);
    fr_registry *registry = NULL;
    int built = fr_build_registry(&registry, &err) == FR_OK;
    cJSON *document = cJSON_Parse(document_json);
    int began = fr_lua_runtime_begin(".", registry, &err) == FR_OK;
    int status = fr_plugins_load(registry, document, ".", &err);
    char message[512];
    snprintf(message, sizeof message, "%s", err.message);
    char log[sizeof FETCH_LOG];
    snprintf(log, sizeof log, "%s", FETCH_LOG);

    cJSON_Delete(document);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    fr_plugins_report_clear();
    fr_resolvers_clear();
    fr_http_set_backend(previous);
    fr_cache_set_enabled(cache_was_enabled);

    ASSERT(built && began);
    ASSERT_EQ(FR_ERR, status);
    ASSERTm(message, strstr(message, "acquired as a resolver") != NULL);
    ASSERT(strstr(log, "hijacked") == NULL);
    PASS();
}

static char ARTIFACT_AUTHORIZATION[128];

static int stub_records_authorization(const char *url, const fr_http_header *headers,
                                      size_t header_count, char **out_body, size_t *out_length,
                                      fr_error *err) {
    (void) url; (void) err;
    ARTIFACT_AUTHORIZATION[0] = '\0';
    for (size_t index = 0; index < header_count; index++) {
        if (strcmp(headers[index].name, "Authorization") != 0) continue;
        snprintf(ARTIFACT_AUTHORIZATION, sizeof ARTIFACT_AUTHORIZATION, "%s",
                 headers[index].value);
    }
    *out_body = copy_body("daukle.plugin{ api = 1, uses = {} }\n", out_length);
    return FR_OK;
}

/* A private artifact needs the same credential its index did, and core names
   no authentication scheme of its own: what reaches the request is whatever
   opaque strings the resolver returned beside the url. */
TEST the_headers_a_resolver_returns_reach_the_artifact_fetch(void) {
    fr_error err;
    const char *document_json =
        "{\"resolvers\":{\"t\":{\"path\":\"./test/fixtures/resolver/with-headers.lua\"}},"
        "\"plugins\":{\"p\":\"t:a/b\"}}";

    int cache_was_enabled = fr_cache_enabled();
    fr_cache_set_enabled(0);
    ARTIFACT_AUTHORIZATION[0] = '\0';
    fr_http_fn previous = fr_http_set_backend(stub_records_authorization);
    fr_registry *registry = NULL;
    int built = fr_build_registry(&registry, &err) == FR_OK;
    cJSON *document = cJSON_Parse(document_json);
    int began = fr_lua_runtime_begin(".", registry, &err) == FR_OK;
    int status = fr_plugins_load(registry, document, ".", &err);
    char seen[128];
    snprintf(seen, sizeof seen, "%s", ARTIFACT_AUTHORIZATION);

    cJSON_Delete(document);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    fr_plugins_report_clear();
    fr_resolvers_clear();
    fr_http_set_backend(previous);
    fr_cache_set_enabled(cache_was_enabled);

    ASSERT(built && began);
    ASSERT_EQm(err.message, FR_OK, status);
    ASSERT_STR_EQ("Bearer secret", seen);
    PASS();
}

/* Four releases: o.lua (1.1.0, in range but not the highest), q.lua (2.0.0,
   out of range), p.lua (1.2.0, in range and the true highest), and p2.lua (a
   second "1.2.0" tag, in range and tied with p.lua). The true highest is
   listed neither first nor last among the in-range entries, so "keep the
   first in-range entry seen" and "keep the last in-range entry seen" cannot
   degenerate into the right answer by coincidence; only comparing every
   in-range candidate with greater() lands on p.lua. The tied duplicate is
   synthetic (a real repository cannot have two releases sharing one tag) and
   exists only so a comparison weakened from strict "greater than" to
   non-strict "greater than or equal" has something to expose: it would pull
   in p2.lua, the later of the tied pair, instead of leaving p.lua's earlier
   win alone. Each tag registers a differently named language so the test can
   tell exactly which asset was fetched. */
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
                              "[{\"name\":\"plugin.lua\",\"browser_download_url\":\"https://x/p.lua\"}]},"
                              "{\"tag_name\":\"1.2.0\",\"assets\":"
                              "[{\"name\":\"plugin.lua\",\"browser_download_url\":\"https://x/p2.lua\"}]}]",
                              out_length);
    } else if (strstr(url, "/o.lua") != NULL) {
        *out_body = copy_body("daukle.plugin{ api = 1, uses = {} }\n"
                              "daukle.language{ name = 'remote-1-1-0', apply = function() return '' end }\n",
                              out_length);
    } else if (strstr(url, "/p2.lua") != NULL) {
        *out_body = copy_body("daukle.plugin{ api = 1, uses = {} }\n"
                              "daukle.language{ name = 'remote-1-2-0-again', apply = function() return '' end }\n",
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
    int tied_duplicate = fr_registry_language(registry, "daukle.language/remote-1-2-0-again") != NULL;
    int requests = release_requests;

    /* Every other fixture resolves a coordinate to itself, so this is the one
       place that separates "resolved holds the resolver's answer" from
       "resolved echoes the coordinate back". */
    const fr_plugin_report *report = fr_plugins_report();
    char resolved[64] = "";
    if (report->count == 1) snprintf(resolved, sizeof resolved, "%s", report->entries[0].resolved);

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
    ASSERT_FALSE(tied_duplicate);
    ASSERT_EQ(2, requests);
    ASSERT_STR_EQ("1.2.0", resolved);
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
   holding borrowed pointers rather than its own copies. The resolved form is
   loaded as well as the local one because resolver and url are NULL for a
   path entry, so a path-only fixture leaves half the strings untested. */
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

    int cache_was_enabled = fr_cache_enabled();
    fr_cache_set_enabled(0);
    fr_http_fn previous = fr_http_set_backend(stub_inline);
    fr_registry *resolved_registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&resolved_registry, &err));
    cJSON *document = cJSON_Parse(
        "{\"resolvers\":{\"t\":{\"path\":\"./test/fixtures/resolver/resolver.lua\"}},"
        "\"plugins\":{\"p\":\"t:a/b\"}}");
    int began = fr_lua_runtime_begin(".", resolved_registry, &err) == FR_OK;
    int loaded = fr_plugins_load(resolved_registry, document, ".", &err) == FR_OK;

    cJSON_Delete(document);
    fr_registry_destroy(resolved_registry);
    fr_lua_runtime_shutdown();
    fr_resolvers_clear();
    fr_http_set_backend(previous);
    fr_cache_set_enabled(cache_was_enabled);

    report = fr_plugins_report();
    ASSERT(began && loaded);
    ASSERT_EQ(1, (int) report->count);
    ASSERT_STR_EQ("t", report->entries[0].resolver);
    ASSERT_STR_EQ("https://example.invalid/a/b.lua", report->entries[0].url);
    ASSERT_STR_EQ("a/b", report->entries[0].resolved);

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
    size_t after_length = 0;
    int served_from_cache = fr_plugin_fetch(url, NULL, 0, &after, &after_length, &err) == FR_OK;
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
    size_t length = 0;
    int status = fr_plugin_fetch(url, NULL, 0, &text, &length, &err) == FR_OK;
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
    size_t length = 0;
    int served = fr_plugin_fetch(url, NULL, 0, &text, &length, &err) == FR_OK;
    free(text);
    fr_http_set_backend(restore);
    return served;
}

/* The report is what "daukle config print" reads, and main.c has no test binary,
   so the report is where this property can be asserted at all. Printing is
   covered by eye; what must not silently change is what the report holds. */
TEST the_report_names_the_resolver_and_the_url(void) {
    fr_error err;
    const char *document_json =
        "{\"resolvers\":{\"t\":{\"path\":\"./test/fixtures/resolver/resolver.lua\"}},"
        "\"plugins\":{\"p\":\"t:a/b\"}}";

    fr_http_fn previous = fr_http_set_backend(stub_named_artifacts);
    fr_registry *registry = NULL;
    int built = fr_build_registry(&registry, &err) == FR_OK;
    cJSON *document = cJSON_Parse(document_json);
    int began = fr_lua_runtime_begin(".", registry, &err) == FR_OK;
    int loaded = fr_plugins_load(registry, document, ".", &err) == FR_OK;

    const fr_plugin_report *report = fr_plugins_report();
    char label[64] = "";
    char resolver[64] = "";
    char url[256] = "";
    char resolved[64] = "";
    size_t reported = report->count;
    if (reported == 1) {
        snprintf(label, sizeof label, "%s", report->entries[0].label);
        snprintf(resolver, sizeof resolver, "%s",
                 report->entries[0].resolver != NULL ? report->entries[0].resolver : "");
        snprintf(url, sizeof url, "%s",
                 report->entries[0].url != NULL ? report->entries[0].url : "");
        snprintf(resolved, sizeof resolved, "%s", report->entries[0].resolved);
    }

    cJSON_Delete(document);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    fr_plugins_report_clear();
    fr_resolvers_clear();
    fr_http_set_backend(previous);

    ASSERT(built && began && loaded);
    ASSERT_EQ(1u, reported);
    ASSERT_STR_EQ("p", label);
    ASSERT_STR_EQ("t", resolver);
    ASSERT_STR_EQ("https://example.invalid/a/b.lua", url);
    ASSERT_STR_EQ("a/b", resolved);
    PASS();
}

/* The spare resolver is the url form, so acquiring it needs the network, and
   the backend installed for the load refuses every request: the load succeeds
   only if the spare is neither fetched nor run. The used resolver is a local
   path and the artifact it names is seeded into the fetch cache first, so
   nothing the load legitimately does reaches the network either. A spare in
   the path form, or a backend that answers, would leave the same report
   whether or not the unreferenced resolver had been acquired. */
TEST the_report_names_a_declared_unused_resolver(void) {
    fr_error err;
    const char *artifact_url = "https://example.invalid/a/b.lua";
    const char *document_json =
        "{\"resolvers\":{\"used\":{\"path\":\"./test/fixtures/resolver/resolver.lua\"},"
        "\"spare\":{\"url\":\"https://example.invalid/spare-resolver.lua\",\"sha256\":"
        "\"0000000000000000000000000000000000000000000000000000000000000000\"}},"
        "\"plugins\":{\"p\":\"used:a/b\"}}";

    char cache_dir[512];
    snprintf(cache_dir, sizeof cache_dir, "%s/daukle_test_unused_resolver_%d",
            fr_test_temp_base(), fr_test_process_id());
    fr_test_set_env("DAUKLE_CACHE_DIR", cache_dir);

    fr_http_fn previous = fr_http_set_backend(stub_named_artifacts);
    int seeded = seed_cached_artifact(artifact_url);
    fr_http_set_backend(stub_refuses_every_request);

    fr_registry *registry = NULL;
    int built = fr_build_registry(&registry, &err) == FR_OK;
    cJSON *document = cJSON_Parse(document_json);
    int began = fr_lua_runtime_begin(".", registry, &err) == FR_OK;
    int loaded = fr_plugins_load(registry, document, ".", &err) == FR_OK;
    char message[512];
    snprintf(message, sizeof message, "%s", err.message);

    const fr_plugin_report *report = fr_plugins_report();
    char unused[64] = "";
    size_t unused_count = report->unused_resolver_count;
    if (unused_count == 1) snprintf(unused, sizeof unused, "%s", report->unused_resolvers[0]);

    cJSON_Delete(document);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    fr_plugins_report_clear();
    fr_resolvers_clear();
    fr_plugin_fetch_discard(artifact_url);
    fr_http_set_backend(previous);
    fr_test_set_env("DAUKLE_CACHE_DIR", NULL);

    ASSERT(built && began && seeded);
    ASSERTm(message, loaded);
    ASSERT_EQ(1u, unused_count);
    ASSERT_STR_EQ("spare", unused);
    PASS();
}

/* Every dependency url below serves this same body, so one runtime-computed digest covers all of
   them; only "gradle.lua" itself serves GRADLE_BODY_FOR_REQUIRES, which each test below fills in
   with its own requires table before installing this backend. */
static const char DEP_BODY_FOR_REQUIRES[] = "daukle.plugin{ api = 1, uses = {} }\n";
static char GRADLE_BODY_FOR_REQUIRES[512];

static int stub_dependency_graph(const char *url, const fr_http_header *headers,
                                 size_t header_count, char **out_body, size_t *out_length,
                                 fr_error *err) {
    (void) headers; (void) header_count; (void) err;
    const char *body =
        strstr(url, "/gradle.lua") != NULL ? GRADLE_BODY_FOR_REQUIRES : DEP_BODY_FOR_REQUIRES;
    *out_body = copy_body(body, out_length);
    return FR_OK;
}

/* A [plugins] entry that requires one dependency: the report must carry a second row for it,
   naming the dependent, the alias it was required under, and the digest computed while acquiring
   it (never recomputed for the report). The root row is unchanged: required_by stays NULL. */
TEST a_dependency_appears_in_the_report_with_its_digest(void) {
    fr_error err;
    char dep_digest[65];
    fr_sha256_hex(DEP_BODY_FOR_REQUIRES, strlen(DEP_BODY_FOR_REQUIRES), dep_digest);
    snprintf(GRADLE_BODY_FOR_REQUIRES, sizeof GRADLE_BODY_FOR_REQUIRES,
            "daukle.plugin{ api = 1, uses = {}, requires = { java = { url ="
            " \"https://x/java.lua\", sha256 = \"%s\" } } }\n", dep_digest);

    int cache_was_enabled = fr_cache_enabled();
    fr_cache_set_enabled(0);
    fr_http_fn previous = fr_http_set_backend(stub_dependency_graph);
    fr_registry *registry = NULL;
    int built = fr_build_registry(&registry, &err) == FR_OK;
    cJSON *document = cJSON_Parse("{\"plugins\":{\"gradle\":\"https://x/gradle.lua\"}}");
    int began = fr_lua_runtime_begin(".", registry, &err) == FR_OK;
    int status = fr_plugins_load(registry, document, ".", &err);
    char message[512];
    snprintf(message, sizeof message, "%s", err.message);

    const fr_plugin_report *report = fr_plugins_report();
    size_t count = report->count;
    int root_required_by_is_null = 0;
    char dep_label[32] = "", dep_required_by[32] = "", dep_alias[32] = "";
    size_t dep_sha_length = 0;
    int dep_overridden = -1;
    if (count == 2) {
        root_required_by_is_null = report->entries[0].required_by == NULL;
        snprintf(dep_label, sizeof dep_label, "%s", report->entries[1].label);
        snprintf(dep_required_by, sizeof dep_required_by, "%s", report->entries[1].required_by);
        snprintf(dep_alias, sizeof dep_alias, "%s", report->entries[1].alias);
        dep_sha_length = strlen(report->entries[1].sha256);
        dep_overridden = report->entries[1].overridden;
    }

    cJSON_Delete(document);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    fr_plugins_report_clear();
    fr_resolvers_clear();
    fr_http_set_backend(previous);
    fr_cache_set_enabled(cache_was_enabled);

    ASSERT(built && began);
    ASSERTm(message, FR_OK == status);
    ASSERT_EQ(2u, count);
    ASSERT(root_required_by_is_null);
    ASSERT_STR_EQ("java", dep_label);
    ASSERT_STR_EQ("gradle", dep_required_by);
    ASSERT_STR_EQ("java", dep_alias);
    ASSERT_EQ(64u, dep_sha_length);
    ASSERT_EQ(0, dep_overridden);
    PASS();
}

/* fr_plugin_deps_acquire walks its requires table with lua_next, whose order is unspecified: this
   table is declared "zulu, mike, alpha" so an unsorted report would very likely NOT come out
   alphabetically by accident. daukle config print must still emit alpha, mike, zulu every time,
   because it is what a person diffs between runs and between machines. */
TEST dependency_rows_are_sorted_by_required_by_then_alias(void) {
    fr_error err;
    char dep_digest[65];
    fr_sha256_hex(DEP_BODY_FOR_REQUIRES, strlen(DEP_BODY_FOR_REQUIRES), dep_digest);
    snprintf(GRADLE_BODY_FOR_REQUIRES, sizeof GRADLE_BODY_FOR_REQUIRES,
            "daukle.plugin{ api = 1, uses = {}, requires = {"
            " zulu = { url = \"https://x/zulu.lua\", sha256 = \"%s\" },"
            " mike = { url = \"https://x/mike.lua\", sha256 = \"%s\" },"
            " alpha = { url = \"https://x/alpha.lua\", sha256 = \"%s\" } } }\n",
            dep_digest, dep_digest, dep_digest);

    int cache_was_enabled = fr_cache_enabled();
    fr_cache_set_enabled(0);
    fr_http_fn previous = fr_http_set_backend(stub_dependency_graph);
    fr_registry *registry = NULL;
    int built = fr_build_registry(&registry, &err) == FR_OK;
    cJSON *document = cJSON_Parse("{\"plugins\":{\"gradle\":\"https://x/gradle.lua\"}}");
    int began = fr_lua_runtime_begin(".", registry, &err) == FR_OK;
    int status = fr_plugins_load(registry, document, ".", &err);
    char message[512];
    snprintf(message, sizeof message, "%s", err.message);

    const fr_plugin_report *report = fr_plugins_report();
    size_t count = report->count;
    char first[32] = "", second[32] = "", third[32] = "";
    if (count == 4) {
        snprintf(first, sizeof first, "%s", report->entries[1].alias);
        snprintf(second, sizeof second, "%s", report->entries[2].alias);
        snprintf(third, sizeof third, "%s", report->entries[3].alias);
    }

    cJSON_Delete(document);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    fr_plugins_report_clear();
    fr_resolvers_clear();
    fr_http_set_backend(previous);
    fr_cache_set_enabled(cache_was_enabled);

    ASSERT(built && began);
    ASSERTm(message, FR_OK == status);
    ASSERT_EQ(4u, count);
    ASSERT_STR_EQ("alpha", first);
    ASSERT_STR_EQ("mike", second);
    ASSERT_STR_EQ("zulu", third);
    PASS();
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

/* One resolve through a runtime of its own, so what comes back is the
   resolver's persisted answer rather than a memo an earlier call left behind. */
static int resolve_once(const fr_resolver_entry *resolver, const char *coordinate,
                        char *out_url, size_t out_size, fr_error *err) {
    fr_registry *registry = NULL;
    if (fr_build_registry(&registry, err) != FR_OK) return 0;

    int status = FR_ERR;
    char *url = NULL;
    char *resolved = NULL;
    fr_http_headers headers = { { { NULL, NULL } }, 0 };
    if (fr_lua_runtime_begin(".", registry, err) == FR_OK) {
        status = fr_resolvers_use(resolver, coordinate, &url, &resolved, &headers, err);
    }
    if (status == FR_OK) snprintf(out_url, out_size, "%s", url);

    fr_http_headers_free(&headers);
    free(url);
    free(resolved);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    fr_resolvers_clear();
    return status == FR_OK;
}

/* "daukle plugin update" IS a cache refresh, so --no-cache must not be allowed
   to turn off the write that persists the fresh mapping. With writes off the
   resolver re-resolves and the stale answer survives, and the next ordinary
   run fetches the old url whose artifact this command just discarded: a
   silent no-op on the one entry kind the command exists for. */
TEST plugin_update_persists_the_fresh_url_with_the_cache_disabled(void) {
    fr_error err;
    const char *document_json =
        "{\"resolvers\":{\"t\":{\"path\":\"./test/fixtures/resolver/from-env.lua\"}},"
        "\"plugins\":{\"p\":\"t:c/d\"}}";

    char cache_dir[512];
    snprintf(cache_dir, sizeof cache_dir, "%s/daukle_test_update_no_cache_%d",
            fr_test_temp_base(), fr_test_process_id());
    fr_test_set_env("DAUKLE_CACHE_DIR", cache_dir);
    fr_test_set_env("RESOLVER_TARGET", "one");

    cJSON *document = cJSON_Parse(document_json);
    fr_resolver_entry *resolvers = NULL;
    size_t resolver_count = 0;
    fr_plugin_entry *entries = NULL;
    size_t count = 0;
    fr_resolvers_parse(document, &resolvers, &resolver_count, &err);
    fr_plugins_parse(document, resolvers, resolver_count, &entries, &count, &err);

    char before[256] = "";
    int resolved_before = resolve_once(&resolvers[0], "c/d", before, sizeof before, &err);

    fr_test_set_env("RESOLVER_TARGET", "two");

    fr_registry *update_registry = NULL;
    int update_began = fr_build_registry(&update_registry, &err) == FR_OK
                    && fr_lua_runtime_begin(".", update_registry, &err) == FR_OK;
    int cache_was_enabled = fr_cache_enabled();
    fr_cache_set_enabled(0);
    size_t removed = 0;
    int updated = update_began
               && fr_plugins_update_cache(entries, count, resolvers, resolver_count, NULL,
                                         &removed, &err) == FR_OK;
    fr_cache_set_enabled(cache_was_enabled);
    fr_lua_runtime_shutdown();
    fr_registry_destroy(update_registry);
    fr_resolvers_clear();

    char after[256] = "";
    int resolved_after = resolve_once(&resolvers[0], "c/d", after, sizeof after, &err);

    fr_plugins_free(entries, count);
    fr_resolvers_free(resolvers, resolver_count);
    cJSON_Delete(document);
    fr_test_set_env("RESOLVER_TARGET", NULL);
    fr_test_set_env("DAUKLE_CACHE_DIR", NULL);

    ASSERT(update_began);
    ASSERTm(err.message, updated);
    ASSERT(resolved_before && resolved_after);
    ASSERT_STR_EQ("https://example.invalid/one.lua", before);
    ASSERT_STR_EQ("https://example.invalid/two.lua", after);
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

/* Serves bytes carrying a NUL, which is what every tar archive does. The body
   is built here rather than taken from a literal so the length is the thing
   under test rather than something strlen could re-derive. */
static int stub_body_with_a_nul(const char *url, const fr_http_header *headers,
                                size_t header_count, char **out_body, size_t *out_length,
                                fr_error *err) {
    (void) url; (void) headers; (void) header_count; (void) err;
    static const char body[] = "daukle.plugin{ api = 1 }\n\0 trailing";
    size_t length = sizeof body - 1;
    char *copy = malloc(length + 1);
    if (copy == NULL) return FR_ERR;
    memcpy(copy, body, length + 1);
    *out_body = copy;
    *out_length = length;
    return FR_OK;
}

TEST fetched_bytes_carry_their_length_past_an_embedded_nul(void) {
    static const char body[] = "daukle.plugin{ api = 1 }\n\0 trailing";
    size_t body_length = sizeof body - 1;

    int cache_was_enabled = fr_cache_enabled();
    fr_cache_set_enabled(0);
    fr_http_fn previous = fr_http_set_backend(stub_body_with_a_nul);

    char *text = NULL;
    size_t length = 0;
    fr_error err;
    int status = fr_plugin_fetch("https://x/artifact", NULL, 0, &text, &length, &err);

    char expected[65];
    fr_sha256_hex(body, body_length, expected);
    char actual[65];
    actual[0] = '\0';
    if (status == FR_OK) fr_sha256_hex(text, length, actual);

    free(text);
    fr_http_set_backend(previous);
    fr_cache_set_enabled(cache_was_enabled);

    ASSERT_EQm(err.message, FR_OK, status);
    ASSERT_EQ(body_length, length);
    ASSERT_STR_EQ(expected, actual);
    PASS();
}

/* greatest's ASSERT_*m keeps the message POINTER and prints it once the test has
   returned, so the buffer it names cannot live on the test's stack. */
static char directory_message[512];

static int loads_directory_plugin(const char *json) {
    fr_error err;
    err.message[0] = '\0';
    directory_message[0] = '\0';

    fr_registry *registry = NULL;
    if (fr_build_registry(&registry, &err) != FR_OK) return FR_ERR;

    cJSON *document = document_from(json);
    int status = FR_ERR;
    if (fr_lua_runtime_begin("test/fixtures/plugin-directory", registry, &err) == FR_OK) {
        status = fr_plugins_load(registry, document, "test/fixtures/plugin-directory", &err);
    }
    snprintf(directory_message, sizeof directory_message, "%s", err.message);

    cJSON_Delete(document);
    fr_registry_destroy(registry);
    return status;
}

TEST a_directory_path_loads_the_plugin_lua_inside_it(void) {
    int status = loads_directory_plugin("{\"plugins\":{\"hello\":{\"path\":\"./plugin\"}}}");
    const fr_plugin_report *report = fr_plugins_report();
    size_t count = report->count;
    char digest[65];
    snprintf(digest, sizeof digest, "%s", count == 1 ? report->entries[0].sha256 : "missing");

    fr_plugins_report_clear();
    fr_lua_runtime_shutdown();

    ASSERT_EQm(directory_message, FR_OK, status);
    ASSERT_EQ(1u, count);
    /* A directory has no byte string to digest, so the report carries none
       rather than a number nothing could reproduce. */
    ASSERT_STR_EQ("", digest);
    PASS();
}

TEST a_directory_path_may_not_carry_a_sha256(void) {
    int status = loads_directory_plugin(
        "{\"plugins\":{\"hello\":{\"path\":\"./plugin\",\"sha256\":"
        "\"0000000000000000000000000000000000000000000000000000000000000000\"}}}");
    char message[512];
    snprintf(message, sizeof message, "%s", directory_message);

    fr_plugins_report_clear();
    fr_lua_runtime_shutdown();

    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "is a directory and cannot carry a sha256") != NULL);
    PASS();
}

/* The refusal above is about directories, not about paths: a single-file path
   keeps its pin exactly as it had it. */
TEST a_single_file_path_with_a_sha256_still_loads(void) {
    fr_error err;
    err.message[0] = '\0';
    char *text = NULL;
    ASSERT_EQ(FR_OK, fr_file_read_text("test/fixtures/plugin-local/plugins/hello.lua", &text,
                                       &err));
    char digest[65];
    fr_sha256_hex(text, strlen(text), digest);
    free(text);

    char json[512];
    snprintf(json, sizeof json,
             "{\"plugins\":{\"hello\":{\"path\":\"./plugins/hello.lua\",\"sha256\":\"%s\"}}}",
             digest);

    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));
    cJSON *document = document_from(json);
    int status = FR_ERR;
    if (fr_lua_runtime_begin("test/fixtures/plugin-local", registry, &err) == FR_OK) {
        status = fr_plugins_load(registry, document, "test/fixtures/plugin-local", &err);
    }
    char message[512];
    snprintf(message, sizeof message, "%s", err.message);

    cJSON_Delete(document);
    fr_registry_destroy(registry);
    fr_plugins_report_clear();
    fr_lua_runtime_shutdown();

    ASSERT_EQm(message, FR_OK, status);
    PASS();
}

static char served_archive[8192];
static size_t served_archive_length;
static int archive_requests;

static int stub_serves_the_archive(const char *url, const fr_http_header *headers,
                                   size_t header_count, char **out_body, size_t *out_length,
                                   fr_error *err) {
    (void) url; (void) headers; (void) header_count; (void) err;
    archive_requests++;
    char *copy = malloc(served_archive_length + 1);
    if (copy == NULL) return FR_ERR;
    memcpy(copy, served_archive, served_archive_length);
    copy[served_archive_length] = '\0';
    *out_body = copy;
    *out_length = served_archive_length;
    return FR_OK;
}

static const char ARCHIVE_ENTRY[] =
    "daukle.plugin{ api = 1, uses = {} }\n"
    "local greeting = daukle.require(\"lib/greeting\")\n"
    "daukle.language{ name = greeting.name,\n"
    "                 apply = function(consumer, resolved, text) return text end }\n";
static const char ARCHIVE_MODULE[] = "return { name = \"from-archive\" }\n";

static void build_served_archive(int with_entry) {
    memset(served_archive, 0, sizeof served_archive);
    size_t offset = 0;
    if (with_entry) {
        offset = fr_test_tar_append(served_archive, offset, "plugin.lua", '0', ARCHIVE_ENTRY,
                                    sizeof ARCHIVE_ENTRY - 1);
    }
    offset = fr_test_tar_append(served_archive, offset, "lib/greeting.lua", '0', ARCHIVE_MODULE,
                                sizeof ARCHIVE_MODULE - 1);
    served_archive_length = fr_test_tar_end(served_archive, offset);
}

static char archive_message[512];

/* Loads one url-form plugin against the archive the stub serves, inside its own
   cache directory: the fetch cache is keyed by url and survives between runs, so
   a test meaning to reach the network can otherwise be answered from a previous
   run's cache and pass for the wrong reason. */
static int loads_served_archive(const char *pin, int *out_registered) {
    fr_error err;
    err.message[0] = '\0';
    archive_message[0] = '\0';
    *out_registered = 0;

    char json[512];
    if (pin != NULL) {
        snprintf(json, sizeof json,
                 "{\"plugins\":{\"a\":{\"url\":\"https://x/plugin.tar\",\"sha256\":\"%s\"}}}", pin);
    } else {
        snprintf(json, sizeof json, "{\"plugins\":{\"a\":\"https://x/plugin.tar\"}}");
    }

    fr_registry *registry = NULL;
    if (fr_build_registry(&registry, &err) != FR_OK) return FR_ERR;
    cJSON *document = document_from(json);

    int status = FR_ERR;
    if (fr_lua_runtime_begin(".", registry, &err) == FR_OK) {
        status = fr_plugins_load(registry, document, ".", &err);
        *out_registered = fr_registry_language(registry, "daukle.language/from-archive") != NULL;
    }
    snprintf(archive_message, sizeof archive_message, "%s", err.message);

    cJSON_Delete(document);
    fr_registry_destroy(registry);
    fr_plugins_report_clear();
    fr_lua_runtime_shutdown();
    return status;
}

TEST an_archive_plugin_loads_from_a_url_under_its_pin(void) {
    build_served_archive(1);

    char digest[65];
    /* Computed from the bytes actually served rather than pasted: a digest
       written into a test is pinned against that checkout's line endings, which
       is how the suite went red on a merge once already. */
    fr_sha256_hex(served_archive, served_archive_length, digest);

    char cache_dir[512];
    snprintf(cache_dir, sizeof cache_dir, "%s/daukle_test_archive_pin_%d", fr_test_temp_base(),
             fr_test_process_id());
    fr_test_remove_tree(cache_dir);
    fr_test_set_env("DAUKLE_CACHE_DIR", cache_dir);

    fr_http_fn previous = fr_http_set_backend(stub_serves_the_archive);
    int registered = 0;
    int status = loads_served_archive(digest, &registered);
    fr_http_set_backend(previous);
    fr_test_set_env("DAUKLE_CACHE_DIR", NULL);
    fr_test_remove_tree(cache_dir);

    ASSERT_EQm(archive_message, FR_OK, status);
    ASSERT(registered);
    PASS();
}

/* The second load reaches a backend that refuses everything, so it can only
   come from the cache. This is also the only test that would catch the cache
   entry's rename being half done. */
TEST an_archive_plugin_loads_again_from_the_cache(void) {
    build_served_archive(1);

    char cache_dir[512];
    snprintf(cache_dir, sizeof cache_dir, "%s/daukle_test_archive_cache_%d", fr_test_temp_base(),
             fr_test_process_id());
    fr_test_remove_tree(cache_dir);
    fr_test_set_env("DAUKLE_CACHE_DIR", cache_dir);

    archive_requests = 0;
    fr_http_fn previous = fr_http_set_backend(stub_serves_the_archive);
    int first_registered = 0;
    int first = loads_served_archive(NULL, &first_registered);
    int requests_after_first = archive_requests;

    fr_http_set_backend(stub_refuses_every_request);
    int second_registered = 0;
    int second = loads_served_archive(NULL, &second_registered);

    fr_http_set_backend(previous);
    fr_test_set_env("DAUKLE_CACHE_DIR", NULL);
    fr_test_remove_tree(cache_dir);

    ASSERT_EQm(archive_message, FR_OK, first);
    ASSERT(first_registered);
    ASSERT_EQ(1, requests_after_first);
    ASSERT_EQm(archive_message, FR_OK, second);
    ASSERT(second_registered);
    PASS();
}

TEST an_archive_without_a_plugin_lua_is_refused(void) {
    build_served_archive(0);

    char cache_dir[512];
    snprintf(cache_dir, sizeof cache_dir, "%s/daukle_test_archive_noentry_%d",
             fr_test_temp_base(), fr_test_process_id());
    fr_test_remove_tree(cache_dir);
    fr_test_set_env("DAUKLE_CACHE_DIR", cache_dir);

    fr_http_fn previous = fr_http_set_backend(stub_serves_the_archive);
    int registered = 0;
    int status = loads_served_archive(NULL, &registered);
    char message[512];
    snprintf(message, sizeof message, "%s", archive_message);
    fr_http_set_backend(previous);
    fr_test_set_env("DAUKLE_CACHE_DIR", NULL);
    fr_test_remove_tree(cache_dir);

    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "no plugin.lua") != NULL);
    PASS();
}

/* A pin covers the archive as a whole, so a mismatch is reported against the
   artifact and the bytes are not kept for the next run to trust. */
TEST an_archive_whose_pin_does_not_match_is_refused_and_discarded(void) {
    build_served_archive(1);

    char cache_dir[512];
    snprintf(cache_dir, sizeof cache_dir, "%s/daukle_test_archive_badpin_%d",
             fr_test_temp_base(), fr_test_process_id());
    fr_test_remove_tree(cache_dir);
    fr_test_set_env("DAUKLE_CACHE_DIR", cache_dir);

    archive_requests = 0;
    fr_http_fn previous = fr_http_set_backend(stub_serves_the_archive);
    int registered = 0;
    int status = loads_served_archive(
        "0000000000000000000000000000000000000000000000000000000000000000", &registered);
    char message[512];
    snprintf(message, sizeof message, "%s", archive_message);

    /* A second load with a correct pin must fetch again rather than be served
       the bytes the first one rejected. */
    char digest[65];
    fr_sha256_hex(served_archive, served_archive_length, digest);
    int second_registered = 0;
    int second = loads_served_archive(digest, &second_registered);
    int requests = archive_requests;

    fr_http_set_backend(previous);
    fr_test_set_env("DAUKLE_CACHE_DIR", NULL);
    fr_test_remove_tree(cache_dir);

    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "expected sha256") != NULL);
    ASSERT_EQ(FR_OK, second);
    ASSERT(second_registered);
    ASSERT_EQ(2, requests);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(parses_the_string_url_form);
    RUN_TEST(parses_the_table_form_with_a_pin);
    RUN_TEST(parses_the_table_form_naming_a_resolver_and_a_coordinate);
    RUN_TEST(a_table_entrys_requires_key_becomes_its_overrides);
    RUN_TEST(a_table_entry_without_requires_has_no_overrides);
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
    RUN_TEST(fr_plugins_read_declaration_rejects_a_closed_runtime);
    RUN_TEST(a_declaration_records_its_requires);
    RUN_TEST(a_requires_entry_without_a_sha256_is_refused);
    RUN_TEST(a_requires_entry_naming_a_path_is_refused);
    RUN_TEST(an_alias_with_a_reserved_character_is_refused);
    RUN_TEST(a_resolver_may_not_require_a_plugin);
    RUN_TEST(a_hostile_index_metatable_on_a_requires_entry_is_never_consulted);
    RUN_TEST(a_declaration_records_its_exports);
    RUN_TEST(an_export_written_as_a_file_name_is_refused);
    RUN_TEST(an_export_that_climbs_out_is_refused);
    RUN_TEST(a_resolver_may_not_export);
    RUN_TEST(a_hostile_index_metatable_on_exports_is_never_consulted);
    RUN_TEST(the_github_plugin_authenticates_from_either_token_variable);
    RUN_TEST(the_github_plugin_names_an_optional_field_that_is_not_a_string);
    RUN_TEST(a_url_entry_fetches_and_loads);
    RUN_TEST(fetched_bytes_carry_their_length_past_an_embedded_nul);
    RUN_TEST(a_directory_path_loads_the_plugin_lua_inside_it);
    RUN_TEST(a_directory_path_may_not_carry_a_sha256);
    RUN_TEST(a_single_file_path_with_a_sha256_still_loads);
    RUN_TEST(an_archive_plugin_loads_from_a_url_under_its_pin);
    RUN_TEST(an_archive_plugin_loads_again_from_the_cache);
    RUN_TEST(an_archive_without_a_plugin_lua_is_refused);
    RUN_TEST(an_archive_whose_pin_does_not_match_is_refused_and_discarded);
    RUN_TEST(a_resolved_entry_loads_what_its_resolver_names);
    RUN_TEST(a_plugin_chunk_may_not_declare_a_resolver);
    RUN_TEST(the_headers_a_resolver_returns_reach_the_artifact_fetch);
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
    RUN_TEST(the_report_names_the_resolver_and_the_url);
    RUN_TEST(the_report_names_a_declared_unused_resolver);
    RUN_TEST(a_dependency_appears_in_the_report_with_its_digest);
    RUN_TEST(dependency_rows_are_sorted_by_required_by_then_alias);
    RUN_TEST(plugin_update_re_resolves_and_takes_the_new_url);
    RUN_TEST(plugin_update_persists_the_fresh_url_with_the_cache_disabled);
    RUN_TEST(fr_plugins_update_cache_with_no_label_touches_only_the_given_entries);
    RUN_TEST(fr_plugins_update_cache_with_a_label_matching_a_url_entry_removes_it);
    RUN_TEST(fr_plugins_update_cache_with_an_unknown_label_errors_naming_it);
    RUN_TEST(fr_plugins_update_cache_with_a_label_matching_a_local_entry_is_not_an_error);
    RUN_TEST(a_local_plugin_with_a_matching_pin_loads_and_a_wrong_one_fails_naming_both_digests);
    GREATEST_MAIN_END();
}
