#include "greatest.h"

#include "config.h"
#include "config_lua.h"
#include "generate.h"
#include "lua_sandbox.h"
#include "manifest.h"
#include "region.h"
#include "registry.h"
#include "sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>

static char *manifest_directory_of(const char *manifest_path) {
    const char *last_slash = strrchr(manifest_path, '/');
    const char *last_backslash = strrchr(manifest_path, '\\');
    const char *last = last_slash;
    if (last_backslash != NULL && (last == NULL || last_backslash > last)) last = last_backslash;

    size_t length = last != NULL ? (size_t) (last - manifest_path) : 0;
    char *dir = malloc(length + 1);
    if (dir != NULL) {
        memcpy(dir, manifest_path, length);
        dir[length] = '\0';
    }
    return dir;
}

static int path_exists(const char *path) {
#ifdef _WIN32
    struct _stat info;
    return _stat(path, &info) == 0;
#else
    struct stat info;
    return stat(path, &info) == 0;
#endif
}

static int generate_with_dir_fails_with(const char *manifest_path, const char *manifest_dir,
                                        int write, char *message, size_t message_size) {
    fr_registry *registry = NULL;
    fr_manifest manifest; fr_error err;
    if (fr_build_registry(&registry, &err) != FR_OK) return 0;
    if (fr_config_load_file(manifest_path, registry, &manifest, &err) != FR_OK) {
        snprintf(message, message_size, "%s", err.message);
        fr_registry_destroy(registry);
        fr_lua_runtime_shutdown();
        return 0;
    }
    fr_sync_report report;
    memset(&report, 0, sizeof report);
    int status = fr_generate(&manifest, manifest_path, manifest_dir, registry, write, &report, &err);
    snprintf(message, message_size, "%s", err.message);
    fr_sync_report_free(&report);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    return status == FR_ERR;
}

static int generate_fails_with(const char *manifest_path, char *message, size_t message_size) {
    char *dir = manifest_directory_of(manifest_path);
    int refused = generate_with_dir_fails_with(manifest_path, dir, 1, message, message_size);
    free(dir);
    return refused;
}

TEST a_generated_path_that_climbs_out_is_refused_naming_the_key(void) {
    char message[512];
    int refused = generate_fails_with("test/fixtures/generate-climbing-key/daukle.toml",
                                      message, sizeof message);
    ASSERT(refused);
    ASSERT(strstr(message, "../escape.txt") != NULL);
    ASSERT(strstr(message, "must stay inside the derived directory") != NULL);
    PASS();
}

TEST a_generated_absolute_path_is_refused_naming_the_key(void) {
    char message[512];
    int refused = generate_fails_with("test/fixtures/generate-absolute-key/daukle.toml",
                                      message, sizeof message);
    ASSERT(refused);
    ASSERT(strstr(message, "/escape.txt") != NULL);
    ASSERT(strstr(message, "must stay inside the derived directory") != NULL);
    PASS();
}

TEST a_generated_backslash_path_is_refused_naming_the_key(void) {
    char message[512];
    int refused = generate_fails_with("test/fixtures/generate-backslash-key/daukle.toml",
                                      message, sizeof message);
    ASSERT(refused);
    ASSERT(strstr(message, "sub\\escape.txt") != NULL);
    ASSERT(strstr(message, "must stay inside the derived directory") != NULL);
    PASS();
}

TEST a_generated_newline_path_is_refused_naming_the_key(void) {
    char message[512];
    int refused = generate_fails_with("test/fixtures/generate-newline-key/daukle.toml",
                                      message, sizeof message);
    ASSERT(refused);
    ASSERT(strstr(message, "a\nb.txt") != NULL);
    ASSERT(strstr(message, "must stay inside the derived directory") != NULL);
    PASS();
}

TEST a_generated_number_value_is_refused_naming_the_key(void) {
    char message[512];
    int refused = generate_fails_with("test/fixtures/generate-number-value/daukle.toml",
                                      message, sizeof message);
    ASSERT(refused);
    ASSERT(strstr(message, "a.txt") != NULL);
    ASSERT(strstr(message, "must be a string") != NULL);
    PASS();
}

TEST a_generated_file_over_the_size_limit_is_refused_naming_the_key(void) {
    char message[512];
    int refused = generate_fails_with("test/fixtures/generate-oversized-value/daukle.toml",
                                      message, sizeof message);
    ASSERT(refused);
    ASSERT(strstr(message, "a.txt") != NULL);
    ASSERT(strstr(message, "is larger than the 1 MiB limit") != NULL);
    PASS();
}

/* Shared by every "a plugin leaks the project root" fixture: none of them can
   discover the absolute project root from inside a sandboxed plugin, so each
   manifest carries a placeholder this rewrites to the given canonical
   directory before the caller runs generate, and the caller restores the
   original text afterwards regardless of how its assertions come out. */
