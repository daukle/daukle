#include "resolvers.h"

#include "cJSON.h"
#include "cache.h"
#include "config_lua.h"
#include "error.h"
#include "greatest.h"
#include "http.h"
#include "region.h"
#include "registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

GREATEST_MAIN_DEFS();

/* The sha256 of test/fixtures/resolver/resolver.lua, the same text
   stub_resolver_chunk serves below. Computed with fr_sha256_hex in a
   throwaway main against that file, not guessed. */
#define RESOLVER_DIGEST "b10d73551edd6c5537151d766d571733d2ec9375bbe41fbed2f87975dd6d6620"

static int parse(const char *json, fr_resolver_entry **out, size_t *out_count, fr_error *err) {
    cJSON *document = cJSON_Parse(json);
    int status = fr_resolvers_parse(document, out, out_count, err);
    cJSON_Delete(document);
    return status;
}

static int stub_resolver_chunk(const char *url, const fr_http_header *headers,
                               size_t header_count, char **out_body, size_t *out_length,
                               fr_error *err) {
    (void) url; (void) headers; (void) header_count;
    char *text = NULL;
    if (fr_file_read_text("./test/fixtures/resolver/resolver.lua", &text, err) != FR_OK) {
        return FR_ERR;
    }
    *out_body = text;
    *out_length = strlen(text);
    return FR_OK;
}

/* Repeated from test_plugin_fetch.c rather than shared: the two test binaries
   link separately and a shared stub would need a third translation unit for
   one function. */
static int stub_refuses(const char *url, const fr_http_header *headers, size_t header_count,
                        char **out_body, size_t *out_length, fr_error *err) {
    (void) url; (void) headers; (void) header_count; (void) out_body; (void) out_length;
    fr_error_set(err, "the network was reached");
    return FR_ERR;
}

TEST an_absent_table_yields_no_resolvers(void) {
    fr_error err;
    fr_resolver_entry *entries = NULL;
    size_t count = 99;
    int status = parse("{\"plugins\":{}}", &entries, &count, &err);
    fr_resolvers_free(entries, count);
    ASSERT_EQ(FR_OK, status);
    ASSERT_EQ(0u, count);
    PASS();
}

TEST a_url_entry_with_a_pin_parses(void) {
    fr_error err;
    fr_resolver_entry *entries = NULL;
    size_t count = 0;
    int status = parse("{\"resolvers\":{\"github\":{\"url\":\"https://h/r.lua\",\"sha256\":\"ab\"}}}",
                       &entries, &count, &err);
    int matched = status == FR_OK && count == 1
               && strcmp(entries[0].label, "github") == 0
               && strcmp(entries[0].url, "https://h/r.lua") == 0
               && strcmp(entries[0].sha256, "ab") == 0
               && entries[0].path == NULL;
    fr_resolvers_free(entries, count);
    ASSERT(matched);
    PASS();
}

TEST a_url_entry_without_a_pin_is_refused(void) {
    fr_error err;
    fr_resolver_entry *entries = NULL;
    size_t count = 0;
    int status = parse("{\"resolvers\":{\"github\":{\"url\":\"https://h/r.lua\"}}}",
                       &entries, &count, &err);
    fr_resolvers_free(entries, count);
    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(err.message, "must carry a sha256") != NULL);
    ASSERT(strstr(err.message, "chooses where every other plugin comes from") != NULL);
    PASS();
}

TEST a_local_resolver_needs_no_pin(void) {
    fr_error err;
    fr_resolver_entry *entries = NULL;
    size_t count = 0;
    int status = parse("{\"resolvers\":{\"dev\":{\"path\":\"./r.lua\"}}}", &entries, &count, &err);
    int matched = status == FR_OK && count == 1 && entries[0].url == NULL
               && strcmp(entries[0].path, "./r.lua") == 0;
    fr_resolvers_free(entries, count);
    ASSERT(matched);
    PASS();
}

TEST an_entry_naming_both_forms_is_refused(void) {
    fr_error err;
    fr_resolver_entry *entries = NULL;
    size_t count = 0;
    int status = parse("{\"resolvers\":{\"x\":{\"url\":\"https://h/r.lua\",\"path\":\"./r.lua\"}}}",
                       &entries, &count, &err);
    fr_resolvers_free(entries, count);
    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(err.message, "names both") != NULL);
    PASS();
}

TEST an_entry_naming_neither_form_is_refused(void) {
    fr_error err;
    fr_resolver_entry *entries = NULL;
    size_t count = 0;
    int status = parse("{\"resolvers\":{\"x\":{\"base\":\"https://h\"}}}", &entries, &count, &err);
    fr_resolvers_free(entries, count);
    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(err.message, "names neither") != NULL);
    PASS();
}

/* block borrows from document (see resolvers.h), so unlike every other
   assertion in this file, document must stay alive until after block is
   read: the shared parse() helper deletes it too early for this one case. */
