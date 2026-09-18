#include "greatest.h"
#include "tomledit.h"

#include <stdlib.h>
#include <string.h>

static const char *ORIGINAL =
    "schema = 1\n"
    "project = \"forebay/x\"\n"
    "\n"
    "# the comment that must survive\n"
    "[[consumers]]\n"
    "id = \"stub\"\n"
    "language = \"npm\"\n"
    "\n"
    "  [consumers.dependencies.\"forebay/basekit\"]\n"
    "  version = \"^5.0.0\"\n"
    "  modules = [\"ir\"]\n";

TEST updates_the_range_of_a_dependency_already_there(void) {
    fr_error err;
    char *out = NULL;
    const char *modules[] = { "ir" };
    ASSERT_EQ(FR_OK, fr_toml_edit_set_dependency(ORIGINAL, "stub", "forebay/basekit",
                                                 "^5.1.0", modules, 1, &out, &err));
    ASSERT(strstr(out, "^5.1.0") != NULL);
    ASSERT(strstr(out, "^5.0.0") == NULL);
    ASSERT(strstr(out, "# the comment that must survive") != NULL);
    free(out);
    PASS();
}

TEST appends_a_dependency_that_was_not_there(void) {
    fr_error err;
    char *out = NULL;
    const char *modules[] = { "core", "api" };
    ASSERT_EQ(FR_OK, fr_toml_edit_set_dependency(ORIGINAL, "stub", "forebay/other",
                                                 "^1.0.0", modules, 2, &out, &err));
    ASSERT(strstr(out, "[consumers.dependencies.\"forebay/other\"]") != NULL);
    ASSERT(strstr(out, "modules = [\"core\", \"api\"]") != NULL);
    ASSERT(strstr(out, "^5.0.0") != NULL);
    ASSERT(strstr(out, "# the comment that must survive") != NULL);
    ASSERT(strstr(out, "  modules = [\"ir\"]\n  [consumers.dependencies.\"forebay/other\"]\n") != NULL);
    free(out);
    PASS();
}

TEST reports_a_consumer_that_does_not_exist(void) {
    fr_error err;
    char *out = NULL;
    const char *modules[] = { "ir" };
    ASSERT_EQ(FR_ERR, fr_toml_edit_set_dependency(ORIGINAL, "absent", "forebay/basekit",
                                                  "^5.0.0", modules, 1, &out, &err));
    ASSERT(strstr(err.message, "absent") != NULL);
    PASS();
}

static const char *NO_DEPENDENCIES_YET =
    "schema = 1\n"
    "project = \"forebay/x\"\n"
    "\n"
    "[[consumers]]\n"
    "id = \"stub\"\n"
    "language = \"npm\"\n";

TEST appends_a_dependency_under_a_consumer_that_has_none_yet(void) {
    fr_error err;
    char *out = NULL;
    const char *modules[] = { "ir" };
    ASSERT_EQ(FR_OK, fr_toml_edit_set_dependency(NO_DEPENDENCIES_YET, "stub", "forebay/basekit",
                                                 "^1.0.0", modules, 1, &out, &err));
    ASSERT(strstr(out, "[consumers.dependencies.\"forebay/basekit\"]") != NULL);
    ASSERT(strstr(out, "  version = \"^1.0.0\"") != NULL);
    free(out);
    PASS();
}

TEST appends_a_dependency_with_no_modules(void) {
    fr_error err;
    char *out = NULL;
    ASSERT_EQ(FR_OK, fr_toml_edit_set_dependency(ORIGINAL, "stub", "forebay/empty",
                                                 "^1.0.0", NULL, 0, &out, &err));
    ASSERT(strstr(out, "modules = []") != NULL);
    free(out);
    PASS();
}

static const char *TWO_CONSUMERS_SAME_PROJECT =
    "[[consumers]]\n"
    "id = \"alpha\"\n"
    "language = \"npm\"\n"
    "\n"
    "  [consumers.dependencies.\"forebay/basekit\"]\n"
    "  version = \"^1.0.0\"\n"
    "  modules = [\"ir\"]\n"
    "\n"
    "[[consumers]]\n"
    "id = \"beta\"\n"
    "language = \"gradle\"\n"
    "\n"
    "  [consumers.dependencies.\"forebay/basekit\"]\n"
    "  version = \"^2.0.0\"\n"
    "  modules = [\"ir\"]\n";

TEST updates_only_the_named_consumers_copy_of_a_shared_project(void) {
    fr_error err;
    char *out = NULL;
    const char *modules[] = { "ir" };
    ASSERT_EQ(FR_OK, fr_toml_edit_set_dependency(TWO_CONSUMERS_SAME_PROJECT, "beta",
                                                 "forebay/basekit", "^2.1.0", modules, 1, &out, &err));
    ASSERT(strstr(out, "^2.1.0") != NULL);
    ASSERT(strstr(out, "^1.0.0") != NULL);
    ASSERT(strstr(out, "^2.0.0") == NULL);
    free(out);
    PASS();
}

