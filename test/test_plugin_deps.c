#include "greatest.h"
#include "plugin_deps.h"

#include "cache.h"
#include "config_lua.h"
#include "error.h"
#include "http.h"
#include "plugins.h"
#include "sha256.h"
#include "support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

GREATEST_MAIN_DEFS();

/* Digests are computed here from the bytes the stub will serve, never pasted:
   a pasted digest is computed over one line ending and checked against
   whatever a checkout wrote, which is how the acquisition branch shipped a
   test that only failed on the merge run. */
static char JAVA_DIGEST[65];
static char FOO_DIGEST[65];

static const char JAVA_CHUNK[] = "daukle.plugin{ api = 1, exports = { \"lib/coords\" } }\n";
static const char FOO_CHUNK[]  = "daukle.plugin{ api = 1 }\n";

static const char ZERO_DIGEST[] =
    "0000000000000000000000000000000000000000000000000000000000000000";

#define STUB_MAX 160

typedef struct {
    const char *url;
    const char *body;
    size_t length;
} stub_entry;

static stub_entry STUB[STUB_MAX];
static size_t stub_count;
static int stub_requests;

static void stub_reset(void) {
    stub_count = 0;
    stub_requests = 0;
}

static void stub_serve(const char *url, const char *body, size_t length) {
    if (stub_count == STUB_MAX) return;
    STUB[stub_count].url = url;
    STUB[stub_count].body = body;
    STUB[stub_count].length = length;
    stub_count++;
}

/* malloc + memcpy rather than strdup: strdup is not C11 and MSVC's /W4 /WX
   treats its own deprecation warning for it as an error. A length is carried
   rather than measured, because an archive body holds NULs. */
static char *copy_bytes(const char *data, size_t length) {
    char *copy = malloc(length + 1);
    if (copy == NULL) return NULL;
    memcpy(copy, data, length);
    copy[length] = '\0';
    return copy;
}

static int serve_stub(const char *url, const fr_http_header *headers, size_t header_count,
                      char **out_body, size_t *out_length, fr_error *err) {
    (void) headers; (void) header_count;
    for (size_t index = 0; index < stub_count; index++) {
        if (strcmp(url, STUB[index].url) != 0) continue;
        stub_requests++;
        *out_body = copy_bytes(STUB[index].body, STUB[index].length);
        if (*out_body == NULL) {
            fr_error_set(err, "out of memory serving %s", url);
            return FR_ERR;
        }
        *out_length = STUB[index].length;
        return FR_OK;
    }
    fr_error_set(err, "no stub for %s", url);
    return FR_ERR;
}

static void compute_digests(void) {
    fr_sha256_hex(JAVA_CHUNK, strlen(JAVA_CHUNK), JAVA_DIGEST);
    fr_sha256_hex(FOO_CHUNK, strlen(FOO_CHUNK), FOO_DIGEST);
}

static void serve_two_plugins(void) {
    stub_reset();
    compute_digests();
    stub_serve("https://x/java.lua", JAVA_CHUNK, strlen(JAVA_CHUNK));
    stub_serve("https://x/foo.lua", FOO_CHUNK, strlen(FOO_CHUNK));
}

/* One "alias = { url = ..., sha256 = ... }" entry of a requires table. */
static void requirement_entry(char *out, size_t size, const char *alias, const char *url,
                              const char *digest) {
    snprintf(out, size, "%s = { url = \"%s\", sha256 = \"%s\" }", alias, url, digest);
}

/* Builds a dependent's declaration source naming url with digest under alias. */
static void dependent_source(char *out, size_t size, const char *alias, const char *url,
                             const char *digest) {
    char entry[256];
    requirement_entry(entry, sizeof entry, alias, url, digest);
    snprintf(out, size, "daukle.plugin{ api = 1, requires = { %s } }\n", entry);
}

/* Reads a dependent's declaration and acquires its graph, reporting the message
   rather than the status. */
