#include "greatest.h"
#include "plugin/plugin_deps.h"

#include "cJSON.h"
#include "cache/cache.h"
#include "config/config_lua.h"
#include "util/error.h"
#include "net/http.h"
#include "plugin/plugins.h"
#include "plugin/registry.h"
#include "util/sha256.h"
#include "support.h"
#include "project/sync.h"

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

static const char JAVA_CHUNK[] =
    "daukle.plugin{ api = 1, exports = { \"lib/coords\", \"lib/paths\" } }\n";
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
   rather than the status. registry is what the runtime registers into, which
   only the test asking whether a dependency's chunk ran needs. */
static int acquire_into(const char *source, fr_registry *registry, fr_plugin_deps **out_deps,
                        char *message, size_t size) {
    fr_error err;
    fr_plugin_declaration declaration;
    memset(&declaration, 0, sizeof declaration);
    message[0] = '\0';
    *out_deps = NULL;

    int cache_was_enabled = fr_cache_enabled();
    fr_cache_set_enabled(0);
    fr_http_fn previous = fr_http_set_backend(serve_stub);

    int status = fr_lua_runtime_begin(".", registry, &err);
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

static int acquire_for(const char *source, fr_plugin_deps **out_deps, char *message, size_t size) {
    return acquire_into(source, NULL, out_deps, message, size);
}

/* Like acquire_for, but with a manifest override: overrides is the borrowed
   cJSON "requires" table a [plugins.<label>] entry's own "requires" key would
   carry, exactly as plugin_deps.c receives it from fr_plugin_entry.overrides.
   base_dir is the sandbox root a path-form override resolves against; every
   caller but the directory-override test passes "." and never writes a path
   override, so the sandbox is never actually consulted for them. */
static int acquire_with_overrides(const char *base_dir, const char *source,
                                  const cJSON *overrides, fr_plugin_deps **out_deps,
                                  char *message, size_t size) {
    fr_error err;
    fr_plugin_declaration declaration;
    memset(&declaration, 0, sizeof declaration);
    message[0] = '\0';
    *out_deps = NULL;

    int cache_was_enabled = fr_cache_enabled();
    fr_cache_set_enabled(0);
    fr_http_fn previous = fr_http_set_backend(serve_stub);

    int status = fr_lua_runtime_begin(base_dir, NULL, &err);
    if (status == FR_OK) {
        status = fr_plugins_read_declaration(source, strlen(source), "@test", "plugin", "gradle",
                                             &declaration, &err);
    }
    if (status == FR_OK) {
        status = fr_plugin_deps_acquire(&declaration, overrides, NULL, 0, out_deps, &err);
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
    size_t count = fr_plugin_deps_count(deps);
    int requests = stub_requests;
    fr_plugin_deps_close(deps);
    fr_lua_runtime_shutdown();

    ASSERT_EQm(message, FR_OK, status);
    /* FR_OK alone passes an acquirer that walks requires and fetches nothing:
       an artifact must actually have been fetched and land in the graph. */
    ASSERT_EQ(1u, count);
    ASSERT_EQ(1, requests);
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
static const char PATHS_MODULE[] = "return { join = function(a, b) return a .. b end }\n";
static const char SCRATCH_MODULE[] = "return { secret = true }\n";

static void serve_java_archive(void) {
    stub_reset();
    memset(JAVA_ARCHIVE, 0, sizeof JAVA_ARCHIVE);
    size_t offset = fr_test_tar_append(JAVA_ARCHIVE, 0, "plugin.lua", '0', JAVA_CHUNK,
                                       sizeof JAVA_CHUNK - 1);
    offset = fr_test_tar_append(JAVA_ARCHIVE, offset, "lib/coords.lua", '0', COORDS_MODULE,
                                sizeof COORDS_MODULE - 1);
    offset = fr_test_tar_append(JAVA_ARCHIVE, offset, "lib/paths.lua", '0', PATHS_MODULE,
                                sizeof PATHS_MODULE - 1);
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
    /* The whole list, not its first entry: an implementation naming only
       exports[0] would satisfy a one-element assertion. */
    ASSERTm(refused_message, strstr(refused_message, "lib/coords, lib/paths") != NULL);
    PASS();
}

TEST an_unknown_alias_is_refused_naming_the_aliases_that_exist(void) {
    static char acquire_message[512];
    static char message[512];
    char source[512];
    char first[256];
    char second[256];
    acquire_message[0] = message[0] = '\0';
    serve_two_plugins();
    requirement_entry(first, sizeof first, "java", "https://x/java.lua", JAVA_DIGEST);
    requirement_entry(second, sizeof second, "foo", "https://x/foo.lua", FOO_DIGEST);
    snprintf(source, sizeof source, "daukle.plugin{ api = 1, requires = { %s, %s } }\n", first,
             second);

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
    ASSERTm(message, strstr(message, "it requires") != NULL);
    /* Both aliases, in whichever order the declaration's table yielded them:
       naming only the first binding would satisfy a one-element assertion. */
    ASSERTm(message, strstr(message, "java") != NULL);
    ASSERTm(message, strstr(message, "foo") != NULL);
    PASS();
}

static char DISAGREEING_BODY[512];
static char DISAGREEING_DIGEST[65];

/* The second pin is reached through a SECOND artifact rather than a second
   alias of the dependent: a requires table yields its entries in whatever
   order lua chooses, so two aliases of one dependent decide by coin toss
   whether the wrong pin arrives first (an ordinary mismatch) or second (the
   memo). Acquisition finishes the dependent's own requires before walking any
   artifact's, so putting the wrong pin one level down makes the memo hit the
   only reachable path. */
static void serve_a_pin_disagreement(void) {
    serve_two_plugins();
    dependent_source(DISAGREEING_BODY, sizeof DISAGREEING_BODY, "jvm", "https://x/java.lua",
                     ZERO_DIGEST);
    fr_sha256_hex(DISAGREEING_BODY, strlen(DISAGREEING_BODY), DISAGREEING_DIGEST);
    stub_serve("https://x/mid.lua", DISAGREEING_BODY, strlen(DISAGREEING_BODY));
}

TEST one_url_pinned_to_two_digests_is_refused_naming_both(void) {
    static char message[512];
    char source[512];
    char first[256];
    char second[256];
    serve_a_pin_disagreement();
    requirement_entry(first, sizeof first, "java", "https://x/java.lua", JAVA_DIGEST);
    requirement_entry(second, sizeof second, "mid", "https://x/mid.lua", DISAGREEING_DIGEST);
    snprintf(source, sizeof source, "daukle.plugin{ api = 1, requires = { %s, %s } }\n", first,
             second);

    fr_plugin_deps *deps = NULL;
    int status = acquire_for(source, &deps, message, sizeof message);
    fr_plugin_deps_close(deps);
    fr_lua_runtime_shutdown();

    ASSERT_EQ(FR_ERR, status);
    ASSERTm(message, strstr(message, "already pinned to") != NULL);
    ASSERTm(message, strstr(message, JAVA_DIGEST) != NULL);
    ASSERTm(message, strstr(message, ZERO_DIGEST) != NULL);
    ASSERTm(message, strstr(message, "alias \"mid\"") != NULL);
    PASS();
}

static const char WOULD_REGISTER_CHUNK[] =
    "daukle.plugin{ api = 1, uses = {} }\n"
    "daukle.language{ name = 'never-run',\n"
    "                 apply = function(consumer, resolved, text) return text end }\n";
static char WOULD_REGISTER_DIGEST[65];

/* The invariant the whole design rests on, and the one a grep cannot prove: a
   dependency is acquired and read, never loaded. Its chunk registers a
   language after the declaration, so a loader hiding anywhere in acquisition
   shows up as that language reaching the registry. */
TEST a_dependencys_entry_chunk_is_never_run(void) {
    static char message[512];
    char source[512];
    fr_error err;
    stub_reset();
    fr_sha256_hex(WOULD_REGISTER_CHUNK, strlen(WOULD_REGISTER_CHUNK), WOULD_REGISTER_DIGEST);
    stub_serve("https://x/runs.lua", WOULD_REGISTER_CHUNK, strlen(WOULD_REGISTER_CHUNK));
    dependent_source(source, sizeof source, "runs", "https://x/runs.lua", WOULD_REGISTER_DIGEST);

    fr_registry *registry = NULL;
    int built = fr_build_registry(&registry, &err);
    fr_plugin_deps *deps = NULL;
    int status = FR_ERR;
    int registered = 1;
    if (built == FR_OK) {
        status = acquire_into(source, registry, &deps, message, sizeof message);
        registered = fr_registry_language(registry, "daukle.language/never-run") != NULL;
    }
    fr_plugin_deps_close(deps);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();

    ASSERT_EQ(FR_OK, built);
    ASSERT_EQm(message, FR_OK, status);
    ASSERTm("the dependency's entry chunk did not run", !registered);
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
    dependent_source(source, sizeof source, "start", CYCLE_A_URL, CYCLE_A_DIGEST);
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
            requirement_entry(left_entry, sizeof left_entry, "left", GENERATED_URL[left],
                              GENERATED_DIGEST[left]);
            requirement_entry(right_entry, sizeof right_entry, "right", GENERATED_URL[right],
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

/* One alias past FR_PLUGIN_MAX_REQUIRES. Appends are bounds-checked so that a
   body outgrowing its buffer fails as a short declaration rather than as
   pointer arithmetic past the end. */
static void serve_too_many_aliases(void) {
    stub_reset();
    size_t filled = 0;
    int written = snprintf(MANY_ALIASES_BODY, sizeof MANY_ALIASES_BODY,
                           "daukle.plugin{ api = 1, requires = {");
    if (written > 0 && (size_t) written < sizeof MANY_ALIASES_BODY) filled = (size_t) written;
    for (int index = 0; index <= FR_PLUGIN_MAX_REQUIRES; index++) {
        char alias[32];
        char entry[256];
        snprintf(alias, sizeof alias, "dep%d", index);
        requirement_entry(entry, sizeof entry, alias, "https://x/leaf.lua", ZERO_DIGEST);
        written = snprintf(MANY_ALIASES_BODY + filled, sizeof MANY_ALIASES_BODY - filled, " %s,",
                           entry);
        if (written < 0 || (size_t) written >= sizeof MANY_ALIASES_BODY - filled) break;
        filled += (size_t) written;
    }
    snprintf(MANY_ALIASES_BODY + filled, sizeof MANY_ALIASES_BODY - filled, " } }\n");
    fr_sha256_hex(MANY_ALIASES_BODY, strlen(MANY_ALIASES_BODY), MANY_ALIASES_DIGEST);
    stub_serve("https://x/many.lua", MANY_ALIASES_BODY, strlen(MANY_ALIASES_BODY));
}

/* The refusal is the declaration reader's, not this module's: requires is a
   fixed FR_PLUGIN_MAX_REQUIRES array, so a seventeenth alias never reaches
   acquisition. What this pins is that acquiring a dependency propagates it. */
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

/* --- The manifest override --- */

static char OVERRIDE_V1_ARCHIVE[8192];
static size_t OVERRIDE_V1_ARCHIVE_LENGTH;
static char OVERRIDE_V1_DIGEST[65];
static char OVERRIDE_V2_ARCHIVE[8192];
static size_t OVERRIDE_V2_ARCHIVE_LENGTH;
static char OVERRIDE_V2_DIGEST[65];

static const char OVERRIDE_PLUGIN_CHUNK[] =
    "daukle.plugin{ api = 1, exports = { \"lib/coords\" } }\n";
static const char OVERRIDE_V1_MODULE[] = "return { tag = \"v1\" }\n";
static const char OVERRIDE_V2_MODULE[] = "return { tag = \"v2\" }\n";

/* Two archives at two urls, distinguished only by what lib/coords returns:
   the override test below asserts on THIS difference, the bytes the dependent
   actually receives, never on a flag or a log line. v1's url is deliberately
   also stubbed here (rather than left unserved) so a broken override that
   still reached the author's original url would be caught by a content
   mismatch, not masked by a fetch failure. */
static void serve_override_targets(void) {
    stub_reset();
    memset(OVERRIDE_V1_ARCHIVE, 0, sizeof OVERRIDE_V1_ARCHIVE);
    size_t offset = fr_test_tar_append(OVERRIDE_V1_ARCHIVE, 0, "plugin.lua", '0',
                                       OVERRIDE_PLUGIN_CHUNK, sizeof OVERRIDE_PLUGIN_CHUNK - 1);
    offset = fr_test_tar_append(OVERRIDE_V1_ARCHIVE, offset, "lib/coords.lua", '0',
                                OVERRIDE_V1_MODULE, sizeof OVERRIDE_V1_MODULE - 1);
    OVERRIDE_V1_ARCHIVE_LENGTH = fr_test_tar_end(OVERRIDE_V1_ARCHIVE, offset);
    fr_sha256_hex(OVERRIDE_V1_ARCHIVE, OVERRIDE_V1_ARCHIVE_LENGTH, OVERRIDE_V1_DIGEST);
    stub_serve("https://x/java-v1.lua", OVERRIDE_V1_ARCHIVE, OVERRIDE_V1_ARCHIVE_LENGTH);

    memset(OVERRIDE_V2_ARCHIVE, 0, sizeof OVERRIDE_V2_ARCHIVE);
    offset = fr_test_tar_append(OVERRIDE_V2_ARCHIVE, 0, "plugin.lua", '0', OVERRIDE_PLUGIN_CHUNK,
                                sizeof OVERRIDE_PLUGIN_CHUNK - 1);
    offset = fr_test_tar_append(OVERRIDE_V2_ARCHIVE, offset, "lib/coords.lua", '0',
                                OVERRIDE_V2_MODULE, sizeof OVERRIDE_V2_MODULE - 1);
    OVERRIDE_V2_ARCHIVE_LENGTH = fr_test_tar_end(OVERRIDE_V2_ARCHIVE, offset);
    fr_sha256_hex(OVERRIDE_V2_ARCHIVE, OVERRIDE_V2_ARCHIVE_LENGTH, OVERRIDE_V2_DIGEST);
    stub_serve("https://x/java-v2.lua", OVERRIDE_V2_ARCHIVE, OVERRIDE_V2_ARCHIVE_LENGTH);
}

TEST a_manifest_override_replaces_what_the_author_named(void) {
    static char acquire_message[512];
    static char member_message[512];
    char source[512];
    char overrides_json[256];
    serve_override_targets();
    dependent_source(source, sizeof source, "java", "https://x/java-v1.lua", OVERRIDE_V1_DIGEST);
    snprintf(overrides_json, sizeof overrides_json,
            "{\"java\":{\"url\":\"https://x/java-v2.lua\",\"sha256\":\"%s\"}}",
            OVERRIDE_V2_DIGEST);
    cJSON *overrides = cJSON_Parse(overrides_json);

    fr_plugin_deps *deps = NULL;
    int status = acquire_with_overrides(".", source, overrides, &deps, acquire_message,
                                        sizeof acquire_message);
    int requests = stub_requests;

    fr_error err;
    const char *text = NULL;
    size_t length = 0;
    fr_plugin_deps *owner = NULL;
    const char *owner_label = NULL;
    int member_status = FR_ERR;
    int served_v2 = 0;
    if (status == FR_OK) {
        member_status = fr_plugin_deps_member(deps, "java", "lib/coords", &text, &length, &owner,
                                              &owner_label, &err);
        if (member_status != FR_OK) {
            snprintf(member_message, sizeof member_message, "%s", err.message);
        } else {
            served_v2 = length == sizeof OVERRIDE_V2_MODULE - 1
                && memcmp(text, OVERRIDE_V2_MODULE, length) == 0;
        }
    }
    cJSON_Delete(overrides);
    fr_plugin_deps_close(deps);
    fr_lua_runtime_shutdown();

    ASSERT_EQm(acquire_message, FR_OK, status);
    ASSERT_EQm(member_message, FR_OK, member_status);
    ASSERTm("the override's own target was served, not the author's original url", served_v2);
    ASSERT_EQm("only the override's url is fetched, never the author's original one", 1, requests);
    PASS();
}

TEST an_override_naming_an_alias_the_dependent_does_not_declare_is_refused(void) {
    static char message[512];
    char source[512];
    serve_two_plugins();
    dependent_source(source, sizeof source, "java", "https://x/java.lua", JAVA_DIGEST);
    char overrides_json[256];
    snprintf(overrides_json, sizeof overrides_json,
            "{\"kotlin\":{\"url\":\"https://x/other.lua\",\"sha256\":\"%s\"}}", ZERO_DIGEST);
    cJSON *overrides = cJSON_Parse(overrides_json);

    fr_plugin_deps *deps = NULL;
    int status = acquire_with_overrides(".", source, overrides, &deps, message, sizeof message);
    cJSON_Delete(overrides);
    fr_plugin_deps_close(deps);
    fr_lua_runtime_shutdown();

    ASSERT_EQ(FR_ERR, status);
    ASSERTm(message, strstr(message, "does not declare") != NULL);
    ASSERTm(message, strstr(message, "kotlin") != NULL);
    ASSERTm(message, strstr(message, "java") != NULL);
    PASS();
}

TEST an_override_naming_its_own_requires_is_refused_as_reserved(void) {
    static char message[512];
    char source[512];
    serve_two_plugins();
    dependent_source(source, sizeof source, "java", "https://x/java.lua", JAVA_DIGEST);
    char overrides_json[512];
    snprintf(overrides_json, sizeof overrides_json,
            "{\"java\":{\"url\":\"https://x/java.lua\",\"sha256\":\"%s\","
            "\"requires\":{\"foo\":{\"url\":\"https://x/foo.lua\",\"sha256\":\"%s\"}}}}",
            JAVA_DIGEST, ZERO_DIGEST);
    cJSON *overrides = cJSON_Parse(overrides_json);

    fr_plugin_deps *deps = NULL;
    int status = acquire_with_overrides(".", source, overrides, &deps, message, sizeof message);
    cJSON_Delete(overrides);
    fr_plugin_deps_close(deps);
    fr_lua_runtime_shutdown();

    ASSERT_EQ(FR_ERR, status);
    ASSERTm(message, strstr(message, "reserved") != NULL);
    /* Names the dotted form it reserves, not merely the alias it is about: a
       refusal naming no form at all would still contain "java" (the alias)
       and would satisfy a looser assertion. */
    ASSERTm(message, strstr(message, "[plugins.gradle.requires.java.requires]") != NULL);
    PASS();
}

/* A malformed override value is refused inside fr_plugins_parse_entry, whose
   messages read "plugin \"%s\": ...": the label passed there must not be the
   bare alias, or the reader is sent looking for a [plugins.java] entry that
   does not exist. */
TEST a_malformed_override_value_is_refused_naming_the_override_not_the_alias(void) {
    static char message[512];
    char source[512];
    serve_two_plugins();
    dependent_source(source, sizeof source, "java", "https://x/java.lua", JAVA_DIGEST);
    cJSON *overrides = cJSON_Parse("{\"java\":{\"path\":\"./x\",\"url\":\"https://x/y\"}}");

    fr_plugin_deps *deps = NULL;
    int status = acquire_with_overrides(".", source, overrides, &deps, message, sizeof message);
    cJSON_Delete(overrides);
    fr_plugin_deps_close(deps);
    fr_lua_runtime_shutdown();

    ASSERT_EQ(FR_ERR, status);
    ASSERTm(message, strstr(message, "path, url, or resolver") != NULL);
    ASSERTm(message, strstr(message, "gradle's override for java") != NULL);
    PASS();
}

/* The other call site sharing plugins.c's "plugin \"%s\": ..." messages:
   fr_plugins_acquire_source's own pin check, reached once parsing succeeded. */
TEST an_override_whose_pin_does_not_match_is_refused_naming_the_override_not_the_alias(void) {
    static char message[512];
    char source[512];
    char overrides_json[256];
    serve_two_plugins();
    dependent_source(source, sizeof source, "java", "https://x/java.lua", JAVA_DIGEST);
    snprintf(overrides_json, sizeof overrides_json,
            "{\"java\":{\"url\":\"https://x/foo.lua\",\"sha256\":\"%s\"}}", ZERO_DIGEST);
    cJSON *overrides = cJSON_Parse(overrides_json);

    fr_plugin_deps *deps = NULL;
    int status = acquire_with_overrides(".", source, overrides, &deps, message, sizeof message);
    cJSON_Delete(overrides);
    fr_plugin_deps_close(deps);
    fr_lua_runtime_shutdown();

    ASSERT_EQ(FR_ERR, status);
    ASSERTm(message, strstr(message, "expected sha256") != NULL);
    ASSERTm(message, strstr(message, "gradle's override for java") != NULL);
    PASS();
}

static const char DIR_OVERRIDE_PLUGIN_TEXT[] =
    "daukle.plugin{ api = 1, exports = { \"lib/coords\" } }\n";
static const char DIR_OVERRIDE_COORDS_TEXT[] = "return { tag = \"directory\" }\n";

/* This is the primary local-development case the override exists for at all
   (a path, never reachable any other way), and it produces node state the
   ordinary url path never does: NULL text, zero length, an empty digest, and
   a directory source handed to read_node_declaration. A real temp directory
   is used rather than a fixture the sandbox already trusts, so the path
   actually travels through fr_lua_sandbox_resolve_dir and
   fr_plugin_source_open_directory, not around them. */
TEST a_path_override_reads_a_local_directory(void) {
    static char acquire_message[512];
    static char member_message[512];
    char scratch[512];
    char lib_dir[512];
    char plugin_file[512];
    char coords_file[512];
    char source[512];
    FILE *plugin_out;
    FILE *coords_out;

    /* Every setup step is gated on the one before it and asserted only after
       fr_test_remove_tree runs below, so a setup failure never leaves the
       scratch directory behind for the next run to trip over. */
    snprintf(scratch, sizeof scratch, "%s/daukle_test_plugin_deps_override_%d",
            fr_test_temp_base(), fr_test_process_id());
    fr_test_remove_tree(scratch);
    int scratch_ok = fr_test_make_directory(scratch) == 0;
    snprintf(lib_dir, sizeof lib_dir, "%s/lib", scratch);
    int lib_dir_ok = scratch_ok && fr_test_make_directory(lib_dir) == 0;

    int plugin_write_ok = 0;
    if (lib_dir_ok) {
        snprintf(plugin_file, sizeof plugin_file, "%s/plugin.lua", scratch);
        plugin_out = fopen(plugin_file, "wb");
        plugin_write_ok = plugin_out != NULL;
        if (plugin_write_ok) {
            fwrite(DIR_OVERRIDE_PLUGIN_TEXT, 1, sizeof DIR_OVERRIDE_PLUGIN_TEXT - 1, plugin_out);
            fclose(plugin_out);
        }
    }

    int coords_write_ok = 0;
    if (plugin_write_ok) {
        snprintf(coords_file, sizeof coords_file, "%s/lib/coords.lua", scratch);
        coords_out = fopen(coords_file, "wb");
        coords_write_ok = coords_out != NULL;
        if (coords_write_ok) {
            fwrite(DIR_OVERRIDE_COORDS_TEXT, 1, sizeof DIR_OVERRIDE_COORDS_TEXT - 1, coords_out);
            fclose(coords_out);
        }
    }

    cJSON *overrides = NULL;
    fr_plugin_deps *deps = NULL;
    int status = FR_ERR;
    int requests = 0;
    fr_error err;
    const char *text = NULL;
    size_t length = 0;
    fr_plugin_deps *owner = NULL;
    const char *owner_label = NULL;
    int member_status = FR_ERR;
    int served_directory = 0;

    if (coords_write_ok) {
        serve_two_plugins();
        dependent_source(source, sizeof source, "java", "https://x/java.lua", JAVA_DIGEST);
        overrides = cJSON_Parse("{\"java\":{\"path\":\".\"}}");

        status = acquire_with_overrides(scratch, source, overrides, &deps, acquire_message,
                                        sizeof acquire_message);
        requests = stub_requests;

        if (status == FR_OK) {
            member_status = fr_plugin_deps_member(deps, "java", "lib/coords", &text, &length, &owner,
                                                  &owner_label, &err);
            if (member_status == FR_OK) {
                served_directory = length == sizeof DIR_OVERRIDE_COORDS_TEXT - 1
                    && memcmp(text, DIR_OVERRIDE_COORDS_TEXT, length) == 0;
            } else {
                snprintf(member_message, sizeof member_message, "%s", err.message);
            }
        }
        cJSON_Delete(overrides);
        fr_plugin_deps_close(deps);
        fr_lua_runtime_shutdown();
    }
    fr_test_remove_tree(scratch);

    ASSERTm("could not create the scratch directory", scratch_ok);
    ASSERTm("could not create the scratch lib directory", lib_dir_ok);
    ASSERTm("could not write the fixture plugin.lua", plugin_write_ok);
    ASSERTm("could not write the fixture lib/coords.lua", coords_write_ok);
    ASSERT_EQm(acquire_message, FR_OK, status);
    ASSERT_EQm(member_message, FR_OK, member_status);
    ASSERTm("the directory override's own lib/coords.lua was served", served_directory);
    ASSERT_EQm("the path override replaces java-v1 entirely: neither stubbed url is ever"
              " fetched", 0, requests);
    PASS();
}

static char SCOPE_MID_ARCHIVE[8192];
static size_t SCOPE_MID_ARCHIVE_LENGTH;
static char SCOPE_MID_DIGEST[65];
static char SCOPE_MID_JAVA_ARCHIVE[8192];
static size_t SCOPE_MID_JAVA_ARCHIVE_LENGTH;
static char SCOPE_MID_JAVA_DIGEST[65];

static const char SCOPE_MARKER_MODULE[] = "return { marker = true }\n";
static const char SCOPE_MID_JAVA_MODULE[] = "return { tag = \"mid-java\" }\n";

/* mid requires its OWN "java", unrelated to the root's: an override on the
   root names one of the ROOT's own aliases, and this fixture exists to prove
   it never reaches an alias merely spelled the same two levels down. Adds to
   whatever the caller already stubbed rather than resetting: the one test
   using this calls serve_override_targets first, for the root's own java-v1
   and java-v2. */
static void serve_scoping_targets(void) {
    memset(SCOPE_MID_JAVA_ARCHIVE, 0, sizeof SCOPE_MID_JAVA_ARCHIVE);
    size_t offset = fr_test_tar_append(SCOPE_MID_JAVA_ARCHIVE, 0, "plugin.lua", '0',
                                       OVERRIDE_PLUGIN_CHUNK, sizeof OVERRIDE_PLUGIN_CHUNK - 1);
    offset = fr_test_tar_append(SCOPE_MID_JAVA_ARCHIVE, offset, "lib/coords.lua", '0',
                                SCOPE_MID_JAVA_MODULE, sizeof SCOPE_MID_JAVA_MODULE - 1);
    SCOPE_MID_JAVA_ARCHIVE_LENGTH = fr_test_tar_end(SCOPE_MID_JAVA_ARCHIVE, offset);
    fr_sha256_hex(SCOPE_MID_JAVA_ARCHIVE, SCOPE_MID_JAVA_ARCHIVE_LENGTH, SCOPE_MID_JAVA_DIGEST);
    stub_serve("https://x/mid-java.lua", SCOPE_MID_JAVA_ARCHIVE, SCOPE_MID_JAVA_ARCHIVE_LENGTH);

    char mid_requirement[256];
    char mid_chunk[512];
    requirement_entry(mid_requirement, sizeof mid_requirement, "java", "https://x/mid-java.lua",
                      SCOPE_MID_JAVA_DIGEST);
    snprintf(mid_chunk, sizeof mid_chunk,
            "daukle.plugin{ api = 1, exports = { \"lib/marker\" }, requires = { %s } }\n",
            mid_requirement);

    memset(SCOPE_MID_ARCHIVE, 0, sizeof SCOPE_MID_ARCHIVE);
    offset = fr_test_tar_append(SCOPE_MID_ARCHIVE, 0, "plugin.lua", '0', mid_chunk,
                                strlen(mid_chunk));
    offset = fr_test_tar_append(SCOPE_MID_ARCHIVE, offset, "lib/marker.lua", '0',
                                SCOPE_MARKER_MODULE, sizeof SCOPE_MARKER_MODULE - 1);
    SCOPE_MID_ARCHIVE_LENGTH = fr_test_tar_end(SCOPE_MID_ARCHIVE, offset);
    fr_sha256_hex(SCOPE_MID_ARCHIVE, SCOPE_MID_ARCHIVE_LENGTH, SCOPE_MID_DIGEST);
    stub_serve("https://x/mid.lua", SCOPE_MID_ARCHIVE, SCOPE_MID_ARCHIVE_LENGTH);

    /* The root's own "java" is deliberately overridden to java-v2 (served by
       serve_override_targets's fixture, called alongside this one) and its
       own declared url, java-v1, is never meant to be reached either. */
}

TEST an_override_does_not_reach_a_transitively_required_artifacts_own_alias(void) {
    static char acquire_message[512];
    static char root_message[512];
    static char mid_message[512];
    char source[512];
    char root_java_req[256];
    char root_mid_req[256];
    char overrides_json[256];

    serve_override_targets();
    serve_scoping_targets();
    requirement_entry(root_java_req, sizeof root_java_req, "java", "https://x/java-v1.lua",
                      OVERRIDE_V1_DIGEST);
    requirement_entry(root_mid_req, sizeof root_mid_req, "mid", "https://x/mid.lua",
                      SCOPE_MID_DIGEST);
    snprintf(source, sizeof source, "daukle.plugin{ api = 1, requires = { %s, %s } }\n",
            root_java_req, root_mid_req);
    snprintf(overrides_json, sizeof overrides_json,
            "{\"java\":{\"url\":\"https://x/java-v2.lua\",\"sha256\":\"%s\"}}",
            OVERRIDE_V2_DIGEST);
    cJSON *overrides = cJSON_Parse(overrides_json);

    fr_plugin_deps *deps = NULL;
    int status = acquire_with_overrides(".", source, overrides, &deps, acquire_message,
                                        sizeof acquire_message);

    fr_error err;
    const char *root_text = NULL;
    size_t root_length = 0;
    fr_plugin_deps *root_owner = NULL;
    const char *root_owner_label = NULL;
    const char *mid_text = NULL;
    size_t mid_length = 0;
    fr_plugin_deps *mid_view = NULL;
    const char *mid_owner_label = NULL;
    fr_plugin_deps *java_owner = NULL;
    const char *java_owner_label = NULL;
    int root_status = FR_ERR;
    int mid_lookup_status = FR_ERR;
    int mid_java_status = FR_ERR;
    int root_saw_v2 = 0;
    int mid_saw_its_own_java = 0;
    if (status == FR_OK) {
        root_status = fr_plugin_deps_member(deps, "java", "lib/coords", &root_text, &root_length,
                                            &root_owner, &root_owner_label, &err);
        if (root_status == FR_OK) {
            root_saw_v2 = root_length == sizeof OVERRIDE_V2_MODULE - 1
                && memcmp(root_text, OVERRIDE_V2_MODULE, root_length) == 0;
        } else {
            snprintf(root_message, sizeof root_message, "%s", err.message);
        }

        mid_lookup_status = fr_plugin_deps_member(deps, "mid", "lib/marker", &mid_text,
                                                  &mid_length, &mid_view, &mid_owner_label, &err);
        if (mid_lookup_status == FR_OK) {
            mid_java_status = fr_plugin_deps_member(mid_view, "java", "lib/coords", &mid_text,
                                                    &mid_length, &java_owner, &java_owner_label,
                                                    &err);
            if (mid_java_status == FR_OK) {
                mid_saw_its_own_java = mid_length == sizeof SCOPE_MID_JAVA_MODULE - 1
                    && memcmp(mid_text, SCOPE_MID_JAVA_MODULE, mid_length) == 0;
            } else {
                snprintf(mid_message, sizeof mid_message, "%s", err.message);
            }
        }
    }
    cJSON_Delete(overrides);
    fr_plugin_deps_close(deps);
    fr_lua_runtime_shutdown();

    ASSERT_EQm(acquire_message, FR_OK, status);
    ASSERT_EQm(root_message, FR_OK, root_status);
    ASSERTm("the root's own java alias is overridden", root_saw_v2);
    ASSERT_EQ(FR_OK, mid_lookup_status);
    ASSERT_EQm(mid_message, FR_OK, mid_java_status);
    ASSERTm("mid's own java alias resolves to what mid itself required, untouched by the"
           " root's override", mid_saw_its_own_java);
    PASS();
}

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(a_declared_dependency_is_acquired);
    RUN_TEST(a_dependency_whose_digest_does_not_match_is_refused);
    RUN_TEST(the_same_artifact_under_two_aliases_is_fetched_once);
    RUN_TEST(an_exported_member_is_served_and_an_unexported_one_is_not);
    RUN_TEST(an_unknown_alias_is_refused_naming_the_aliases_that_exist);
    RUN_TEST(one_url_pinned_to_two_digests_is_refused_naming_both);
    RUN_TEST(a_dependencys_entry_chunk_is_never_run);
    RUN_TEST(a_cycle_between_two_artifacts_is_refused);
    RUN_TEST(a_graph_at_the_depth_limit_is_acquired);
    RUN_TEST(a_graph_deeper_than_the_limit_is_refused);
    RUN_TEST(a_graph_wider_than_the_node_limit_is_refused);
    RUN_TEST(a_dependency_naming_more_aliases_than_the_limit_is_refused);
    RUN_TEST(a_manifest_override_replaces_what_the_author_named);
    RUN_TEST(an_override_naming_an_alias_the_dependent_does_not_declare_is_refused);
    RUN_TEST(an_override_naming_its_own_requires_is_refused_as_reserved);
    RUN_TEST(a_malformed_override_value_is_refused_naming_the_override_not_the_alias);
    RUN_TEST(an_override_whose_pin_does_not_match_is_refused_naming_the_override_not_the_alias);
    RUN_TEST(a_path_override_reads_a_local_directory);
    RUN_TEST(an_override_does_not_reach_a_transitively_required_artifacts_own_alias);
    GREATEST_MAIN_END();
}
