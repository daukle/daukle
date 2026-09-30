#include "greatest.h"
#include "net/http.h"
#include "http_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TOKEN_HEADER_NAME = "Authorization";
static const char *SERVED_BODY = "{\"served\":true}";

static void url_for(char *out, size_t size, int port, const char *path) {
    snprintf(out, size, "http://127.0.0.1:%d%s", port, path);
}

/* Drives the real backend rather than the test double: the header-drop rule
   lives inside each backend, below the seam fr_http_set_backend replaces, so
   nothing installed through it can reach the rule.

   Which origins differ is settled exhaustively offline in test_url, against
   the one comparison both backends now share. What only a real server can
   show is that the comparison is WIRED to the headers, so these two cases
   are a yes and a no, on the axis a loopback server can vary without
   depending on how the machine resolves a name. */
static int fetch(const char *url, char **body, fr_error *err) {
    fr_http_header header;
    header.name = TOKEN_HEADER_NAME;
    header.value = "Bearer test-token";
    size_t length = 0;
    return fr_http_backend_get(url, &header, 1, body, &length, err);
}

/* The positive control. Without it, a backend that dropped the header on
   EVERY redirect would satisfy the negative case below. */
TEST keeps_the_header_across_a_same_origin_redirect(void) {
    fr_test_server *server = fr_test_server_create();
    ASSERT(server != NULL);

    char target[256];
    char start[256];
    url_for(target, sizeof target, fr_test_server_port(server), "/target");
    url_for(start, sizeof start, fr_test_server_port(server), "/start");
    fr_test_server_add_redirect(server, "/start", 302, target);
    fr_test_server_add_body(server, "/target", SERVED_BODY);
    fr_test_server_start(server);

    char *body = NULL;
    static fr_error err;
    int result = fetch(start, &body, &err);
    fr_test_server_stop(server);

    ASSERT_EQm(err.message, FR_OK, result);
    ASSERT(body != NULL);
    ASSERT_STR_EQ(SERVED_BODY, body);
    free(body);

    ASSERT(fr_test_server_was_requested(server, "/start"));
    ASSERT(fr_test_server_was_requested(server, "/target"));
    ASSERT(fr_test_server_saw_header(server, "/start", TOKEN_HEADER_NAME));
    ASSERT(fr_test_server_saw_header(server, "/target", TOKEN_HEADER_NAME));

    fr_test_server_free(server);
    PASS();
}

TEST drops_the_header_when_the_redirect_leaves_the_origin(void) {
    fr_test_server *first = fr_test_server_create();
    fr_test_server *second = fr_test_server_create();
    ASSERT(first != NULL);
    ASSERT(second != NULL);

    char target[256];
    char start[256];
    url_for(target, sizeof target, fr_test_server_port(second), "/target");
    url_for(start, sizeof start, fr_test_server_port(first), "/start");
    fr_test_server_add_redirect(first, "/start", 302, target);
    fr_test_server_add_body(second, "/target", SERVED_BODY);
    fr_test_server_start(first);
    fr_test_server_start(second);

    char *body = NULL;
    static fr_error err;
    int result = fetch(start, &body, &err);
    fr_test_server_stop(first);
    fr_test_server_stop(second);

    ASSERT_EQm(err.message, FR_OK, result);
    ASSERT(body != NULL);
    ASSERT_STR_EQ(SERVED_BODY, body);
    free(body);

    ASSERT(fr_test_server_saw_header(first, "/start", TOKEN_HEADER_NAME));
    ASSERT(fr_test_server_was_requested(second, "/target"));
    ASSERT_FALSE(fr_test_server_saw_header(second, "/target", TOKEN_HEADER_NAME));

    fr_test_server_free(first);
    fr_test_server_free(second);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(keeps_the_header_across_a_same_origin_redirect);
    RUN_TEST(drops_the_header_when_the_redirect_leaves_the_origin);
    GREATEST_MAIN_END();
}
