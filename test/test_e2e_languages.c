#include "greatest.h"
#include "region.h"
#include "sync.h"

#include <stdlib.h>
#include <string.h>

/* The targets are written from these constants rather than read from the
   checked-in copies, so a clone that converted them to crlf cannot turn a
   byte-exact assertion into a line-ending assertion. */
static const char *GRADLE_TEMPLATE =
    "dependencies {\n"
    "    // daukle:begin\n"
    "    // daukle:end\n"
    "    testImplementation \"junit\"\n"
    "}\n";

static const char *CMAKE_TEMPLATE =
    "cmake_minimum_required(VERSION 3.20)\n"
    "project(demo C)\n"
    "\n"
    "add_executable(demo main.c)\n"
    "\n"
    "# daukle:begin\n"
    "# daukle:end\n";

static const char *PACKAGE_TEMPLATE =
    "{\n"
    "  \"name\": \"example\",\n"
    "  \"version\": \"1.0.0\",\n"
    "  \"dependencies\": {\n"
    "    \"@openauthjs/openauth\": \"^0.4.3\"\n"
    "  },\n"
    "  \"devDependencies\": {\n"
    "    \"typescript\": \"^5.4.0\"\n"
    "  }\n"
    "}\n";

static const char *LANGUAGES_MANIFEST = "test/fixtures/languages/daukle.json";
static const char *GRADLE_TARGET = "test/fixtures/languages/build.gradle";
static const char *CMAKE_TARGET = "test/fixtures/languages/CMakeLists.txt";
static const char *PACKAGE_TARGET = "test/fixtures/languages/package.json";
static const char *TWO_NPM_MANIFEST = "test/fixtures/two-npm/daukle.json";
static const char *TWO_NPM_REVERSED = "test/fixtures/two-npm/daukle-reversed.json";
static const char *TWO_NPM_TARGET = "test/fixtures/two-npm/package.json";
static const char *LEDGER_ORDER_MANIFEST = "test/fixtures/npm-ledger-order/daukle.json";
static const char *LEDGER_ORDER_TARGET = "test/fixtures/npm-ledger-order/package.json";
static const char *NOTHING_OWNED_MANIFEST = "test/fixtures/npm-nothing-owned/daukle.json";
static const char *NOTHING_OWNED_TARGET = "test/fixtures/npm-nothing-owned/package.json";

static void write_file(const char *path, const char *text) {
    fr_error err;
    fr_file_write_text(path, text, &err);
}

static void reset_language_targets(void) {
    write_file(GRADLE_TARGET, GRADLE_TEMPLATE);
    write_file(CMAKE_TARGET, CMAKE_TEMPLATE);
    write_file(PACKAGE_TARGET, PACKAGE_TEMPLATE);
}

static char *read_file(const char *path) {
    fr_error err;
    char *text = NULL;
    fr_file_read_text(path, &text, &err);
    return text;
}

static int sync_manifest(const char *manifest, int write, size_t *count) {
    fr_sync_report report;
    fr_error err;
    int status = fr_sync(manifest, write, 1, &report, &err);
    *count = report.count;
    fr_sync_report_free(&report);
    return status;
}