static int rewrite_placeholder(const char *manifest_path, const char *placeholder,
                               const char *replacement, char **out_original, fr_error *err) {
    char *original_text = NULL;
    if (fr_file_read_text(manifest_path, &original_text, err) != FR_OK) return FR_ERR;

    const char *found = strstr(original_text, placeholder);
    if (found == NULL) {
        free(original_text);
        return FR_ERR;
    }

    size_t prefix_length = (size_t) (found - original_text);
    size_t suffix_length = strlen(found + strlen(placeholder));
    size_t rewritten_length = prefix_length + strlen(replacement) + suffix_length + 1;
    char *rewritten_text = malloc(rewritten_length);
    if (rewritten_text == NULL) {
        free(original_text);
        return FR_ERR;
    }
    memcpy(rewritten_text, original_text, prefix_length);
    memcpy(rewritten_text + prefix_length, replacement, strlen(replacement));
    memcpy(rewritten_text + prefix_length + strlen(replacement), found + strlen(placeholder),
          suffix_length + 1);

    int status = fr_file_write_text(manifest_path, rewritten_text, err);
    free(rewritten_text);
    if (status != FR_OK) {
        free(original_text);
        return FR_ERR;
    }

    *out_original = original_text;
    return FR_OK;
}

TEST a_generated_file_naming_the_project_root_is_refused(void) {
    static const char *manifest_path = "test/fixtures/generate-root-path/daukle.toml";
    static const char *placeholder = "<placeholder>";

    char *dir = manifest_directory_of(manifest_path);
    ASSERT(dir != NULL);

    char *canonical = NULL;
    fr_error canonical_err;
    int canonicalised = fr_lua_sandbox_canonical_dir(dir, &canonical, &canonical_err) == FR_OK;
    ASSERT(canonicalised);

    char *original_text = NULL;
    fr_error err;
    int rewritten = rewrite_placeholder(manifest_path, placeholder, canonical, &original_text,
                                        &err) == FR_OK;
    ASSERT(rewritten);

    char message[512];
    int refused = generate_fails_with(manifest_path, message, sizeof message);

    int restored = fr_file_write_text(manifest_path, original_text, &err) == FR_OK;

    free(original_text);
    free(canonical);
    free(dir);

    ASSERT(restored);
    ASSERT(refused);
    ASSERT(strstr(message, "a.txt") != NULL);
    ASSERT(strstr(message, "names the project root; use the relative \"root\"") != NULL);
    PASS();
}

/* Every fixture path in this file's other tests contains a slash, so none of
   them can see fr_generate's own manifest_dir normalisation: "daukle sync"
   run from inside the project root hands fr_generate a manifest_dir of "",
   and fr_lua_sandbox_canonical_dir("") fails on both platforms, so a missing
   ("" -> ".") normalisation silently disables the root-path guard exactly
   there. This drives fr_generate with dir = "" directly rather than through
   generate_fails_with, so the guard has to work on the bare-manifest-dir
   seam callers hit in the ordinary case, and the leaked text is the canonical
   form of "." itself, the project root a bare "daukle sync" resolves to. */
TEST a_generated_file_naming_the_project_root_is_refused_with_an_empty_manifest_dir(void) {
    static const char *manifest_path = "test/fixtures/generate-root-path-empty-dir/daukle.toml";
    static const char *placeholder = "<placeholder>";

    char *canonical = NULL;
    fr_error canonical_err;
    int canonicalised = fr_lua_sandbox_canonical_dir(".", &canonical, &canonical_err) == FR_OK;
    ASSERT(canonicalised);

    char *original_text = NULL;
    fr_error err;
    int rewritten = rewrite_placeholder(manifest_path, placeholder, canonical, &original_text,
                                        &err) == FR_OK;
    ASSERT(rewritten);

    char message[512];
    int refused = generate_with_dir_fails_with(manifest_path, "", 0, message, sizeof message);

    int restored = fr_file_write_text(manifest_path, original_text, &err) == FR_OK;

    free(original_text);
    free(canonical);

    ASSERT(restored);
    ASSERT(refused);
    ASSERT(strstr(message, "a.txt") != NULL);
    ASSERT(strstr(message, "names the project root; use the relative \"root\"") != NULL);
    PASS();
}

#ifdef _WIN32
/* GetFinalPathNameByHandleA, which canonical_root comes from, returns
   backslashes, but CMake output spells the very same path with forward
   slashes, the form CMake actually wants. A guard that compares only the
   exact spelling misses the most likely real-world violation, so this feeds
   generate-root-path's placeholder fixture the canonical root with every
   backslash flipped to a forward slash instead of canonical_root's own
   spelling. POSIX has only one separator, so this case cannot arise there. */
