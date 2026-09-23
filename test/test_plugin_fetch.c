#include "plugin_fetch.h"

#include "cache.h"
#include "error.h"
#include "greatest.h"
#include "http.h"
#include "support.h"

#include <stdlib.h>
#include <string.h>

GREATEST_MAIN_DEFS();

static int requests;

static char *copy_body(const char *text, size_t *out_length) {
    size_t length = strlen(text);
    char *copy = malloc(length + 1);
    if (copy != NULL) memcpy(copy, text, length + 1);
    *out_length = length;
    return copy;
}

static int stub_bytes(const char *url, const fr_http_header *headers, size_t header_count,
                      char **out_body, size_t *out_length, fr_error *err) {
    (void) url; (void) headers; (void) header_count; (void) err;
    requests++;
    *out_body = copy_body("the artifact", out_length);
    return FR_OK;
}

static int stub_refuses(const char *url, const fr_http_header *headers, size_t header_count,
                        char **out_body, size_t *out_length, fr_error *err) {
    (void) url; (void) headers; (void) header_count; (void) out_body; (void) out_length;
    fr_error_set(err, "the network was reached");
    return FR_ERR;
}

static void unique_url(char *out, size_t out_size, const char *tag) {
    snprintf(out, out_size, "https://example.invalid/%d/%s.lua", fr_test_process_id(), tag);
}

TEST the_floor_fetches_the_bytes_at_a_url(void) {
    fr_error err;
    char url[256];
    unique_url(url, sizeof url, "plain");
    fr_plugin_fetch_discard(url);

    fr_http_fn previous = fr_http_set_backend(stub_bytes);
    char *text = NULL;
    int status = fr_plugin_fetch(url, NULL, 0, &text, &err);
    fr_http_set_backend(previous);

    int matched = status == FR_OK && text != NULL && strcmp(text, "the artifact") == 0;
    free(text);
    fr_plugin_fetch_discard(url);
    ASSERT(matched);
    PASS();
}

TEST a_second_fetch_is_served_from_cache_without_the_network(void) {
    fr_error err;
    char url[256];
    unique_url(url, sizeof url, "cached");
    fr_plugin_fetch_discard(url);

    fr_http_fn previous = fr_http_set_backend(stub_bytes);
    requests = 0;
    char *first = NULL;
    int first_status = fr_plugin_fetch(url, NULL, 0, &first, &err);
    int first_requests = requests;
    free(first);

    fr_http_set_backend(stub_refuses);
    char *second = NULL;
    int second_status = fr_plugin_fetch(url, NULL, 0, &second, &err);
    int matched = second_status == FR_OK && second != NULL && strcmp(second, "the artifact") == 0;
    free(second);

    fr_http_set_backend(previous);
    fr_plugin_fetch_discard(url);
    ASSERT_EQ(FR_OK, first_status);
    ASSERT_EQ(1, first_requests);
    ASSERT(matched);
    PASS();
}

TEST discarding_an_entry_forces_the_next_fetch_to_the_network(void) {
    fr_error err;
    char url[256];
    unique_url(url, sizeof url, "discard");
    fr_plugin_fetch_discard(url);

    fr_http_fn previous = fr_http_set_backend(stub_bytes);
    char *first = NULL;
    int first_status = fr_plugin_fetch(url, NULL, 0, &first, &err);
    free(first);

    fr_plugin_fetch_discard(url);
    requests = 0;
    char *second = NULL;
    int second_status = fr_plugin_fetch(url, NULL, 0, &second, &err);
    int second_requests = requests;
    free(second);

    fr_http_set_backend(previous);
    fr_plugin_fetch_discard(url);
    ASSERT_EQ(FR_OK, first_status);
    ASSERT_EQ(FR_OK, second_status);
    ASSERT_EQ(1, second_requests);
    PASS();
}

TEST two_urls_do_not_share_a_cache_entry(void) {
    fr_error err;
    char one[256];
    char two[256];
    unique_url(one, sizeof one, "one");
    unique_url(two, sizeof two, "two");
    fr_plugin_fetch_discard(one);
    fr_plugin_fetch_discard(two);

    fr_http_fn previous = fr_http_set_backend(stub_bytes);
    char *first = NULL;
    fr_plugin_fetch(one, NULL, 0, &first, &err);
    free(first);

    requests = 0;
    char *second = NULL;
    int status = fr_plugin_fetch(two, NULL, 0, &second, &err);
    int second_requests = requests;
    free(second);

    fr_http_set_backend(previous);
    fr_plugin_fetch_discard(one);
    fr_plugin_fetch_discard(two);
    ASSERT_EQ(FR_OK, status);
    ASSERT_EQ(1, second_requests);
    PASS();
}

TEST a_failed_fetch_reports_the_backend_error(void) {
    fr_error err;
    char url[256];
    unique_url(url, sizeof url, "failing");
    fr_plugin_fetch_discard(url);

    fr_http_fn previous = fr_http_set_backend(stub_refuses);
    char *text = NULL;
    int status = fr_plugin_fetch(url, NULL, 0, &text, &err);
    fr_http_set_backend(previous);
    free(text);
    fr_plugin_fetch_discard(url);

    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(err.message, "the network was reached") != NULL);
    PASS();
}

SUITE(plugin_fetch_suite) {
    RUN_TEST(the_floor_fetches_the_bytes_at_a_url);
    RUN_TEST(a_second_fetch_is_served_from_cache_without_the_network);
    RUN_TEST(discarding_an_entry_forces_the_next_fetch_to_the_network);
    RUN_TEST(two_urls_do_not_share_a_cache_entry);
    RUN_TEST(a_failed_fetch_reports_the_backend_error);
}

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(plugin_fetch_suite);
    GREATEST_MAIN_END();
}