static int acquire_for(const char *source, fr_plugin_deps **out_deps, char *message, size_t size) {
    fr_error err;
    fr_plugin_declaration declaration;
    memset(&declaration, 0, sizeof declaration);
    message[0] = '\0';
    *out_deps = NULL;

    int cache_was_enabled = fr_cache_enabled();
    fr_cache_set_enabled(0);
    fr_http_fn previous = fr_http_set_backend(serve_stub);

    int status = fr_lua_runtime_begin(".", NULL, &err);
    if (status == FR_OK) {
        status = fr_plugins_read_declaration(source, strlen(source), "@test", "plugin", "gradle",
                                             &declaration, &err);
    }
    if (status == FR_OK) {
        status = fr_plugin_deps_acquire(&declaration, NULL, NULL, 0, out_deps, &err);
    }
    if (status != FR_OK) snprintf(message, size, "%s", err.message);

    fr_plugins_free_declaration(&declaration);
    fr_http_set_backend(previous);
    fr_cache_set_enabled(cache_was_enabled);
    return status;
}

TEST a_declared_dependency_is_acquired(void) {
    static char message[512];
    char source[512];
    serve_two_plugins();
    dependent_source(source, sizeof source, "java", "https://x/java.lua", JAVA_DIGEST);

    fr_plugin_deps *deps = NULL;
    int status = acquire_for(source, &deps, message, sizeof message);
    fr_plugin_deps_close(deps);
    fr_lua_runtime_shutdown();

    ASSERT_EQm(message, FR_OK, status);
    PASS();
}

TEST a_dependency_whose_digest_does_not_match_is_refused(void) {
    static char message[512];
    char source[512];
    serve_two_plugins();
    dependent_source(source, sizeof source, "java", "https://x/java.lua", ZERO_DIGEST);

    fr_plugin_deps *deps = NULL;
    int status = acquire_for(source, &deps, message, sizeof message);
    fr_plugin_deps_close(deps);
    fr_lua_runtime_shutdown();

    ASSERT_EQ(FR_ERR, status);
    ASSERTm(message, strstr(message, "expected sha256") != NULL);
    ASSERTm(message, strstr(message, JAVA_DIGEST) != NULL);
    PASS();
}

TEST the_same_artifact_under_two_aliases_is_fetched_once(void) {
    static char message[512];
    char source[512];
    char first[256];
    char second[256];
    char third[256];
    serve_two_plugins();
    requirement_entry(first, sizeof first, "java", "https://x/java.lua", JAVA_DIGEST);
    requirement_entry(second, sizeof second, "jvm", "https://x/java.lua", JAVA_DIGEST);
    requirement_entry(third, sizeof third, "foo", "https://x/foo.lua", FOO_DIGEST);
    snprintf(source, sizeof source, "daukle.plugin{ api = 1, requires = { %s, %s, %s } }\n", first,
             second, third);

    fr_plugin_deps *deps = NULL;
    int status = acquire_for(source, &deps, message, sizeof message);
    int requests = stub_requests;
    fr_plugin_deps_close(deps);
    fr_lua_runtime_shutdown();

    ASSERT_EQm(message, FR_OK, status);
    ASSERT_EQm("the artifact two aliases name is fetched once", 2, requests);
    PASS();
}

static char JAVA_ARCHIVE[8192];
static size_t JAVA_ARCHIVE_LENGTH;
static char JAVA_ARCHIVE_DIGEST[65];

static const char COORDS_MODULE[] = "return { parse = function(text) return text end }\n";
static const char SCRATCH_MODULE[] = "return { secret = true }\n";

static void serve_java_archive(void) {
    stub_reset();
    memset(JAVA_ARCHIVE, 0, sizeof JAVA_ARCHIVE);
    size_t offset = fr_test_tar_append(JAVA_ARCHIVE, 0, "plugin.lua", '0', JAVA_CHUNK,
                                       sizeof JAVA_CHUNK - 1);
    offset = fr_test_tar_append(JAVA_ARCHIVE, offset, "lib/coords.lua", '0', COORDS_MODULE,
                                sizeof COORDS_MODULE - 1);
    offset = fr_test_tar_append(JAVA_ARCHIVE, offset, "internal/scratch.lua", '0', SCRATCH_MODULE,
                                sizeof SCRATCH_MODULE - 1);
    JAVA_ARCHIVE_LENGTH = fr_test_tar_end(JAVA_ARCHIVE, offset);
    fr_sha256_hex(JAVA_ARCHIVE, JAVA_ARCHIVE_LENGTH, JAVA_ARCHIVE_DIGEST);
    stub_serve("https://x/java.lua", JAVA_ARCHIVE, JAVA_ARCHIVE_LENGTH);
}

