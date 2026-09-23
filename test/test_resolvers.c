#include "resolvers.h"

#include "cJSON.h"
#include "greatest.h"

#include <string.h>

GREATEST_MAIN_DEFS();

static int parse(const char *json, fr_resolver_entry **out, size_t *out_count, fr_error *err) {
    cJSON *document = cJSON_Parse(json);
    int status = fr_resolvers_parse(document, out, out_count, err);
    cJSON_Delete(document);
    return status;
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
}

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(resolvers_suite);
    GREATEST_MAIN_END();
}