TEST an_unknown_key_is_kept_for_the_resolver(void) {
    fr_error err;
    fr_resolver_entry *entries = NULL;
    size_t count = 0;
    cJSON *document = cJSON_Parse("{\"resolvers\":{\"x\":{\"path\":\"./r.lua\",\"base\":\"https://h\"}}}");
    int status = fr_resolvers_parse(document, &entries, &count, &err);
    const cJSON *base = NULL;
    if (status == FR_OK && count == 1) {
        base = cJSON_GetObjectItemCaseSensitive(entries[0].block, "base");
    }
    int matched = base != NULL && cJSON_IsString(base)
               && strcmp(base->valuestring, "https://h") == 0;
    fr_resolvers_free(entries, count);
    cJSON_Delete(document);
    ASSERT(matched);
    PASS();
}

TEST a_resolver_cannot_name_another_resolver(void) {
    fr_error err;
    fr_resolver_entry *entries = NULL;
    size_t count = 0;
    int status = parse("{\"resolvers\":{\"x\":{\"resolver\":\"y\",\"coordinate\":\"a/b\"}}}",
                       &entries, &count, &err);
    fr_resolvers_free(entries, count);
    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(err.message, "names neither") != NULL);
    PASS();
}

TEST find_returns_the_entry_for_a_label(void) {
    fr_error err;
    fr_resolver_entry *entries = NULL;
    size_t count = 0;
    parse("{\"resolvers\":{\"a\":{\"path\":\"./a.lua\"},\"b\":{\"path\":\"./b.lua\"}}}",
          &entries, &count, &err);
    const fr_resolver_entry *found = fr_resolvers_find(entries, count, "b");
    const fr_resolver_entry *missing = fr_resolvers_find(entries, count, "c");
    int matched = found != NULL && strcmp(found->path, "./b.lua") == 0 && missing == NULL;
    fr_resolvers_free(entries, count);
    ASSERT(matched);
    PASS();
}

/* Parses inline rather than through the parse() helper, for the same reason
   use_resolver below does: fr_resolvers_use reads entry->block through to
   fr_lua_resolver_call, and block borrows from document, so document must
   outlive that call. */
TEST a_resolver_is_acquired_through_the_floor_and_invoked(void) {
    fr_error err;
    cJSON *document = cJSON_Parse(
        "{\"resolvers\":{\"t\":{\"path\":\"./test/fixtures/resolver/resolver.lua\"}}}");
    fr_resolver_entry *entries = NULL;
    size_t count = 0;
    fr_resolvers_parse(document, &entries, &count, &err);

    fr_registry *registry = fr_registry_create();
    int began = fr_lua_runtime_begin(".", registry, &err) == FR_OK;
    char *url = NULL;
    char *resolved = NULL;
    fr_http_headers headers = { { { NULL, NULL } }, 0 };
    int status = fr_resolvers_use(&entries[0], "daukle/npm@^1.0.0", &url, &resolved,
                                  &headers, &err);
    fr_http_headers_free(&headers);
    int matched = status == FR_OK && url != NULL
               && strcmp(url, "https://example.invalid/daukle/npm@^1.0.0.lua") == 0;

    free(url);
    free(resolved);
    fr_resolvers_free(entries, count);
    cJSON_Delete(document);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    fr_resolvers_clear();
    ASSERT(began);
    ASSERT(matched);
    PASS();
}

/* Every one of these follows the same shape, and the shape is the point: parse,
   begin a runtime, act, capture into locals, release everything, then assert.
   Asserting before the shutdown would leave the shared runtime open and turn
   one failure into a cascade through the rest of the file. */
/* The parse() helper above deletes the document, and fr_resolver_entry.block
   borrows from it, so this returns the document for the caller to delete once
   the resolver has run: handing a freed block to Lua is a use-after-free. */
static cJSON *parse_keeping_the_document(const char *document_json, fr_resolver_entry **out,
                                         size_t *out_count, fr_error *err) {
    cJSON *document = cJSON_Parse(document_json);
    if (fr_resolvers_parse(document, out, out_count, err) != FR_OK || *out_count != 1) {
        fr_resolvers_free(*out, *out_count);
        cJSON_Delete(document);
        return NULL;
    }
    return document;
}

static int use_resolver(const char *document_json, const char *coordinate,
                        char **out_url, char **out_resolved, fr_error *err) {
    fr_resolver_entry *entries = NULL;
    size_t count = 0;
    cJSON *document = parse_keeping_the_document(document_json, &entries, &count, err);
    if (document == NULL) return FR_ERR;

    fr_registry *registry = fr_registry_create();
    fr_http_headers headers = { { { NULL, NULL } }, 0 };
    int status = FR_ERR;
    if (fr_lua_runtime_begin(".", registry, err) == FR_OK) {
        status = fr_resolvers_use(&entries[0], coordinate, out_url, out_resolved, &headers,
                                  err);
    }
    fr_http_headers_free(&headers);

    fr_resolvers_free(entries, count);
    cJSON_Delete(document);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    fr_resolvers_clear();
    return status;
}

