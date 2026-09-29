#include "greatest.h"
#include "http.h"
#include "http_server.h"
#include "region.h"
#include "sha256.h"
#include "support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *LAST_URL = NULL;

static int stub_get(const char *url, const fr_http_header *headers, size_t header_count,
                    char **out_body, size_t *out_length, fr_error *err) {
    (void) headers; (void) header_count; (void) err;
    LAST_URL = url;
    const char *body = "{}";
    *out_length = strlen(body);
    *out_body = malloc(*out_length + 1);
    memcpy(*out_body, body, *out_length + 1);
    return FR_OK;
}

TEST routes_through_the_installed_backend(void) {
    fr_http_fn previous = fr_http_set_backend(stub_get);
    char *body = NULL;
    size_t length = 0;
    fr_error err;
    ASSERT_EQ(FR_OK, fr_http_get("https://example.invalid/x", NULL, 0, &body, &length, &err));
    ASSERT_STR_EQ("https://example.invalid/x", LAST_URL);
    ASSERT_STR_EQ("{}", body);
    ASSERT_EQ(2u, (unsigned) length);
    free(body);
    fr_http_set_backend(previous);
    PASS();
}

TEST restores_the_previous_backend(void) {
    fr_http_fn original = fr_http_set_backend(stub_get);
    fr_http_fn installed = fr_http_set_backend(original);
    ASSERT(installed == stub_get);
    PASS();
}

TEST a_streamed_body_lands_in_the_file_with_its_digest(void) {
    static char message[512];
    char body[70000];
    for (size_t index = 0; index < sizeof body; index++) body[index] = (char) (index % 256);

    char expected[65];
    fr_sha256_hex(body, sizeof body, expected);

    fr_test_server *server = fr_test_server_create();
    fr_test_server_add_body_bytes(server, "/blob", body, sizeof body);
    fr_test_server_start(server);

    char url[256];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/blob", fr_test_server_port(server));
    char path[1024];
    snprintf(path, sizeof path, "%s/streamed-%d.bin", fr_test_temp_base(), fr_test_process_id());

    fr_sha256 digest;
    fr_sha256_init(&digest);
    size_t length = 0;
    fr_error err;
    int result = fr_http_get_to_file(url, NULL, 0, path, FR_HTTP_MAX_FILE, &digest, &length, &err);
    char actual[65];
    fr_sha256_final(&digest, actual);

    char *read_back = NULL;
    size_t read_length = 0;
    int read_result = fr_file_read_bytes(path, &read_back, &read_length, &err);
    int identical = read_result == FR_OK && read_length == sizeof body
                 && memcmp(read_back, body, sizeof body) == 0;
    free(read_back);
    remove(path);
    fr_test_server_stop(server);
    fr_test_server_free(server);

    snprintf(message, sizeof message, "result %d, length %zu, digest %s", result, length, actual);
    ASSERT_EQm(message, FR_OK, result);
    ASSERT_EQm(message, sizeof body, length);
    ASSERT_STR_EQm(message, expected, actual);
    ASSERTm(message, identical);
    PASS();
}

TEST a_body_past_the_ceiling_is_refused_by_name(void) {
    static char message[512];
    char body[9000];
    memset(body, 'x', sizeof body);

    fr_test_server *server = fr_test_server_create();
    fr_test_server_add_body_bytes(server, "/big", body, sizeof body);
    fr_test_server_start(server);

    char url[256];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/big", fr_test_server_port(server));
    char path[1024];
    snprintf(path, sizeof path, "%s/toobig-%d.bin", fr_test_temp_base(), fr_test_process_id());

    size_t length = 0;
    fr_error err;
    err.message[0] = '\0';
    int result = fr_http_get_to_file(url, NULL, 0, path, 4096, NULL, &length, &err);
    snprintf(message, sizeof message, "%s", err.message);
    int left_behind = 0;
    FILE *probe = fopen(path, "rb");
    if (probe != NULL) { left_behind = 1; fclose(probe); remove(path); }
    fr_test_server_stop(server);
    fr_test_server_free(server);

    ASSERT_EQm(message, FR_ERR, result);
    ASSERTm(message, strstr(message, "larger than") != NULL);
    ASSERTm(message, strstr(message, "4096") != NULL);
    ASSERT_EQm("a refused transfer must not leave a partial file", 0, left_behind);
    PASS();
}

TEST the_streamed_fetch_follows_a_redirect(void) {
    static char message[512];
    const char body[] = "redirected-payload";

    fr_test_server *server = fr_test_server_create();
    fr_test_server_add_redirect(server, "/from", 302, "/to");
    fr_test_server_add_body_bytes(server, "/to", body, sizeof body - 1);
    fr_test_server_start(server);

    char url[256];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/from", fr_test_server_port(server));
    char path[1024];
    snprintf(path, sizeof path, "%s/redir-%d.bin", fr_test_temp_base(), fr_test_process_id());

    size_t length = 0;
    fr_error err;
    int result = fr_http_get_to_file(url, NULL, 0, path, FR_HTTP_MAX_FILE, NULL, &length, &err);
    remove(path);
    int reached = fr_test_server_was_requested(server, "/to");
    fr_test_server_stop(server);
    fr_test_server_free(server);

    snprintf(message, sizeof message, "result %d, length %zu", result, length);
    ASSERT_EQm(message, FR_OK, result);
    ASSERT_EQm(message, sizeof body - 1, length);
    ASSERTm(message, reached);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(routes_through_the_installed_backend);
    RUN_TEST(restores_the_previous_backend);
    RUN_TEST(a_streamed_body_lands_in_the_file_with_its_digest);
    RUN_TEST(a_body_past_the_ceiling_is_refused_by_name);
    RUN_TEST(the_streamed_fetch_follows_a_redirect);
    GREATEST_MAIN_END();
}