static const char *DECOY_ID_INSIDE_A_DEPENDENCY_TABLE =
    "schema = 1\n"
    "project = \"forebay/x\"\n"
    "\n"
    "[[consumers]]\n"
    "id = \"decoy\"\n"
    "language = \"npm\"\n"
    "\n"
    "  [consumers.dependencies.\"forebay/basekit\"]\n"
    "  version = \"^1.0.0\"\n"
    "  modules = [\"ir\"]\n"
    "\n"
    "  [consumers.dependencies.\"forebay/basekit\".configuration]\n"
    "  id = \"stub\"\n"
    "\n"
    "[[consumers]]\n"
    "id = \"stub\"\n"
    "language = \"gradle\"\n";

TEST does_not_confuse_an_id_line_inside_a_dependency_table_with_a_consumers_own(void) {
    fr_error err;
    char *out = NULL;
    const char *modules[] = { "ir" };
    ASSERT_EQ(FR_OK, fr_toml_edit_set_dependency(DECOY_ID_INSIDE_A_DEPENDENCY_TABLE, "stub",
                                                 "forebay/newproj", "^2.0.0", modules, 1, &out, &err));
    char *gradle = strstr(out, "language = \"gradle\"");
    char *newproj = strstr(out, "forebay/newproj");
    ASSERT(gradle != NULL);
    ASSERT(newproj != NULL);
    ASSERT(newproj > gradle);
    ASSERT(strstr(out, "  id = \"stub\"") != NULL);
    free(out);
    PASS();
}

TEST appends_a_dependency_when_the_file_has_no_trailing_newline(void) {
    fr_error err;
    char *out = NULL;
    const char *modules[] = { "ir" };
    size_t length = strlen(ORIGINAL);
    char *without_trailing_newline = (char *) malloc(length);
    memcpy(without_trailing_newline, ORIGINAL, length - 1);
    without_trailing_newline[length - 1] = '\0';

    ASSERT_EQ(FR_OK, fr_toml_edit_set_dependency(without_trailing_newline, "stub", "forebay/other",
                                                 "^1.0.0", modules, 1, &out, &err));
    ASSERT(strstr(out, "modules = [\"ir\"]\n  [consumers.dependencies.\"forebay/other\"]") != NULL);
    free(without_trailing_newline);
    free(out);
    PASS();
}

static const char *CRLF_ORIGINAL =
    "schema = 1\r\n"
    "project = \"forebay/x\"\r\n"
    "\r\n"
    "[[consumers]]\r\n"
    "id = \"stub\"\r\n"
    "language = \"npm\"\r\n";

static int has_bare_lf(const char *text) {
    for (const char *cursor = text; *cursor != '\0'; cursor++) {
        if (*cursor == '\n' && (cursor == text || cursor[-1] != '\r')) return 1;
    }
    return 0;
}

TEST keeps_crlf_line_endings_in_the_text_it_writes(void) {
    fr_error err;
    char *out = NULL;
    const char *modules[] = { "ir" };
    ASSERT_EQ(FR_OK, fr_toml_edit_set_dependency(CRLF_ORIGINAL, "stub", "forebay/basekit",
                                                 "^1.0.0", modules, 1, &out, &err));
    ASSERT(strstr(out, "[consumers.dependencies.\"forebay/basekit\"]") != NULL);
    ASSERT(!has_bare_lf(out));
    free(out);
    PASS();
}

static const char *VERSION_WITH_COMMENT =
    "[[consumers]]\n"
    "id = \"stub\"\n"
    "language = \"npm\"\n"
    "\n"
    "  [consumers.dependencies.\"forebay/basekit\"]\n"
    "  version = \"^5.0.0\"  # pinned, do not bump\n"
    "  modules = [\"ir\"]\n";

TEST preserves_an_inline_comment_on_the_version_line_it_updates(void) {
    fr_error err;
    char *out = NULL;
    const char *modules[] = { "ir" };
    ASSERT_EQ(FR_OK, fr_toml_edit_set_dependency(VERSION_WITH_COMMENT, "stub", "forebay/basekit",
                                                 "^5.1.0", modules, 1, &out, &err));
    ASSERT(strstr(out, "  version = \"^5.1.0\"  # pinned, do not bump\n") != NULL);
    ASSERT(strstr(out, "^5.0.0") == NULL);
    free(out);
    PASS();
}

static const char *VERSION_WITH_TRAILING_WHITESPACE =
    "[[consumers]]\n"
    "id = \"stub\"\n"
    "language = \"npm\"\n"
    "\n"
    "  [consumers.dependencies.\"forebay/basekit\"]\n"
    "  version = \"^5.0.0\"   \n"
    "  modules = [\"ir\"]\n";