TEST a_resolver_receives_its_own_block(void) {
    fr_error err;
    char *url = NULL;
    char *resolved = NULL;
    int status = use_resolver(
        "{\"resolvers\":{\"t\":{\"path\":\"./test/fixtures/resolver/resolver.lua\","
        "\"base\":\"https://mirror.invalid\"}}}",
        "a/b", &url, &resolved, &err);
    int matched = status == FR_OK && url != NULL
               && strcmp(url, "https://mirror.invalid/a/b.lua") == 0;
    free(url);
    free(resolved);
    ASSERT(matched);
    PASS();
}

TEST a_resolver_that_raises_is_reported_with_its_own_message(void) {
    fr_error err;
    char *url = NULL;
    char *resolved = NULL;
    int status = use_resolver("{\"resolvers\":{\"t\":{\"path\":\"./test/fixtures/resolver/raises.lua\"}}}",
                              "a/b", &url, &resolved, &err);
    char message[512];
    snprintf(message, sizeof message, "%s", err.message);
    free(url);
    free(resolved);

    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "no such thing") != NULL);
    ASSERT(strstr(message, "\"t\"") != NULL);
    ASSERT(strstr(message, "a/b") != NULL);
    PASS();
}

TEST a_resolver_returning_no_url_is_refused(void) {
    fr_error err;
    char *url = NULL;
    char *resolved = NULL;
    int status = use_resolver("{\"resolvers\":{\"t\":{\"path\":\"./test/fixtures/resolver/no-url.lua\"}}}",
                              "a/b", &url, &resolved, &err);
    char message[512];
    snprintf(message, sizeof message, "%s", err.message);
    free(url);
    free(resolved);

    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "returned no url") != NULL);
    PASS();
}

/* Core forwards a resolver's headers to the request verbatim, so a value
   carrying a line break would let a plugin append a header of its own or split
   the request outright. This is the one place in the headers channel where a
   defect is an injection rather than a wrong answer. */
TEST a_header_value_holding_a_line_break_is_refused(void) {
    fr_error err;
    char *url = NULL;
    char *resolved = NULL;
    int status = use_resolver(
        "{\"resolvers\":{\"t\":{\"path\":\"./test/fixtures/resolver/header-injection.lua\"}}}",
        "a/b", &url, &resolved, &err);
    char message[512];
    snprintf(message, sizeof message, "%s", err.message);
    free(url);
    free(resolved);

    ASSERT_EQ(FR_ERR, status);
    ASSERTm(message, strstr(message, "line break") != NULL);
    PASS();
}

TEST more_headers_than_the_bound_are_refused(void) {
    fr_error err;
    char *url = NULL;
    char *resolved = NULL;
    int status = use_resolver(
        "{\"resolvers\":{\"t\":{\"path\":\"./test/fixtures/resolver/too-many-headers.lua\"}}}",
        "a/b", &url, &resolved, &err);
    char message[512];
    snprintf(message, sizeof message, "%s", err.message);
    free(url);
    free(resolved);

    ASSERT_EQ(FR_ERR, status);
    ASSERTm(message, strstr(message, "more than 16 headers") != NULL);
    PASS();
}

/* setmetatable is a base global a plugin keeps, so the table resolve RETURNS
   can carry an __index that answers for keys it does not hold. Every read of
   that table is raw, so the metamethod is never consulted and the answer is
   the absent one: swapping any of those reads to lua_getfield makes this
   resolver succeed with a url core never saw written down. */
TEST a_hostile_metatable_on_the_returned_table_is_never_consulted(void) {
    fr_error err;
    char *url = NULL;
    char *resolved = NULL;
    int status = use_resolver(
        "{\"resolvers\":{\"t\":{\"path\":\"./test/fixtures/resolver/hostile-result.lua\"}}}",
        "a/b", &url, &resolved, &err);
    char message[512];
    snprintf(message, sizeof message, "%s", err.message);
    int url_taken = url != NULL;
    free(url);
    free(resolved);

    ASSERT_EQ(FR_ERR, status);
    ASSERT_FALSE(url_taken);
    ASSERTm(message, strstr(message, "returned no url") != NULL);
    PASS();
}

TEST a_chunk_that_declares_no_resolver_is_refused(void) {
    fr_error err;
    char *url = NULL;
    char *resolved = NULL;
    int status = use_resolver(
        "{\"resolvers\":{\"t\":{\"path\":\"./test/fixtures/resolver/not-a-resolver.lua\"}}}",
        "a/b", &url, &resolved, &err);
    char message[512];
    snprintf(message, sizeof message, "%s", err.message);
    free(url);
    free(resolved);

    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "declares no resolver") != NULL);
    PASS();
}

