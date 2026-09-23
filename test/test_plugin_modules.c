#include "greatest.h"
#include "plugin_modules.h"

#include "config_lua.h"
#include "error.h"
#include "registry.h"
#include "support.h"
#include "sync.h"
#include "tar.h"

#include <stdlib.h>
#include <string.h>

GREATEST_MAIN_DEFS();

static char archive_bytes[8192];

static size_t build_archive(void) {
    memset(archive_bytes, 0, sizeof archive_bytes);
    size_t offset = fr_test_tar_append(archive_bytes, 0, "plugin.lua", '0', "return 1", 8);
    offset = fr_test_tar_append(archive_bytes, offset, "lib/greeting.lua", '0', "return 22", 9);
    return fr_test_tar_end(archive_bytes, offset);
}

/* Opens the archive above, asks for module_name, and reports the message rather
   than the status: every caller here is about which refusal came back. */
static void member_message(const char *module_name, int *out_status, char *message, size_t size) {
    size_t length = build_archive();
    fr_plugin_source *source = NULL;
    fr_error err;
    message[0] = '\0';
    *out_status = FR_ERR;

    if (fr_plugin_source_open_bytes(archive_bytes, length, &source, &err) != FR_OK) {
        snprintf(message, size, "%s", err.message);
        return;
    }

    const char *text = NULL;
    size_t text_length = 0;
    *out_status = fr_plugin_source_member(source, module_name, &text, &text_length, &err);
    if (*out_status != FR_OK) snprintf(message, size, "%s", err.message);
    fr_plugin_source_close(source);
}

TEST an_archive_serves_its_entry_member(void) {
    size_t length = build_archive();
    fr_plugin_source *source = NULL;
    fr_error err;
    ASSERT_EQ(FR_OK, fr_plugin_source_open_bytes(archive_bytes, length, &source, &err));

    const char *text = NULL;
    size_t text_length = 0;
    int status = fr_plugin_source_entry(source, &text, &text_length, &err);
    int matched = status == FR_OK && text_length == 8 && memcmp(text, "return 1", 8) == 0;
    fr_plugin_source_close(source);

    ASSERT_EQ(FR_OK, status);
    ASSERT(matched);
    PASS();
}

TEST an_archive_serves_a_module(void) {
    size_t length = build_archive();
    fr_plugin_source *source = NULL;
    fr_error err;
    ASSERT_EQ(FR_OK, fr_plugin_source_open_bytes(archive_bytes, length, &source, &err));

    const char *text = NULL;
    size_t text_length = 0;
    int status = fr_plugin_source_member(source, "lib/greeting", &text, &text_length, &err);
    int matched = status == FR_OK && text_length == 9 && memcmp(text, "return 22", 9) == 0;
    fr_plugin_source_close(source);

    ASSERT_EQ(FR_OK, status);
    ASSERT(matched);
    PASS();
}

TEST an_archive_without_an_entry_member_is_refused(void) {
    memset(archive_bytes, 0, sizeof archive_bytes);
    size_t offset = fr_test_tar_append(archive_bytes, 0, "lib/greeting.lua", '0', "return 1", 8);
    offset = fr_test_tar_end(archive_bytes, offset);

    fr_plugin_source *source = NULL;
    fr_error err;
    ASSERT_EQ(FR_OK, fr_plugin_source_open_bytes(archive_bytes, offset, &source, &err));

    const char *text = NULL;
    size_t length = 0;
    int status = fr_plugin_source_entry(source, &text, &length, &err);
    char message[512];
    snprintf(message, sizeof message, "%s", err.message);
    fr_plugin_source_close(source);

    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "no plugin.lua") != NULL);
    PASS();
}

/* The .lua rule is daukle's, not tar's, so it is enforced when the archive is
   opened as a plugin rather than when it is read as bytes. */
TEST an_archive_carrying_something_other_than_lua_is_refused(void) {
    memset(archive_bytes, 0, sizeof archive_bytes);
    size_t offset = fr_test_tar_append(archive_bytes, 0, "plugin.lua", '0', "return 1", 8);
    offset = fr_test_tar_append(archive_bytes, offset, "README.md", '0', "hello", 5);
    offset = fr_test_tar_end(archive_bytes, offset);

    fr_plugin_source *source = NULL;
    fr_error err;
    int status = fr_plugin_source_open_bytes(archive_bytes, offset, &source, &err);
    char message[512];
    snprintf(message, sizeof message, "%s", err.message);
    fr_plugin_source_close(source);

    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "is not a .lua file") != NULL);
    PASS();
}

