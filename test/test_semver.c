#include "greatest.h"
#include "util/semver.h"

#include <string.h>

TEST parses_a_plain_version(void) {
    fr_version v; fr_error err;
    ASSERT_EQ(FR_OK, fr_version_parse("5.0.0", &v, &err));
    ASSERT_EQ(5, v.major); ASSERT_EQ(0, v.minor); ASSERT_EQ(0, v.patch);
    PASS();
}

TEST rejects_a_malformed_version(void) {
    fr_version v; fr_error err;
    ASSERT_EQ(FR_ERR, fr_version_parse("5.0", &v, &err));
    ASSERT(strstr(err.message, "5.0") != NULL);
    PASS();
}

TEST parses_each_range_kind(void) {
    fr_range r; fr_error err;
    ASSERT_EQ(FR_OK, fr_range_parse("^5.0.0", &r, &err));
    ASSERT_EQ(FR_RANGE_CARET, r.kind); ASSERT_EQ(5, r.base.major);
    ASSERT_EQ(FR_OK, fr_range_parse("~1.4.0", &r, &err));
    ASSERT_EQ(FR_RANGE_TILDE, r.kind); ASSERT_EQ(4, r.base.minor);
    ASSERT_EQ(FR_OK, fr_range_parse("2.1.3", &r, &err));
    ASSERT_EQ(FR_RANGE_EXACT, r.kind); ASSERT_EQ(3, r.base.patch);
    PASS();
}

TEST caret_allows_minor_and_patch_but_not_major(void) {
    fr_range r; fr_version v; fr_error err;
    fr_range_parse("^5.1.0", &r, &err);
    fr_version_parse("5.1.0", &v, &err); ASSERT_EQ(1, fr_range_satisfies(&r, &v));
    fr_version_parse("5.9.3", &v, &err); ASSERT_EQ(1, fr_range_satisfies(&r, &v));
    fr_version_parse("5.0.9", &v, &err); ASSERT_EQ(0, fr_range_satisfies(&r, &v));
    fr_version_parse("6.0.0", &v, &err); ASSERT_EQ(0, fr_range_satisfies(&r, &v));
    PASS();
}

TEST tilde_allows_patch_but_not_minor(void) {
    fr_range r; fr_version v; fr_error err;
    fr_range_parse("~1.4.2", &r, &err);
    fr_version_parse("1.4.9", &v, &err); ASSERT_EQ(1, fr_range_satisfies(&r, &v));
    fr_version_parse("1.4.1", &v, &err); ASSERT_EQ(0, fr_range_satisfies(&r, &v));
    fr_version_parse("1.5.0", &v, &err); ASSERT_EQ(0, fr_range_satisfies(&r, &v));
    PASS();
}

TEST exact_allows_only_itself(void) {
    fr_range r; fr_version v; fr_error err;
    fr_range_parse("2.1.3", &r, &err);
    fr_version_parse("2.1.3", &v, &err); ASSERT_EQ(1, fr_range_satisfies(&r, &v));
    fr_version_parse("2.1.4", &v, &err); ASSERT_EQ(0, fr_range_satisfies(&r, &v));
    PASS();
}

TEST rejects_an_oversized_component(void) {
    fr_version v; fr_error err;
    ASSERT_EQ(FR_ERR, fr_version_parse("99999999999999999999.0.0", &v, &err));
    PASS();
}

TEST rejects_leading_whitespace(void) {
    fr_version v; fr_error err;
    ASSERT_EQ(FR_ERR, fr_version_parse(" 5.0.0", &v, &err));
    PASS();
}

TEST rejects_whitespace_before_a_later_component(void) {
    fr_version v; fr_error err;
    ASSERT_EQ(FR_ERR, fr_version_parse("5. 0.0", &v, &err));
    ASSERT_EQ(FR_ERR, fr_version_parse("5.0. 0", &v, &err));
    PASS();
}

TEST rejects_a_negative_component(void) {
    fr_version v; fr_error err;
    ASSERT_EQ(FR_ERR, fr_version_parse("-1.0.0", &v, &err));
    PASS();
}

TEST rejects_a_plus_sign(void) {
    fr_version v; fr_error err;
    ASSERT_EQ(FR_ERR, fr_version_parse("+1.0.0", &v, &err));
    PASS();
}

TEST accepts_the_largest_component(void) {
    fr_version v; fr_error err;
    ASSERT_EQ(FR_OK, fr_version_parse("1000000000.0.0", &v, &err));
    ASSERT_EQ(1000000000, v.major);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(parses_a_plain_version);
    RUN_TEST(rejects_a_malformed_version);
    RUN_TEST(parses_each_range_kind);
    RUN_TEST(caret_allows_minor_and_patch_but_not_major);
    RUN_TEST(tilde_allows_patch_but_not_minor);
    RUN_TEST(exact_allows_only_itself);
    RUN_TEST(rejects_an_oversized_component);
    RUN_TEST(rejects_leading_whitespace);
    RUN_TEST(rejects_whitespace_before_a_later_component);
    RUN_TEST(rejects_a_negative_component);
    RUN_TEST(rejects_a_plus_sign);
    RUN_TEST(accepts_the_largest_component);
    GREATEST_MAIN_END();
}
