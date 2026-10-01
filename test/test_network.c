#include "greatest.h"
#include "net/http.h"

#include "support.h"

#include <stdio.h>
#include <stdlib.h>

/* The only case in the suite whose verdict depends on a machine daukle does not
   own, so it is also the only one that can go red without a defect: on
   2026-10-01 it timed out on a macOS runner whose network was failing to reach
   codeload.github.com at the same moment. Retrying keeps the assertion on every
   platform while absorbing that, and a real outage still fails after the last
   attempt, so red here still means something. Attempts are deliberately few: the
   point is to survive a blip, not to wait out a sustained failure.

   The wait between attempts is the part that was missing until 2026-10-01, and
   its absence was measured rather than argued: github returned 500 three times
   in 56 milliseconds on an ubuntu runner, which is one attempt wearing a loop,
   and a re-run minutes later was green. A retry with no wait survives a blip
   only if the blip is shorter than three round trips. */
#define FR_NETWORK_ATTEMPTS 3
#define FR_NETWORK_RETRY_SECONDS 2

TEST fetches_a_real_release_asset(void) {
    if (getenv("DAUKLE_NETWORK_TESTS") == NULL) SKIPm("DAUKLE_NETWORK_TESTS is not set");

    char *body = NULL;
    size_t length = 0;
    fr_error err;
    int status = FR_ERR;
    for (int attempt = 1; attempt <= FR_NETWORK_ATTEMPTS; attempt++) {
        status = fr_http_get(
            "https://github.com/forebay/basekit/releases/download/5.0.0/basekit-contracts.jar",
            NULL, 0, &body, &length, &err);
        if (status == FR_OK) break;
        fprintf(stderr, "network attempt %d of %d failed: %s\n", attempt, FR_NETWORK_ATTEMPTS,
                err.message);
        if (attempt < FR_NETWORK_ATTEMPTS) fr_test_sleep_seconds(FR_NETWORK_RETRY_SECONDS);
    }
    /* err lives on this frame and greatest keeps the message POINTER, so the
       last attempt's reason is copied somewhere that outlives the failure. */
    static char last_failure[256];
    snprintf(last_failure, sizeof last_failure, "%s", status == FR_OK ? "" : err.message);
    ASSERT_EQm(last_failure, FR_OK, status);
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