/* Two coordinates through one resolver entry. The memo is what stops the second
   from re-reading and re-running the chunk, and a stub that fails on any request
   is what proves it rather than a timing guess. The fetch floor's own disk
   cache is disabled for the test: it is keyed by url alone, so a prior run
   having warmed it would let the second call succeed on a cache hit even with
   the memo gone, hiding exactly the regression this test exists to catch. */
TEST one_resolver_named_twice_is_acquired_once(void) {
    fr_error err;
    cJSON *document = cJSON_Parse(
        "{\"resolvers\":{\"t\":{\"url\":\"https://example.invalid/r.lua\","
        "\"sha256\":\"" RESOLVER_DIGEST "\"}}}");
    fr_resolver_entry *entries = NULL;
    size_t count = 0;
    fr_resolvers_parse(document, &entries, &count, &err);

    int cache_was_enabled = fr_cache_enabled();
    fr_cache_set_enabled(0);
    fr_http_fn previous = fr_http_set_backend(stub_resolver_chunk);
    fr_registry *registry = fr_registry_create();
    int began = fr_lua_runtime_begin(".", registry, &err) == FR_OK;

    char *first_url = NULL;
    char *first_resolved = NULL;
    fr_http_headers first_headers = { { { NULL, NULL } }, 0 };
    int first = fr_resolvers_use(&entries[0], "a/b", &first_url, &first_resolved,
                                 &first_headers, &err);
    fr_http_headers_free(&first_headers);

    fr_http_set_backend(stub_refuses);
    char *second_url = NULL;
    char *second_resolved = NULL;
    fr_http_headers second_headers = { { { NULL, NULL } }, 0 };
    int second = fr_resolvers_use(&entries[0], "c/d", &second_url, &second_resolved,
                                  &second_headers, &err);
    fr_http_headers_free(&second_headers);
    int matched = second == FR_OK && second_url != NULL
               && strcmp(second_url, "https://example.invalid/c/d.lua") == 0;

    free(first_url); free(first_resolved); free(second_url); free(second_resolved);
    fr_resolvers_free(entries, count);
    cJSON_Delete(document);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    fr_resolvers_clear();
    fr_http_set_backend(previous);
    fr_cache_set_enabled(cache_was_enabled);

    ASSERT(began);
    ASSERT_EQ(FR_OK, first);
    ASSERT(matched);
    PASS();
}

/* fr_resolvers_use dereferences entry->label on its first line, so a NULL
   entry reaching it has to fail cleanly here rather than segfault; a caller
   that forgets its own "resolver not found" check must still get a real
   daukle error back. */
TEST fr_resolvers_use_rejects_a_null_entry(void) {
    fr_error err;
    char *url = NULL;
    char *resolved = NULL;
    fr_http_headers headers = { { { NULL, NULL } }, 0 };
    int status = fr_resolvers_use(NULL, "a/b", &url, &resolved, &headers, &err);

    ASSERT_EQ(FR_ERR, status);
    ASSERT_STR_EQ("no resolver to use", err.message);
    ASSERT(url == NULL);
    ASSERT(resolved == NULL);
    PASS();
}

SUITE(resolvers_suite) {
    RUN_TEST(an_absent_table_yields_no_resolvers);
    RUN_TEST(a_url_entry_with_a_pin_parses);
    RUN_TEST(a_url_entry_without_a_pin_is_refused);
    RUN_TEST(a_local_resolver_needs_no_pin);
    RUN_TEST(an_entry_naming_both_forms_is_refused);
    RUN_TEST(an_entry_naming_neither_form_is_refused);
    RUN_TEST(an_unknown_key_is_kept_for_the_resolver);
    RUN_TEST(a_resolver_cannot_name_another_resolver);
    RUN_TEST(find_returns_the_entry_for_a_label);
    RUN_TEST(a_resolver_is_acquired_through_the_floor_and_invoked);
    RUN_TEST(a_resolver_receives_its_own_block);
    RUN_TEST(a_resolver_that_raises_is_reported_with_its_own_message);
    RUN_TEST(a_resolver_returning_no_url_is_refused);
    RUN_TEST(a_header_value_holding_a_line_break_is_refused);
    RUN_TEST(more_headers_than_the_bound_are_refused);
    RUN_TEST(a_hostile_metatable_on_the_returned_table_is_never_consulted);
    RUN_TEST(a_chunk_that_declares_no_resolver_is_refused);
    RUN_TEST(one_resolver_named_twice_is_acquired_once);
    RUN_TEST(fr_resolvers_use_rejects_a_null_entry);
}

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(resolvers_suite);
    GREATEST_MAIN_END();
}