TEST one_declaration_writes_all_three_languages(void) {
    reset_language_targets();
    size_t count = 0;
    ASSERT_EQ(FR_OK, sync_manifest(LANGUAGES_MANIFEST, 1, &count));
    ASSERT_EQ(3, (int) count);

    char *gradle = read_file(GRADLE_TARGET);
    ASSERT_STR_EQ(
        "dependencies {\n"
        "    // daukle:begin\n"
        "    githubImplementation \"forebay:basekit:5.0.0:contracts\"\n"
        "    githubImplementation \"forebay:basekit:5.0.0:ir\"\n"
        "    // daukle:end\n"
        "    testImplementation \"junit\"\n"
        "}\n", gradle);
    free(gradle);

    char *cmake = read_file(CMAKE_TARGET);
    ASSERT_STR_EQ(
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(demo C)\n"
        "\n"
        "add_executable(demo main.c)\n"
        "\n"
        "# daukle:begin\n"
        "include(FetchContent)\n"
        "FetchContent_Declare(basekit-contracts URL \"https://example.test/basekit-contracts-5.0.0.tar.gz\")\n"
        "FetchContent_MakeAvailable(basekit-contracts)\n"
        "FetchContent_Declare(basekit-ir URL \"https://example.test/basekit-ir-5.0.0.tar.gz\" URL_HASH SHA256=9f2e1c4b)\n"
        "FetchContent_MakeAvailable(basekit-ir)\n"
        "target_link_libraries(demo PRIVATE basekit-contracts basekit-ir)\n"
        "# daukle:end\n", cmake);
    free(cmake);

    char *package = read_file(PACKAGE_TARGET);
    ASSERT_STR_EQ(
        "{\n"
        "  \"name\": \"example\",\n"
        "  \"version\": \"1.0.0\",\n"
        "  \"dependencies\": {\n"
        "    \"@openauthjs/openauth\": \"^0.4.3\",\n"
        "    \"@intisy-ai/basekit-contracts\": \"^5.0.0\",\n"
        "    \"@intisy-ai/basekit-ir\": \"^5.0.0\"\n"
        "  },\n"
        "  \"devDependencies\": {\n"
        "    \"typescript\": \"^5.4.0\"\n"
        "  },\n"
        "  \"daukle\": {\n"
        "    \"managed\": {\n"
        "      \"dependencies\": [\n"
        "        \"@intisy-ai/basekit-contracts\",\n"
        "        \"@intisy-ai/basekit-ir\"\n"
        "      ]\n"
        "    }\n"
        "  }\n"
        "}\n", package);
    free(package);

    reset_language_targets();
    PASS();
}

TEST the_second_run_across_languages_changes_nothing(void) {
    reset_language_targets();
    size_t count = 0;
    ASSERT_EQ(FR_OK, sync_manifest(LANGUAGES_MANIFEST, 1, &count));
    ASSERT_EQ(FR_OK, sync_manifest(LANGUAGES_MANIFEST, 0, &count));
    ASSERT_EQ(0, (int) count);
    reset_language_targets();
    PASS();
}

TEST check_reports_drift_in_the_npm_target(void) {
    reset_language_targets();
    size_t count = 0;
    sync_manifest(LANGUAGES_MANIFEST, 1, &count);

    fr_sync_report report; fr_error err;
    write_file(PACKAGE_TARGET, PACKAGE_TEMPLATE);
    ASSERT_EQ(FR_OK, fr_sync(LANGUAGES_MANIFEST, 0, 1, &report, &err));
    ASSERT_EQ(1, (int) report.count);
    ASSERT(strstr(report.files[0], "package.json") != NULL);
    fr_sync_report_free(&report);

    reset_language_targets();
    PASS();
}

TEST check_reports_drift_in_the_c_target(void) {
    reset_language_targets();
    size_t count = 0;
    sync_manifest(LANGUAGES_MANIFEST, 1, &count);

    fr_sync_report report; fr_error err;
    write_file(CMAKE_TARGET, CMAKE_TEMPLATE);
    ASSERT_EQ(FR_OK, fr_sync(LANGUAGES_MANIFEST, 0, 1, &report, &err));
    ASSERT_EQ(1, (int) report.count);
    ASSERT(strstr(report.files[0], "CMakeLists.txt") != NULL);
    fr_sync_report_free(&report);

    reset_language_targets();
    PASS();
}

TEST the_report_lists_only_the_target_that_drifted(void) {
    reset_language_targets();
    size_t count = 0;
    sync_manifest(LANGUAGES_MANIFEST, 1, &count);

    fr_sync_report report; fr_error err;
    write_file(GRADLE_TARGET, GRADLE_TEMPLATE);
    ASSERT_EQ(FR_OK, fr_sync(LANGUAGES_MANIFEST, 1, 1, &report, &err));
    ASSERT_EQ(1, (int) report.count);
    ASSERT(strstr(report.files[0], "build.gradle") != NULL);
    fr_sync_report_free(&report);

    reset_language_targets();
    PASS();
}

TEST two_npm_consumers_on_one_file_reach_a_fixed_point(void) {
    write_file(TWO_NPM_TARGET, PACKAGE_TEMPLATE);
    size_t count = 0;
    ASSERT_EQ(FR_OK, sync_manifest(TWO_NPM_MANIFEST, 1, &count));
    ASSERT_EQ(1, (int) count);
    ASSERT_EQ(FR_OK, sync_manifest(TWO_NPM_MANIFEST, 0, &count));
    ASSERT_EQ(0, (int) count);

    char *package = read_file(TWO_NPM_TARGET);
    ASSERT(strstr(package, "\"@intisy-ai/basekit-ir\": \"^5.0.0\"") != NULL);
    ASSERT(strstr(package, "\"devDependencies\"") != NULL);
    ASSERT(strstr(package, "\"typescript\": \"^5.4.0\"") != NULL);
    free(package);

    write_file(TWO_NPM_TARGET, PACKAGE_TEMPLATE);
    PASS();
}

