#include "greatest.h"
#include "net/url.h"

TEST matches_a_url_with_itself_and_a_different_path(void) {
    ASSERT(fr_url_same_origin("https://github.com/a/b", "https://github.com/a/b"));
    ASSERT(fr_url_same_origin("https://github.com/a/b", "https://github.com/c/d?x=1#y"));
    ASSERT(fr_url_same_origin("https://github.com", "https://github.com/"));
    PASS();
}

/* The axis that actually bit: an https to http redirect kept the header,
   because neither backend compared the scheme, and the token went out in
   cleartext. */
TEST separates_two_schemes(void) {
    ASSERT_FALSE(fr_url_same_origin("https://github.com/a", "http://github.com/a"));
    ASSERT_FALSE(fr_url_same_origin("http://github.com/a", "https://github.com/a"));
    PASS();
}

TEST separates_two_hosts(void) {
    ASSERT_FALSE(fr_url_same_origin("https://github.com/a", "https://objects.github.com/a"));
    ASSERT_FALSE(fr_url_same_origin("http://127.0.0.1/a", "http://localhost/a"));
    PASS();
}

TEST separates_two_ports(void) {
    ASSERT_FALSE(fr_url_same_origin("http://host:8080/a", "http://host:9090/a"));
    ASSERT_FALSE(fr_url_same_origin("https://host/a", "https://host:8443/a"));
    PASS();
}

/* A url naming its scheme's default port is the same origin as one omitting
   it, or every redirect that spelled the port out would drop the header. */
TEST treats_an_explicit_default_port_as_the_default(void) {
    ASSERT(fr_url_same_origin("https://host/a", "https://host:443/a"));
    ASSERT(fr_url_same_origin("http://host/a", "http://host:80/a"));
    ASSERT_FALSE(fr_url_same_origin("https://host:80/a", "http://host:80/a"));
    PASS();
}

TEST ignores_case_in_the_scheme_and_host(void) {
    ASSERT(fr_url_same_origin("HTTPS://GitHub.COM/a", "https://github.com/a"));
    PASS();
}

/* Userinfo is not part of an origin and the last "@" ends it, so a url that
   dresses one host up as another is judged on the real host. */
TEST judges_a_url_carrying_userinfo_on_its_real_host(void) {
    ASSERT(fr_url_same_origin("https://token@github.com/a", "https://github.com/a"));
    ASSERT_FALSE(fr_url_same_origin("https://github.com/a", "https://github.com@evil.example/a"));
    PASS();
}

TEST keeps_the_colons_inside_an_ipv6_literal(void) {
    ASSERT(fr_url_same_origin("http://[::1]/a", "http://[::1]:80/a"));
    ASSERT_FALSE(fr_url_same_origin("http://[::1]:8080/a", "http://[::1]:9090/a"));
    ASSERT_FALSE(fr_url_same_origin("http://[::1]/a", "http://[::2]/a"));
    PASS();
}

/* Unparseable compares unequal, so the caller drops the headers rather than
   sending them somewhere it could not identify. */
TEST refuses_what_it_cannot_parse(void) {
    ASSERT_FALSE(fr_url_same_origin("github.com/a", "github.com/a"));
    ASSERT_FALSE(fr_url_same_origin("https:///a", "https:///a"));
    ASSERT_FALSE(fr_url_same_origin(NULL, "https://github.com/a"));
    ASSERT_FALSE(fr_url_same_origin("https://github.com/a", NULL));
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(matches_a_url_with_itself_and_a_different_path);
    RUN_TEST(separates_two_schemes);
    RUN_TEST(separates_two_hosts);
    RUN_TEST(separates_two_ports);
    RUN_TEST(treats_an_explicit_default_port_as_the_default);
    RUN_TEST(ignores_case_in_the_scheme_and_host);
    RUN_TEST(judges_a_url_carrying_userinfo_on_its_real_host);
    RUN_TEST(keeps_the_colons_inside_an_ipv6_literal);
    RUN_TEST(refuses_what_it_cannot_parse);
    GREATEST_MAIN_END();
}