TEST an_exported_member_is_served_and_an_unexported_one_is_not(void) {
    static char acquire_message[512];
    static char exported_message[512];
    static char refused_message[512];
    char source[512];
    acquire_message[0] = exported_message[0] = refused_message[0] = '\0';
    serve_java_archive();
    dependent_source(source, sizeof source, "java", "https://x/java.lua", JAVA_ARCHIVE_DIGEST);

    fr_plugin_deps *deps = NULL;
    int status = acquire_for(source, &deps, acquire_message, sizeof acquire_message);

    fr_error err;
    const char *text = NULL;
    size_t length = 0;
    fr_plugin_deps *owner = NULL;
    const char *owner_label = NULL;
    int exported_status = FR_ERR;
    int refused_status = FR_OK;
    int served_the_module = 0;
    int named_its_owner = 0;
    if (status == FR_OK) {
        exported_status = fr_plugin_deps_member(deps, "java", "lib/coords", &text, &length, &owner,
                                                &owner_label, &err);
        if (exported_status != FR_OK) {
            snprintf(exported_message, sizeof exported_message, "%s", err.message);
        } else {
            served_the_module = length == sizeof COORDS_MODULE - 1
                && memcmp(text, COORDS_MODULE, length) == 0;
            named_its_owner = owner != NULL && owner_label != NULL
                && strcmp(owner_label, "java") == 0;
        }
        refused_status = fr_plugin_deps_member(deps, "java", "internal/scratch", &text, &length,
                                               &owner, &owner_label, &err);
        if (refused_status != FR_OK) {
            snprintf(refused_message, sizeof refused_message, "%s", err.message);
        }
    }
    fr_plugin_deps_close(deps);
    fr_lua_runtime_shutdown();

    ASSERT_EQm(acquire_message, FR_OK, status);
    ASSERT_EQm(exported_message, FR_OK, exported_status);
    ASSERTm("an exported member is served as the archive holds it", served_the_module);
    ASSERTm("an exported member names the artifact's own deps and label", named_its_owner);
    ASSERT_EQ(FR_ERR, refused_status);
    ASSERTm(refused_message, strstr(refused_message, "does not export") != NULL);
    ASSERTm(refused_message, strstr(refused_message, "lib/coords") != NULL);
    PASS();
}

TEST an_unknown_alias_is_refused_naming_the_aliases_that_exist(void) {
    static char acquire_message[512];
    static char message[512];
    char source[512];
    acquire_message[0] = message[0] = '\0';
    serve_java_archive();
    dependent_source(source, sizeof source, "java", "https://x/java.lua", JAVA_ARCHIVE_DIGEST);

    fr_plugin_deps *deps = NULL;
    int status = acquire_for(source, &deps, acquire_message, sizeof acquire_message);

    fr_error err;
    const char *text = NULL;
    size_t length = 0;
    fr_plugin_deps *owner = NULL;
    const char *owner_label = NULL;
    int member_status = FR_OK;
    if (status == FR_OK) {
        member_status = fr_plugin_deps_member(deps, "kotlin", "lib/coords", &text, &length, &owner,
                                              &owner_label, &err);
        if (member_status != FR_OK) snprintf(message, sizeof message, "%s", err.message);
    }
    fr_plugin_deps_close(deps);
    fr_lua_runtime_shutdown();

    ASSERT_EQm(acquire_message, FR_OK, status);
    ASSERT_EQ(FR_ERR, member_status);
    ASSERTm(message, strstr(message, "it requires java") != NULL);
    PASS();
}

static char CYCLE_A_URL[] = "https://x/cycle-a.lua";
static char CYCLE_B_URL[] = "https://x/cycle-b.lua";
static char CYCLE_A_BODY[512];
static char CYCLE_B_BODY[512];
static char CYCLE_A_DIGEST[65];
static char CYCLE_B_DIGEST[65];

/* @implNote b names a with a placeholder digest because no real one exists: a's
   bytes hold b's digest, so a digest of a that b could carry would have to be
   computed before a was written. The cycle is refused before any digest of the
   repeated url is compared, which is what makes the placeholder harmless. */