TEST a_generated_file_naming_the_project_root_with_forward_slashes_is_refused(void) {
    static const char *manifest_path = "test/fixtures/generate-root-path/daukle.toml";
    static const char *placeholder = "<placeholder>";

    char *dir = manifest_directory_of(manifest_path);
    ASSERT(dir != NULL);

    char *canonical = NULL;
    fr_error canonical_err;
    int canonicalised = fr_lua_sandbox_canonical_dir(dir, &canonical, &canonical_err) == FR_OK;
    ASSERT(canonicalised);
    int has_backslash_form = strchr(canonical, '\\') != NULL;

    size_t length = strlen(canonical);
    char forward_slash[1024];
    ASSERT(length < sizeof forward_slash);
    for (size_t index = 0; index < length; index++) {
        char c = canonical[index];
        forward_slash[index] = c == '\\' ? '/' : c;
    }
    forward_slash[length] = '\0';

    char *original_text = NULL;
    fr_error err;
    int rewritten = rewrite_placeholder(manifest_path, placeholder, forward_slash, &original_text,
                                        &err) == FR_OK;
    ASSERT(rewritten);

    char message[512];
    int refused = generate_fails_with(manifest_path, message, sizeof message);

    int restored = fr_file_write_text(manifest_path, original_text, &err) == FR_OK;

    free(original_text);
    free(canonical);
    free(dir);

    ASSERT(restored);
    ASSERT(has_backslash_form);
    ASSERT(refused);
    ASSERT(strstr(message, "a.txt") != NULL);
    ASSERT(strstr(message, "names the project root; use the relative \"root\"") != NULL);
    PASS();
}
#endif

TEST a_toolchain_no_plugin_provides_is_refused_naming_it(void) {
    char message[512];
    int refused = generate_fails_with("test/fixtures/toolchain-unprovided/daukle.toml",
                                      message, sizeof message);
    ASSERT(refused);
    ASSERT(strstr(message, "no plugin provides toolchain") != NULL);
    ASSERT(strstr(message, "add it to [plugins]") != NULL);
    PASS();
}

TEST one_name_in_both_modes_is_refused_naming_both(void) {
    char message[512];
    int refused = generate_fails_with("test/fixtures/both-modes/daukle.toml",
                                      message, sizeof message);
    ASSERT(refused);
    ASSERT(strstr(message, "is declared as a toolchain and as a consumer's language") != NULL);
    PASS();
}

TEST generation_may_not_exec(void) {
    char message[512];
    int refused = generate_fails_with("test/fixtures/generate-execs/daukle.toml",
                                      message, sizeof message);
    ASSERT(refused);
    ASSERT(strstr(message, "is not available while generating") != NULL);
    PASS();
}

TEST generation_may_not_resolve_a_tool(void) {
    char message[512];
    int refused = generate_fails_with("test/fixtures/generate-resolves-a-tool/daukle.toml",
                                      message, sizeof message);
    ASSERT(refused);
    ASSERT(strstr(message, "is not available while generating") != NULL);
    PASS();
}

TEST an_empty_generate_table_creates_no_directory(void) {
    fr_registry *registry = NULL;
    fr_manifest manifest; fr_error err;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));
    ASSERT_EQ(FR_OK, fr_config_load_file("test/fixtures/generate-empty/daukle.toml", registry,
                                        &manifest, &err));

    fr_sync_report report;
    memset(&report, 0, sizeof report);
    char *dir = manifest_directory_of("test/fixtures/generate-empty/daukle.toml");
    int status = fr_generate(&manifest, "test/fixtures/generate-empty/daukle.toml", dir, registry,
                             1, &report, &err);

    int no_stub_dir = !path_exists("test/fixtures/generate-empty/build/daukle/stub");
    int no_root_dir = !path_exists("test/fixtures/generate-empty/build/daukle");
    size_t reported = report.count;

    free(dir);
    fr_sync_report_free(&report);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();

    ASSERT_EQ(FR_OK, status);
    ASSERT_EQ(0, (int) reported);
    ASSERT(no_stub_dir);
    ASSERT(no_root_dir);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(a_generated_path_that_climbs_out_is_refused_naming_the_key);
    RUN_TEST(a_generated_absolute_path_is_refused_naming_the_key);
    RUN_TEST(a_generated_backslash_path_is_refused_naming_the_key);
    RUN_TEST(a_generated_newline_path_is_refused_naming_the_key);
    RUN_TEST(a_generated_number_value_is_refused_naming_the_key);
    RUN_TEST(a_generated_file_over_the_size_limit_is_refused_naming_the_key);
    RUN_TEST(a_generated_file_naming_the_project_root_is_refused);
    RUN_TEST(a_generated_file_naming_the_project_root_is_refused_with_an_empty_manifest_dir);
#ifdef _WIN32
    RUN_TEST(a_generated_file_naming_the_project_root_with_forward_slashes_is_refused);
#endif
    RUN_TEST(a_toolchain_no_plugin_provides_is_refused_naming_it);
    RUN_TEST(one_name_in_both_modes_is_refused_naming_both);
    RUN_TEST(generation_may_not_exec);
    RUN_TEST(generation_may_not_resolve_a_tool);
    RUN_TEST(an_empty_generate_table_creates_no_directory);
    GREATEST_MAIN_END();
}
