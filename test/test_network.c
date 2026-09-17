#include "greatest.h"
#include "http.h"

#include <stdlib.h>

TEST fetches_a_real_release_asset(void) {
    if (getenv("TERKO_NETWORK_TESTS") == NULL) SKIPm("TERKO_NETWORK_TESTS is not set");

    char *body = NULL;
    size_t length = 0;
    fr_error err;
    ASSERT_EQ(FR_OK, fr_http_get(
        "https://github.com/forebay/basekit/releases/download/5.0.0/basekit-contracts.jar",
        NULL, 0, &body, &length, &err));
    ASSERT(length > 1000);
    ASSERT_EQ('P', body[0]);
    ASSERT_EQ('K', body[1]);
    free(body);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(fetches_a_real_release_asset);
    GREATEST_MAIN_END();
}