static void serve_a_cycle(void) {
    stub_reset();
    dependent_source(CYCLE_B_BODY, sizeof CYCLE_B_BODY, "back", CYCLE_A_URL, ZERO_DIGEST);
    fr_sha256_hex(CYCLE_B_BODY, strlen(CYCLE_B_BODY), CYCLE_B_DIGEST);
    dependent_source(CYCLE_A_BODY, sizeof CYCLE_A_BODY, "onward", CYCLE_B_URL, CYCLE_B_DIGEST);
    fr_sha256_hex(CYCLE_A_BODY, strlen(CYCLE_A_BODY), CYCLE_A_DIGEST);
    stub_serve(CYCLE_A_URL, CYCLE_A_BODY, strlen(CYCLE_A_BODY));
    stub_serve(CYCLE_B_URL, CYCLE_B_BODY, strlen(CYCLE_B_BODY));
}

TEST a_cycle_between_two_artifacts_is_refused(void) {
    static char message[512];
    char source[512];
    char chain[256];
    serve_a_cycle();
    dependent_source(source, sizeof source, "a", CYCLE_A_URL, CYCLE_A_DIGEST);
    snprintf(chain, sizeof chain, "%s -> %s -> %s", CYCLE_A_URL, CYCLE_B_URL, CYCLE_A_URL);

    fr_plugin_deps *deps = NULL;
    int status = acquire_for(source, &deps, message, sizeof message);
    fr_plugin_deps_close(deps);
    fr_lua_runtime_shutdown();

    ASSERT_EQ(FR_ERR, status);
    ASSERTm(message, strstr(message, "is a cycle") != NULL);
    ASSERTm(message, strstr(message, chain) != NULL);
    PASS();
}

#define GENERATED_MAX 160

static char GENERATED_URL[GENERATED_MAX][40];
static char GENERATED_BODY[GENERATED_MAX][512];
static char GENERATED_DIGEST[GENERATED_MAX][65];

/* A chain of length artifacts, the last requiring nothing. Built from the end
   backwards, because an artifact's bytes carry the digest of the one it
   requires. */
static void serve_a_chain(size_t length) {
    stub_reset();
    for (size_t index = length; index >= 1; index--) {
        snprintf(GENERATED_URL[index], sizeof GENERATED_URL[index], "https://x/chain-%zu.lua",
                 index);
        if (index == length) {
            snprintf(GENERATED_BODY[index], sizeof GENERATED_BODY[index],
                     "daukle.plugin{ api = 1 }\n");
        } else {
            dependent_source(GENERATED_BODY[index], sizeof GENERATED_BODY[index], "next",
                             GENERATED_URL[index + 1], GENERATED_DIGEST[index + 1]);
        }
        fr_sha256_hex(GENERATED_BODY[index], strlen(GENERATED_BODY[index]),
                      GENERATED_DIGEST[index]);
        stub_serve(GENERATED_URL[index], GENERATED_BODY[index], strlen(GENERATED_BODY[index]));
    }
}

TEST a_graph_at_the_depth_limit_is_acquired(void) {
    static char message[512];
    char source[512];
    serve_a_chain(FR_PLUGIN_DEPS_MAX_DEPTH);
    dependent_source(source, sizeof source, "head", GENERATED_URL[1], GENERATED_DIGEST[1]);

    fr_plugin_deps *deps = NULL;
    int status = acquire_for(source, &deps, message, sizeof message);
    fr_plugin_deps_close(deps);
    fr_lua_runtime_shutdown();

    ASSERT_EQm(message, FR_OK, status);
    PASS();
}

TEST a_graph_deeper_than_the_limit_is_refused(void) {
    static char message[512];
    char source[512];
    serve_a_chain(FR_PLUGIN_DEPS_MAX_DEPTH + 1);
    dependent_source(source, sizeof source, "head", GENERATED_URL[1], GENERATED_DIGEST[1]);

    fr_plugin_deps *deps = NULL;
    int status = acquire_for(source, &deps, message, sizeof message);
    fr_plugin_deps_close(deps);
    fr_lua_runtime_shutdown();

    ASSERT_EQ(FR_ERR, status);
    ASSERTm(message, strstr(message, "deeper than") != NULL);
    ASSERTm(message, strstr(message, GENERATED_URL[FR_PLUGIN_DEPS_MAX_DEPTH + 1]) != NULL);
    PASS();
}

#define TREE_NODES 127

/* A binary tree of TREE_NODES artifacts, seven levels deep and so past the node
   limit while well inside the depth limit. Built from the leaves backwards for
   the same reason the chain is. */