TEST an_archive_member_that_climbs_out_is_refused(void) {
    memset(archive_bytes, 0, sizeof archive_bytes);
    size_t offset = fr_test_tar_append(archive_bytes, 0, "plugin.lua", '0', "return 1", 8);
    offset = fr_test_tar_append(archive_bytes, offset, "../escape.lua", '0', "return 2", 8);
    offset = fr_test_tar_end(archive_bytes, offset);

    fr_plugin_source *source = NULL;
    fr_error err;
    int status = fr_plugin_source_open_bytes(archive_bytes, offset, &source, &err);
    char message[512];
    snprintf(message, sizeof message, "%s", err.message);
    fr_plugin_source_close(source);

    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "would leave the plugin") != NULL);
    PASS();
}

TEST a_single_chunk_plugin_has_no_modules(void) {
    static const char chunk[] = "daukle.plugin{ api = 1 }\n";
    fr_plugin_source *source = NULL;
    fr_error err;
    ASSERT_EQ(FR_OK, fr_plugin_source_open_bytes(chunk, sizeof chunk - 1, &source, &err));

    const char *text = NULL;
    size_t length = 0;
    int status = fr_plugin_source_member(source, "lib/greeting", &text, &length, &err);
    char message[512];
    snprintf(message, sizeof message, "%s", err.message);

    int entry_status = fr_plugin_source_entry(source, &text, &length, &err);
    int entry_is_the_chunk = entry_status == FR_OK && length == sizeof chunk - 1;
    fr_plugin_source_close(source);

    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "single file") != NULL);
    ASSERT(entry_is_the_chunk);
    PASS();
}

TEST a_module_name_written_as_a_file_name_is_refused(void) {
    int status = FR_OK;
    char message[512];
    member_message("lib/greeting.lua", &status, message, sizeof message);
    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "without the \".lua\"") != NULL);
    PASS();
}

TEST an_empty_module_name_is_refused(void) {
    int status = FR_OK;
    char message[512];
    member_message("", &status, message, sizeof message);
    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "empty module name") != NULL);
    PASS();
}

TEST a_module_name_that_climbs_out_is_refused(void) {
    int status = FR_OK;
    char message[512];
    member_message("../escape", &status, message, sizeof message);
    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "may not leave the plugin") != NULL);
    PASS();
}

TEST an_absolute_module_name_is_refused(void) {
    int status = FR_OK;
    char message[512];
    member_message("/escape", &status, message, sizeof message);
    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "may not leave the plugin") != NULL);

    member_message("C:/escape", &status, message, sizeof message);
    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "may not leave the plugin") != NULL);
    PASS();
}

TEST a_module_name_with_a_backslash_is_refused(void) {
    int status = FR_OK;
    char message[512];
    member_message("lib\\greeting", &status, message, sizeof message);
    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "separates with") != NULL);
    PASS();
}

TEST a_module_name_ending_in_a_separator_is_refused(void) {
    int status = FR_OK;
    char message[512];
    member_message("lib/", &status, message, sizeof message);
    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "does not end in") != NULL);
    PASS();
}

TEST a_module_name_past_the_length_limit_is_refused(void) {
    char name[FR_TAR_MAX_NAME + 2];
    memset(name, 'a', sizeof name - 1);
    name[sizeof name - 1] = '\0';

    int status = FR_OK;
    char message[512];
    member_message(name, &status, message, sizeof message);
    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "longer than") != NULL);
    PASS();
}

/* The syntax child spec 6 will use is refused by name now, so arriving later is
   not a breaking change. */
/* One colon separates a plugin from its module, and config_lua.c splits on the
   first one before it gets here, so what reaches this check is the member half
   alone: a second colon in it names nothing. */