TEST preserves_trailing_whitespace_on_a_version_line_with_no_comment(void) {
    fr_error err;
    char *out = NULL;
    const char *modules[] = { "ir" };
    ASSERT_EQ(FR_OK, fr_toml_edit_set_dependency(VERSION_WITH_TRAILING_WHITESPACE, "stub",
                                                 "forebay/basekit", "^5.1.0", modules, 1, &out, &err));
    ASSERT(strstr(out, "  version = \"^5.1.0\"   \n") != NULL);
    free(out);
    PASS();
}

static const char *FOUR_SPACE_INDENT =
    "[[consumers]]\n"
    "id = \"stub\"\n"
    "language = \"npm\"\n"
    "\n"
    "    [consumers.dependencies.\"forebay/basekit\"]\n"
    "    version = \"^1.0.0\"\n"
    "    modules = [\"ir\"]\n";

TEST mirrors_a_four_space_indent_when_appending_a_new_dependency(void) {
    fr_error err;
    char *out = NULL;
    const char *modules[] = { "ir" };
    ASSERT_EQ(FR_OK, fr_toml_edit_set_dependency(FOUR_SPACE_INDENT, "stub", "forebay/other",
                                                 "^2.0.0", modules, 1, &out, &err));
    ASSERT(strstr(out,
                 "    [consumers.dependencies.\"forebay/other\"]\n"
                 "    version = \"^2.0.0\"\n"
                 "    modules = [\"ir\"]\n") != NULL);
    free(out);
    PASS();
}

static const char *TAB_INDENT =
    "[[consumers]]\n"
    "id = \"stub\"\n"
    "language = \"npm\"\n"
    "\n"
    "\t[consumers.dependencies.\"forebay/basekit\"]\n"
    "\tversion = \"^1.0.0\"\n"
    "\tmodules = [\"ir\"]\n";

TEST mirrors_a_tab_indent_when_appending_a_new_dependency(void) {
    fr_error err;
    char *out = NULL;
    const char *modules[] = { "ir" };
    ASSERT_EQ(FR_OK, fr_toml_edit_set_dependency(TAB_INDENT, "stub", "forebay/other",
                                                 "^2.0.0", modules, 1, &out, &err));
    ASSERT(strstr(out,
                 "\t[consumers.dependencies.\"forebay/other\"]\n"
                 "\tversion = \"^2.0.0\"\n"
                 "\tmodules = [\"ir\"]\n") != NULL);
    free(out);
    PASS();
}

TEST reports_an_oversized_consumer_id_instead_of_truncating_it(void) {
    fr_error err;
    char *out = NULL;
    const char *modules[] = { "ir" };
    char oversized_id[600];
    memset(oversized_id, 'a', sizeof oversized_id - 1);
    oversized_id[sizeof oversized_id - 1] = '\0';

    ASSERT_EQ(FR_ERR, fr_toml_edit_set_dependency(ORIGINAL, oversized_id, "forebay/basekit",
                                                  "^5.0.0", modules, 1, &out, &err));
    ASSERT(strstr(err.message, "too long") != NULL);
    free(out);
    PASS();
}

TEST reports_an_oversized_project_name_instead_of_truncating_it(void) {
    fr_error err;
    char *out = NULL;
    const char *modules[] = { "ir" };
    char oversized_project[600];
    memset(oversized_project, 'b', sizeof oversized_project - 1);
    oversized_project[sizeof oversized_project - 1] = '\0';

    ASSERT_EQ(FR_ERR, fr_toml_edit_set_dependency(ORIGINAL, "stub", oversized_project,
                                                  "^5.0.0", modules, 1, &out, &err));
    ASSERT(strstr(err.message, "too long") != NULL);
    free(out);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(updates_the_range_of_a_dependency_already_there);
    RUN_TEST(appends_a_dependency_that_was_not_there);
    RUN_TEST(reports_a_consumer_that_does_not_exist);
    RUN_TEST(appends_a_dependency_under_a_consumer_that_has_none_yet);
    RUN_TEST(appends_a_dependency_with_no_modules);
    RUN_TEST(updates_only_the_named_consumers_copy_of_a_shared_project);
    RUN_TEST(does_not_confuse_an_id_line_inside_a_dependency_table_with_a_consumers_own);
    RUN_TEST(appends_a_dependency_when_the_file_has_no_trailing_newline);
    RUN_TEST(keeps_crlf_line_endings_in_the_text_it_writes);
    RUN_TEST(preserves_an_inline_comment_on_the_version_line_it_updates);
    RUN_TEST(preserves_trailing_whitespace_on_a_version_line_with_no_comment);
    RUN_TEST(mirrors_a_four_space_indent_when_appending_a_new_dependency);
    RUN_TEST(mirrors_a_tab_indent_when_appending_a_new_dependency);
    RUN_TEST(reports_an_oversized_consumer_id_instead_of_truncating_it);
    RUN_TEST(reports_an_oversized_project_name_instead_of_truncating_it);
    GREATEST_MAIN_END();
}