static void serve_a_tree(void) {
    stub_reset();
    for (size_t index = TREE_NODES; index >= 1; index--) {
        snprintf(GENERATED_URL[index], sizeof GENERATED_URL[index], "https://x/tree-%zu.lua",
                 index);
        size_t left = index * 2;
        size_t right = index * 2 + 1;
        if (right <= TREE_NODES) {
            char left_entry[256];
            char right_entry[256];
            requirement_entry(left_entry, sizeof left_entry, "a", GENERATED_URL[left],
                              GENERATED_DIGEST[left]);
            requirement_entry(right_entry, sizeof right_entry, "b", GENERATED_URL[right],
                              GENERATED_DIGEST[right]);
            snprintf(GENERATED_BODY[index], sizeof GENERATED_BODY[index],
                     "daukle.plugin{ api = 1, requires = { %s, %s } }\n", left_entry, right_entry);
        } else {
            snprintf(GENERATED_BODY[index], sizeof GENERATED_BODY[index],
                     "daukle.plugin{ api = 1 }\n");
        }
        fr_sha256_hex(GENERATED_BODY[index], strlen(GENERATED_BODY[index]),
                      GENERATED_DIGEST[index]);
        stub_serve(GENERATED_URL[index], GENERATED_BODY[index], strlen(GENERATED_BODY[index]));
    }
}

TEST a_graph_wider_than_the_node_limit_is_refused(void) {
    static char message[512];
    char source[512];
    serve_a_tree();
    dependent_source(source, sizeof source, "root", GENERATED_URL[1], GENERATED_DIGEST[1]);

    fr_plugin_deps *deps = NULL;
    int status = acquire_for(source, &deps, message, sizeof message);
    fr_plugin_deps_close(deps);
    fr_lua_runtime_shutdown();

    ASSERT_EQ(FR_ERR, status);
    ASSERTm(message, strstr(message, "more than 64") != NULL);
    PASS();
}

static char MANY_ALIASES_BODY[4096];
static char MANY_ALIASES_DIGEST[65];

static void serve_too_many_aliases(void) {
    stub_reset();
    size_t filled = (size_t) snprintf(MANY_ALIASES_BODY, sizeof MANY_ALIASES_BODY,
                                      "daukle.plugin{ api = 1, requires = {");
    for (int index = 0; index <= FR_PLUGIN_MAX_REQUIRES; index++) {
        char alias[32];
        char entry[256];
        snprintf(alias, sizeof alias, "dep%d", index);
        requirement_entry(entry, sizeof entry, alias, "https://x/leaf.lua", ZERO_DIGEST);
        filled += (size_t) snprintf(MANY_ALIASES_BODY + filled, sizeof MANY_ALIASES_BODY - filled,
                                    " %s,", entry);
    }
    snprintf(MANY_ALIASES_BODY + filled, sizeof MANY_ALIASES_BODY - filled, " } }\n");
    fr_sha256_hex(MANY_ALIASES_BODY, strlen(MANY_ALIASES_BODY), MANY_ALIASES_DIGEST);
    stub_serve("https://x/many.lua", MANY_ALIASES_BODY, strlen(MANY_ALIASES_BODY));
}

TEST a_dependency_naming_more_aliases_than_the_limit_is_refused(void) {
    static char message[512];
    char source[512];
    serve_too_many_aliases();
    dependent_source(source, sizeof source, "many", "https://x/many.lua", MANY_ALIASES_DIGEST);

    fr_plugin_deps *deps = NULL;
    int status = acquire_for(source, &deps, message, sizeof message);
    fr_plugin_deps_close(deps);
    fr_lua_runtime_shutdown();

    ASSERT_EQ(FR_ERR, status);
    ASSERTm(message, strstr(message, "more than 16 plugins") != NULL);
    PASS();
}

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(a_declared_dependency_is_acquired);
    RUN_TEST(a_dependency_whose_digest_does_not_match_is_refused);
    RUN_TEST(the_same_artifact_under_two_aliases_is_fetched_once);
    RUN_TEST(an_exported_member_is_served_and_an_unexported_one_is_not);
    RUN_TEST(an_unknown_alias_is_refused_naming_the_aliases_that_exist);
    RUN_TEST(a_cycle_between_two_artifacts_is_refused);
    RUN_TEST(a_graph_at_the_depth_limit_is_acquired);
    RUN_TEST(a_graph_deeper_than_the_limit_is_refused);
    RUN_TEST(a_graph_wider_than_the_node_limit_is_refused);
    RUN_TEST(a_dependency_naming_more_aliases_than_the_limit_is_refused);
    GREATEST_MAIN_END();
}