TEST a_module_name_with_a_second_colon_is_refused(void) {
    int status = FR_OK;
    char message[512];
    member_message("java:semver", &status, message, sizeof message);
    ASSERT_EQ(FR_ERR, status);
    ASSERTm(message, strstr(message, "at most one \":\"") != NULL);
    ASSERTm(message, strstr(message, "separates a plugin from its module") != NULL);
    PASS();
}

TEST a_module_no_member_matches_lists_what_there_is(void) {
    int status = FR_OK;
    char message[512];
    member_message("lib/absent", &status, message, sizeof message);
    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "lib/absent.lua") != NULL);
    ASSERT(strstr(message, "lib/greeting.lua") != NULL);
    PASS();
}

TEST a_directory_serves_its_entry_and_its_modules(void) {
    fr_registry *registry = NULL;
    fr_error err;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin("test/fixtures/plugin-directory", registry, &err));

    fr_plugin_source *source = NULL;
    int opened = fr_plugin_source_open_directory(fr_lua_runtime_state(), "plugin", &source, &err);

    const char *entry = NULL;
    size_t entry_length = 0;
    int entry_status = FR_ERR;
    const char *module = NULL;
    size_t module_length = 0;
    int module_status = FR_ERR;
    if (opened == FR_OK) {
        entry_status = fr_plugin_source_entry(source, &entry, &entry_length, &err);
        module_status = fr_plugin_source_member(source, "lib/greeting", &module, &module_length,
                                                &err);
    }
    int module_is_the_file = module_status == FR_OK && module_length > 0
                             && strstr(module, "greeting") != NULL;
    char message[512];
    snprintf(message, sizeof message, "%s", err.message);

    fr_plugin_source_close(source);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();

    ASSERT_EQm(message, FR_OK, opened);
    ASSERT_EQm(message, FR_OK, entry_status);
    ASSERT(entry_length > 0);
    ASSERT_EQm(message, FR_OK, module_status);
    ASSERT(module_is_the_file);
    PASS();
}

TEST a_directory_module_is_read_once_and_kept(void) {
    fr_registry *registry = NULL;
    fr_error err;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin("test/fixtures/plugin-directory", registry, &err));

    fr_plugin_source *source = NULL;
    int opened = fr_plugin_source_open_directory(fr_lua_runtime_state(), "plugin", &source, &err);

    const char *first = NULL;
    const char *second = NULL;
    size_t length = 0;
    int first_status = FR_ERR;
    int second_status = FR_ERR;
    if (opened == FR_OK) {
        first_status = fr_plugin_source_member(source, "lib/greeting", &first, &length, &err);
        second_status = fr_plugin_source_member(source, "lib/greeting", &second, &length, &err);
    }
    fr_plugin_source_close(source);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();

    ASSERT_EQ(FR_OK, opened);
    ASSERT_EQ(FR_OK, first_status);
    ASSERT_EQ(FR_OK, second_status);
    /* The same pointer, which is what makes the bytes outlive the call for every
       form rather than only for an archive. */
    ASSERT_EQ(first, second);
    PASS();
}

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(an_archive_serves_its_entry_member);
    RUN_TEST(an_archive_serves_a_module);
    RUN_TEST(an_archive_without_an_entry_member_is_refused);
    RUN_TEST(an_archive_carrying_something_other_than_lua_is_refused);
    RUN_TEST(an_archive_member_that_climbs_out_is_refused);
    RUN_TEST(a_single_chunk_plugin_has_no_modules);
    RUN_TEST(a_module_name_written_as_a_file_name_is_refused);
    RUN_TEST(an_empty_module_name_is_refused);
    RUN_TEST(a_module_name_that_climbs_out_is_refused);
    RUN_TEST(an_absolute_module_name_is_refused);
    RUN_TEST(a_module_name_with_a_backslash_is_refused);
    RUN_TEST(a_module_name_ending_in_a_separator_is_refused);
    RUN_TEST(a_module_name_past_the_length_limit_is_refused);
    RUN_TEST(a_module_name_with_a_second_colon_is_refused);
    RUN_TEST(a_module_no_member_matches_lists_what_there_is);
    RUN_TEST(a_directory_serves_its_entry_and_its_modules);
    RUN_TEST(a_directory_module_is_read_once_and_kept);
    GREATEST_MAIN_END();
}