TEST reversing_the_two_consumers_reaches_a_fixed_point_too(void) {
    write_file(TWO_NPM_TARGET, PACKAGE_TEMPLATE);
    size_t count = 0;
    ASSERT_EQ(FR_OK, sync_manifest(TWO_NPM_REVERSED, 1, &count));
    ASSERT_EQ(1, (int) count);
    ASSERT_EQ(FR_OK, sync_manifest(TWO_NPM_REVERSED, 0, &count));
    ASSERT_EQ(0, (int) count);

    char *package = read_file(TWO_NPM_TARGET);
    ASSERT(strstr(package, "\"@intisy-ai/basekit-contracts\": \"^5.0.0\"") != NULL);
    ASSERT(strstr(package, "\"@intisy-ai/basekit-ir\": \"^5.0.0\"") != NULL);
    free(package);

    write_file(TWO_NPM_TARGET, PACKAGE_TEMPLATE);
    PASS();
}

/* The resolver orders entries by module name, so a producer whose published
   package names sort the other way round is what makes an unsorted ledger
   visible: the members below keep resolved order and the ledger does not. */
TEST the_ledger_is_sorted_when_the_resolved_order_is_not(void) {
    write_file(LEDGER_ORDER_TARGET, PACKAGE_TEMPLATE);
    size_t count = 0;
    ASSERT_EQ(FR_OK, sync_manifest(LEDGER_ORDER_MANIFEST, 1, &count));
    ASSERT_EQ(1, (int) count);

    char *package = read_file(LEDGER_ORDER_TARGET);
    ASSERT_STR_EQ(
        "{\n"
        "  \"name\": \"example\",\n"
        "  \"version\": \"1.0.0\",\n"
        "  \"dependencies\": {\n"
        "    \"@openauthjs/openauth\": \"^0.4.3\",\n"
        "    \"@intisy/bayonet\": \"^1.8.0\",\n"
        "    \"@intisy-ai/basekit-contracts\": \"^5.0.0\"\n"
        "  },\n"
        "  \"devDependencies\": {\n"
        "    \"typescript\": \"^5.4.0\"\n"
        "  },\n"
        "  \"daukle\": {\n"
        "    \"managed\": {\n"
        "      \"dependencies\": [\n"
        "        \"@intisy-ai/basekit-contracts\",\n"
        "        \"@intisy/bayonet\"\n"
        "      ]\n"
        "    }\n"
        "  }\n"
        "}\n", package);
    free(package);

    write_file(LEDGER_ORDER_TARGET, PACKAGE_TEMPLATE);
    PASS();
}

/* The consumer names no modules, so the plugin resolves nothing and finds no
   ledger: the document must come back without even a "daukle" member, which is
   the one case that never reaches daukle.json_set at all. */
TEST a_package_json_it_owns_nothing_in_is_left_byte_for_byte(void) {
    write_file(NOTHING_OWNED_TARGET, PACKAGE_TEMPLATE);
    size_t count = 0;
    ASSERT_EQ(FR_OK, sync_manifest(NOTHING_OWNED_MANIFEST, 1, &count));
    ASSERT_EQ(0, (int) count);

    char *package = read_file(NOTHING_OWNED_TARGET);
    ASSERT_STR_EQ(PACKAGE_TEMPLATE, package);
    free(package);

    write_file(NOTHING_OWNED_TARGET, PACKAGE_TEMPLATE);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(one_declaration_writes_all_three_languages);
    RUN_TEST(the_second_run_across_languages_changes_nothing);
    RUN_TEST(check_reports_drift_in_the_npm_target);
    RUN_TEST(check_reports_drift_in_the_c_target);
    RUN_TEST(the_report_lists_only_the_target_that_drifted);
    RUN_TEST(two_npm_consumers_on_one_file_reach_a_fixed_point);
    RUN_TEST(reversing_the_two_consumers_reaches_a_fixed_point_too);
    RUN_TEST(the_ledger_is_sorted_when_the_resolved_order_is_not);
    RUN_TEST(a_package_json_it_owns_nothing_in_is_left_byte_for_byte);
    GREATEST_MAIN_END();
}
