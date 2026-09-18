# TOML and Lua Configuration Surface Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give daukle a TOML manifest and an optional Lua logic layer, both feeding the JSON validator that already exists, so users author configuration in a readable format and write plugins without a C compiler.

**Architecture:** A third plugin table, `fr_config_plugin`, joins the source and language tables in `registry.c`. Every format (JSON, TOML, Lua) produces a cJSON document and hands it to one validator in `manifest.c`. Formats are either primaries (one per project) or overlays (Lua, which runs after a primary and may also register source and language plugins into the same registry the C plugins use).

**Tech Stack:** C11, CMake, vendored cJSON, vendored `tomlc99`, vendored Lua 5.4.7, `greatest` test harness.

**Spec:** `docs/superpowers/specs/2026-09-18-toml-lua-config-surface-design.md`

## Global Constraints

- C11, `/W4 /WX` on MSVC and `-Wall -Wextra -Werror` elsewhere. Code in `src/` must compile clean under both. Vendored code goes in `vendor/` and joins the `daukle_vendor` target, which deliberately carries no warning flags.
- Every symbol is prefixed `fr_`. Do not rename existing symbols; the `fr_` prefix predates the `daukle` name and renaming it is not this plan's job.
- Capability strings are namespaced: `daukle.source/<kind>`, `daukle.language/<name>`, and now `daukle.config/<format>`.
- `FR_OK` is 0 and `FR_ERR` is 1. Errors are reported by `fr_error_set(err, fmt, ...)` into a 512 byte buffer. The CLI layer decides what to print; library code never prints.
- `STRUCTURE.md` is a living document: a task that adds a module adds its row to the ownership index in section 2 **in the same commit**.
- Commits follow Conventional Commits: `type(scope): summary`, imperative, lowercase, no trailing period, no body unless the change is large.
- Never write an en dash or em dash anywhere, including code comments and commit messages.
- Comments: default to zero. A comment may only carry non-obvious *why*. The existing files show the house style; match it.
- Work happens on the `development` branch.

**Build and test commands** (MSVC multi-config, which is what `build/` is configured as):

```bash
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
ctest --test-dir build -C Release -R test_config --output-on-failure
```

Tests run with their working directory set to the repo root, so fixture paths in test code are written relative to the repo root (`test/fixtures/...`).

---

## File Structure

**New source modules:**

| file | responsibility |
| --- | --- |
| `src/config.c` / `.h` | find the manifest, dispatch to a format plugin, apply overlays. Knows no format's name. |
| `src/config_json.c` / `.h` | the JSON primary format plugin. Wraps what `manifest.c` does today. |
| `src/config_toml.c` / `.h` | the TOML primary format plugin. Walks a vendored TOML tree into cJSON. |
| `src/config_lua.c` / `.h` | the Lua overlay plugin: runs the script, reads `daukle.config` back, registers declared plugins. |
| `src/luax.c` / `.h` | the thin Lua helper, as `jsonx.c` is for cJSON. Owns cJSON to Lua and Lua to cJSON. |
| `src/lua_sandbox.c` / `.h` | builds the restricted global table, the instruction hook, the capped allocator, `daukle.include`. |
| `src/tomledit.c` / `.h` | splice edits into TOML text so comments and ordering survive. Sibling of `jsonedit.c`. |

**Modified:** `src/registry.c` / `.h` (third table), `src/manifest.c` / `.h` (expose the validator), `src/sync.c` (registry before manifest), `src/cli.c` / `.h` and `src/main.c` (new commands and flags), `CMakeLists.txt`, `cmake/check_agnostic.cmake`, `STRUCTURE.md`.

**Vendored:** `vendor/toml/` (tomlc99), `vendor/lua/` (Lua 5.4.7 library sources).

**Phases.** Tasks 1 to 5 deliver TOML and are useful shipped alone. Tasks 6 to 11 deliver Lua. Tasks 12 to 14 deliver editing and the CLI. Each phase boundary is a sane place to stop.

---

### Task 1: Expose the validator behind a document

**Files:**
- Modify: `src/manifest.h`, `src/manifest.c:309-320`
- Test: `test/test_manifest.c`

**Interfaces:**
- Produces: `int fr_manifest_from_document(struct cJSON *root, const char *origin, fr_manifest *out, fr_error *err)`. Takes ownership of `root` in every outcome: on success it becomes `out->document`, on failure it is deleted. Every later task reaches the validator through this function.

- [ ] **Step 1: Write the failing test**

Add to `test/test_manifest.c`, above `GREATEST_MAIN_DEFS()`:

```c
TEST validates_a_document_built_in_memory(void) {
    cJSON *root = cJSON_Parse(
        "{\"schema\":1,\"project\":\"forebay/x\",\"version\":\"1.0.0\","
        "\"modules\":{},\"sources\":{},\"consumers\":[]}");
    ASSERT(root != NULL);
    fr_manifest manifest; fr_error err;
    ASSERT_EQ(FR_OK, fr_manifest_from_document(root, "<memory>", &manifest, &err));
    ASSERT_STR_EQ("forebay/x", manifest.self.project);
    fr_manifest_free(&manifest);
    PASS();
}

TEST reports_the_origin_of_a_bad_document(void) {
    cJSON *root = cJSON_Parse("{\"schema\":99}");
    ASSERT(root != NULL);
    fr_manifest manifest; fr_error err;
    ASSERT_EQ(FR_ERR, fr_manifest_from_document(root, "<memory>", &manifest, &err));
    ASSERT(strstr(err.message, "<memory>") != NULL);
    PASS();
}
```

Register both in `main`:

```c
    RUN_TEST(validates_a_document_built_in_memory);
    RUN_TEST(reports_the_origin_of_a_bad_document);
```

- [ ] **Step 2: Run it and watch it fail**

Run: `cmake --build build --config Release`
Expected: FAIL to compile, `fr_manifest_from_document` undefined.

- [ ] **Step 3: Declare it**

In `src/manifest.h`, above `fr_manifest_read`:

```c
int fr_manifest_from_document(struct cJSON *root, const char *origin, fr_manifest *out, fr_error *err);
```

- [ ] **Step 4: Implement it by splitting `fr_manifest_read`**

Replace `fr_manifest_read` in `src/manifest.c` with:

```c
int fr_manifest_from_document(cJSON *root, const char *origin, fr_manifest *out, fr_error *err) {
    memset(out, 0, sizeof *out);
    if (manifest_from_json(root, origin, out, err) != FR_OK) {
        fr_manifest_free(out);
        cJSON_Delete(root);
        return FR_ERR;
    }
    out->document = root;
    return FR_OK;
}

int fr_manifest_read(const char *file_path, fr_manifest *out, fr_error *err) {
    memset(out, 0, sizeof *out);
    cJSON *root = NULL;
    if (fr_json_read_file(file_path, &root, err) != FR_OK) return FR_ERR;
    return fr_manifest_from_document(root, file_path, out, err);
}
```

- [ ] **Step 5: Run the tests**

Run: `ctest --test-dir build -C Release -R test_manifest --output-on-failure`
Expected: PASS, including every test that was already there.

- [ ] **Step 6: Commit**

```bash
git add src/manifest.c src/manifest.h test/test_manifest.c
git commit -m "refactor(manifest): validate a document the caller already parsed"
```

---

### Task 2: The config plugin table

**Files:**
- Modify: `src/registry.h`, `src/registry.c`
- Create: `test/test_registry.c`

**Interfaces:**
- Produces:
  - `fr_config_plugin` as defined below
  - `int fr_registry_add_config(fr_registry *registry, const fr_config_plugin *plugin, fr_error *err)`
  - `const fr_config_plugin *fr_registry_config(const fr_registry *registry, const char *capability)`
  - `size_t fr_registry_config_count(const fr_registry *registry)`
  - `const fr_config_plugin *fr_registry_config_at(const fr_registry *registry, size_t index)`

The last two exist so `config.c` can search a directory by asking every registered format for its file name, instead of holding a list of formats itself.

- [ ] **Step 1: Write the failing test**

Create `test/test_registry.c`:

```c
#include "greatest.h"
#include "registry.h"

static int never_loads(void *state, const char *text, const char *origin, const char *base_dir,
                       fr_registry *registry, const struct cJSON *document,
                       struct cJSON **out, fr_error *err) {
    (void) state; (void) text; (void) origin; (void) base_dir;
    (void) registry; (void) document; (void) out; (void) err;
    return FR_ERR;
}

static const fr_config_plugin PRIMARY = { "daukle.config/aaa", "daukle.aaa", 0, never_loads, NULL };
static const fr_config_plugin OVERLAY = { "daukle.config/bbb", "daukle.bbb", 1, never_loads, NULL };

TEST registers_and_finds_a_config_plugin(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_registry_add_config(registry, &PRIMARY, &err));
    const fr_config_plugin *found = fr_registry_config(registry, "daukle.config/aaa");
    ASSERT(found != NULL);
    ASSERT_STR_EQ("daukle.aaa", found->file_name);
    ASSERT(fr_registry_config(registry, "daukle.config/zzz") == NULL);
    fr_registry_destroy(registry);
    PASS();
}

TEST rejects_a_duplicate_config_capability(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_registry_add_config(registry, &PRIMARY, &err));
    ASSERT_EQ(FR_ERR, fr_registry_add_config(registry, &PRIMARY, &err));
    fr_registry_destroy(registry);
    PASS();
}

TEST walks_every_registered_config_plugin(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    fr_registry_add_config(registry, &PRIMARY, &err);
    fr_registry_add_config(registry, &OVERLAY, &err);
    ASSERT_EQ(2, (int) fr_registry_config_count(registry));
    ASSERT_EQ(0, fr_registry_config_at(registry, 0)->overlay);
    ASSERT_EQ(1, fr_registry_config_at(registry, 1)->overlay);
    fr_registry_destroy(registry);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(registers_and_finds_a_config_plugin);
    RUN_TEST(rejects_a_duplicate_config_capability);
    RUN_TEST(walks_every_registered_config_plugin);
    GREATEST_MAIN_END();
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `cmake --build build --config Release`
Expected: FAIL to compile, `fr_config_plugin` undefined. The test executable is picked up automatically by the `file(GLOB ... test/test_*.c)` in `CMakeLists.txt`; if CMake does not notice the new file, re-run `cmake -S . -B build`.

- [ ] **Step 3: Declare the table**

In `src/registry.h`, after `fr_language_plugin`:

```c
typedef struct {
    const char *capability;
    const char *file_name;
    int overlay;
    int (*load)(void *state, const char *text, const char *origin, const char *base_dir,
                fr_registry *registry, const struct cJSON *document,
                struct cJSON **out, fr_error *err);
    void *state;
} fr_config_plugin;
```

`fr_registry` is already forward declared in this header, but the typedef currently appears below the plugin structs. Move `typedef struct fr_registry fr_registry;` above them so the `load` pointer can name it. Add `struct cJSON;` beside it if the header does not already forward declare cJSON (`types.h` does; including it is enough).

Then the four functions:

```c
int fr_registry_add_config(fr_registry *registry, const fr_config_plugin *plugin, fr_error *err);
const fr_config_plugin *fr_registry_config(const fr_registry *registry, const char *capability);
size_t fr_registry_config_count(const fr_registry *registry);
const fr_config_plugin *fr_registry_config_at(const fr_registry *registry, size_t index);
```

- [ ] **Step 4: Implement, mirroring the two tables already there**

In `src/registry.c`, add to `struct fr_registry`:

```c
    fr_config_plugin configs[FR_REGISTRY_CAPACITY];
    size_t config_count;
```

and:

```c
int fr_registry_add_config(fr_registry *registry, const fr_config_plugin *plugin, fr_error *err) {
    for (size_t index = 0; index < registry->config_count; index++) {
        if (strcmp(registry->configs[index].capability, plugin->capability) == 0) {
            fr_error_set(err, "capability \"%s\" is already registered", plugin->capability);
            return FR_ERR;
        }
    }
    if (registry->config_count == FR_REGISTRY_CAPACITY) {
        fr_error_set(err, "cannot register \"%s\": registry is full", plugin->capability);
        return FR_ERR;
    }
    registry->configs[registry->config_count++] = *plugin;
    return FR_OK;
}

const fr_config_plugin *fr_registry_config(const fr_registry *registry, const char *capability) {
    for (size_t index = 0; index < registry->config_count; index++) {
        if (strcmp(registry->configs[index].capability, capability) == 0) return &registry->configs[index];
    }
    return NULL;
}

size_t fr_registry_config_count(const fr_registry *registry) {
    return registry->config_count;
}

const fr_config_plugin *fr_registry_config_at(const fr_registry *registry, size_t index) {
    if (index >= registry->config_count) return NULL;
    return &registry->configs[index];
}
```

- [ ] **Step 5: Run the tests**

Run: `ctest --test-dir build -C Release -R test_registry --output-on-failure`
Expected: PASS, 3 tests.

- [ ] **Step 6: Update STRUCTURE.md**

The `registry.c` row in section 2 currently reads "the two plugin tables, source and language". Change it to "the three plugin tables, source, language and config" and change the "it is NOT" column to name `fr_source_plugin`, `fr_language_plugin` and `fr_config_plugin`.

- [ ] **Step 7: Commit**

```bash
git add src/registry.c src/registry.h test/test_registry.c STRUCTURE.md
git commit -m "feat(registry): add the config format plugin table"
```

---

### Task 3: The format seam and the JSON plugin

**Files:**
- Create: `src/config.c`, `src/config.h`, `src/config_json.c`, `src/config_json.h`, `test/test_config.c`
- Create fixtures: `test/fixtures/search/json-only/daukle.json`, `test/fixtures/search/both/daukle.json`, `test/fixtures/search/both/daukle.toml`, `test/fixtures/search/empty/.keep`
- Modify: `src/sync.c:69-95,164-184`

**Interfaces:**
- Consumes: `fr_manifest_from_document` (Task 1), the config table (Task 2).
- Produces:
  - `int fr_config_load_file(const char *file_path, fr_registry *registry, fr_manifest *out, fr_error *err)`
  - `int fr_config_find(const char *directory, const fr_registry *registry, char **out_path, fr_error *err)`, `*out_path` is `malloc`ed and owned by the caller
  - `extern const fr_config_plugin FR_CONFIG_JSON;`
  - `int fr_build_registry(fr_registry **out, fr_error *err)` in `sync.h`, renamed from the static `build_registry` so tests can build the same registry the tool uses

- [ ] **Step 1: Write the fixtures**

`test/fixtures/search/json-only/daukle.json`:

```json
{
  "schema": 1,
  "project": "forebay/search",
  "version": "1.0.0",
  "modules": {},
  "sources": {},
  "consumers": []
}
```

`test/fixtures/search/both/daukle.json`: the same content. `test/fixtures/search/both/daukle.toml`: an empty file for now. `test/fixtures/search/empty/.keep`: an empty file, so git keeps the directory.

- [ ] **Step 2: Write the failing test**

Create `test/test_config.c`:

```c
#include "greatest.h"
#include "config.h"
#include "registry.h"
#include "sync.h"

#include <stdlib.h>
#include <string.h>

TEST loads_a_json_manifest_through_the_seam(void) {
    fr_error err;
    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));
    fr_manifest manifest;
    ASSERT_EQ(FR_OK, fr_config_load_file("test/fixtures/consumer/daukle.json", registry, &manifest, &err));
    ASSERT_STR_EQ("forebay/stub-translator", manifest.self.project);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    PASS();
}

TEST rejects_a_file_no_format_claims(void) {
    fr_error err;
    fr_registry *registry = NULL;
    fr_build_registry(&registry, &err);
    fr_manifest manifest;
    ASSERT_EQ(FR_ERR, fr_config_load_file("test/fixtures/consumer/daukle.xyz", registry, &manifest, &err));
    ASSERT(strstr(err.message, "xyz") != NULL);
    fr_registry_destroy(registry);
    PASS();
}

TEST finds_the_only_manifest_in_a_directory(void) {
    fr_error err;
    fr_registry *registry = NULL;
    fr_build_registry(&registry, &err);
    char *found = NULL;
    ASSERT_EQ(FR_OK, fr_config_find("test/fixtures/search/json-only", registry, &found, &err));
    ASSERT(strstr(found, "daukle.json") != NULL);
    free(found);
    fr_registry_destroy(registry);
    PASS();
}

TEST refuses_two_manifests_in_one_directory(void) {
    fr_error err;
    fr_registry *registry = NULL;
    fr_build_registry(&registry, &err);
    char *found = NULL;
    ASSERT_EQ(FR_ERR, fr_config_find("test/fixtures/search/both", registry, &found, &err));
    ASSERT(strstr(err.message, "daukle.json") != NULL);
    fr_registry_destroy(registry);
    PASS();
}

TEST reports_a_directory_with_no_manifest(void) {
    fr_error err;
    fr_registry *registry = NULL;
    fr_build_registry(&registry, &err);
    char *found = NULL;
    ASSERT_EQ(FR_ERR, fr_config_find("test/fixtures/search/empty", registry, &found, &err));
    ASSERT(strstr(err.message, "daukle.json") != NULL);
    fr_registry_destroy(registry);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(loads_a_json_manifest_through_the_seam);
    RUN_TEST(rejects_a_file_no_format_claims);
    RUN_TEST(finds_the_only_manifest_in_a_directory);
    RUN_TEST(refuses_two_manifests_in_one_directory);
    RUN_TEST(reports_a_directory_with_no_manifest);
    GREATEST_MAIN_END();
}
```

Note: `refuses_two_manifests_in_one_directory` passes from this task onward only because `daukle.toml` is a registered file name from Task 4. Until then the TOML plugin is not registered, the directory looks like it holds one manifest, and the test fails. Write the test now, mark it with `SKIPm("toml plugin lands in task 4")` as its first line, and delete that line in Task 4 Step 8.

- [ ] **Step 3: Run it and watch it fail**

Run: `cmake --build build --config Release`
Expected: FAIL to compile, `config.h` not found.

- [ ] **Step 4: Write the JSON format plugin**

`src/config_json.h`:

```c
#ifndef DAUKLE_CONFIG_JSON_H
#define DAUKLE_CONFIG_JSON_H

#include "registry.h"

extern const fr_config_plugin FR_CONFIG_JSON;

#endif
```

`src/config_json.c`:

```c
#include "config_json.h"

#include "error.h"
#include "jsonx.h"

static int config_json_load(void *state, const char *text, const char *origin, const char *base_dir,
                            fr_registry *registry, const cJSON *document,
                            cJSON **out, fr_error *err) {
    (void) state; (void) base_dir; (void) registry; (void) document;
    cJSON *root = cJSON_Parse(text);
    if (root == NULL) {
        fr_error_set(err, "\"%s\" is not valid json", origin);
        return FR_ERR;
    }
    *out = root;
    return FR_OK;
}

const fr_config_plugin FR_CONFIG_JSON = { "daukle.config/json", "daukle.json", 0, config_json_load, NULL };
```

- [ ] **Step 5: Write the seam**

`src/config.h`:

```c
#ifndef DAUKLE_CONFIG_H
#define DAUKLE_CONFIG_H

#include "types.h"
#include "registry.h"

/* Dispatches on the file's extension alone, so the set of formats is whatever
   is registered and this module never names one. */
int fr_config_load_file(const char *file_path, fr_registry *registry, fr_manifest *out, fr_error *err);
int fr_config_find(const char *directory, const fr_registry *registry, char **out_path, fr_error *err);

#endif
```

`src/config.c`:

```c
#include "config.h"

#include "error.h"
#include "manifest.h"
#include "region.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *extension_of(const char *file_path) {
    const char *dot = strrchr(file_path, '.');
    return dot == NULL ? "" : dot + 1;
}

static char *directory_of(const char *file_path) {
    const char *slash = strrchr(file_path, '/');
    const char *backslash = strrchr(file_path, '\\');
    const char *last = slash > backslash ? slash : backslash;
    if (last == NULL) {
        char *here = malloc(2);
        if (here != NULL) memcpy(here, ".", 2);
        return here;
    }
    size_t length = (size_t) (last - file_path);
    char *directory = malloc(length + 1);
    if (directory != NULL) {
        memcpy(directory, file_path, length);
        directory[length] = '\0';
    }
    return directory;
}

static char *join(const char *directory, const char *name) {
    size_t length = strlen(directory) + 1 + strlen(name) + 1;
    char *path = malloc(length);
    if (path != NULL) snprintf(path, length, "%s/%s", directory, name);
    return path;
}

static int file_exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) return 0;
    fclose(file);
    return 1;
}

static const fr_config_plugin *plugin_for(const fr_registry *registry, const char *file_path,
                                          fr_error *err) {
    char capability[128];
    snprintf(capability, sizeof capability, "daukle.config/%s", extension_of(file_path));
    const fr_config_plugin *plugin = fr_registry_config(registry, capability);
    if (plugin == NULL) {
        fr_error_set(err, "\"%s\": no config format registered for \"%s\"",
                     file_path, extension_of(file_path));
    }
    return plugin;
}

static int apply_overlays(const char *directory, fr_registry *registry,
                          cJSON *document, cJSON **out, fr_error *err) {
    cJSON *current = document;
    for (size_t index = 0; index < fr_registry_config_count(registry); index++) {
        const fr_config_plugin *plugin = fr_registry_config_at(registry, index);
        if (!plugin->overlay) continue;

        char *path = join(directory, plugin->file_name);
        if (path == NULL) {
            fr_error_set(err, "out of memory joining \"%s\"", plugin->file_name);
            cJSON_Delete(current);
            return FR_ERR;
        }
        if (!file_exists(path)) {
            free(path);
            continue;
        }

        char *text = NULL;
        if (fr_file_read_text(path, &text, err) != FR_OK) {
            free(path);
            cJSON_Delete(current);
            return FR_ERR;
        }
        cJSON *next = NULL;
        int status = plugin->load(plugin->state, text, path, directory, registry, current, &next, err);
        free(text);
        free(path);
        cJSON_Delete(current);
        if (status != FR_OK) return FR_ERR;
        current = next;
    }
    *out = current;
    return FR_OK;
}

int fr_config_load_file(const char *file_path, fr_registry *registry, fr_manifest *out, fr_error *err) {
    memset(out, 0, sizeof *out);

    const fr_config_plugin *plugin = plugin_for(registry, file_path, err);
    if (plugin == NULL) return FR_ERR;

    char *directory = directory_of(file_path);
    if (directory == NULL) {
        fr_error_set(err, "out of memory deriving the directory of \"%s\"", file_path);
        return FR_ERR;
    }

    char *text = NULL;
    if (fr_file_read_text(file_path, &text, err) != FR_OK) {
        free(directory);
        return FR_ERR;
    }

    cJSON *document = NULL;
    int status = plugin->load(plugin->state, text, file_path, directory, registry, NULL, &document, err);
    free(text);
    if (status != FR_OK) {
        free(directory);
        return FR_ERR;
    }

    cJSON *final = NULL;
    if (plugin->overlay) {
        final = document;
    } else if (apply_overlays(directory, registry, document, &final, err) != FR_OK) {
        free(directory);
        return FR_ERR;
    }
    free(directory);
    return fr_manifest_from_document(final, file_path, out, err);
}

int fr_config_find(const char *directory, const fr_registry *registry, char **out_path, fr_error *err) {
    *out_path = NULL;
    char *primary = NULL;
    char *overlay = NULL;

    for (size_t index = 0; index < fr_registry_config_count(registry); index++) {
        const fr_config_plugin *plugin = fr_registry_config_at(registry, index);
        char *path = join(directory, plugin->file_name);
        if (path == NULL) {
            fr_error_set(err, "out of memory joining \"%s\"", plugin->file_name);
            free(primary);
            free(overlay);
            return FR_ERR;
        }
        if (!file_exists(path)) {
            free(path);
            continue;
        }
        if (plugin->overlay) {
            free(overlay);
            overlay = path;
        } else if (primary != NULL) {
            fr_error_set(err, "\"%s\" holds both \"%s\" and \"%s\": keep one",
                         directory, primary, path);
            free(primary);
            free(overlay);
            free(path);
            return FR_ERR;
        } else {
            primary = path;
        }
    }

    if (primary != NULL) {
        free(overlay);
        *out_path = primary;
        return FR_OK;
    }
    if (overlay != NULL) {
        *out_path = overlay;
        return FR_OK;
    }
    fr_error_set(err, "\"%s\" holds no manifest: expected daukle.json", directory);
    return FR_ERR;
}
```

The "expected daukle.json" wording is a deliberate compromise and the one place core mentions a file name: it is the only registered primary at this point in the plan. Task 4 Step 8 replaces the literal with a list built from the registered plugins.

- [ ] **Step 6: Rewire `sync.c` so the registry exists before the manifest is read**

A Lua config registers plugins while it is being read, so the registry must be built first. In `src/sync.c`, rename `static int build_registry` to `int fr_build_registry`, declare it in `src/sync.h`:

```c
int fr_build_registry(fr_registry **out, fr_error *err);
```

register the JSON format inside it, beside the sources and languages:

```c
    if (fr_registry_add_config(registry, &FR_CONFIG_JSON, err) != FR_OK) {
        fr_registry_destroy(registry);
        return FR_ERR;
    }
```

and reorder the opening of `fr_sync` so the registry comes first and the manifest arrives through the seam:

```c
int fr_sync(const char *manifest_path, int write, int use_cache, fr_sync_report *report, fr_error *err) {
    memset(report, 0, sizeof *report);
    fr_cache_set_enabled(use_cache);

    fr_registry *registry = NULL;
    if (fr_build_registry(&registry, err) != FR_OK) return FR_ERR;

    fr_manifest manifest;
    if (fr_config_load_file(manifest_path, registry, &manifest, err) != FR_OK) {
        fr_registry_destroy(registry);
        return FR_ERR;
    }

    char *manifest_dir = manifest_directory(manifest_path);
    if (manifest_dir == NULL) {
        fr_error_set(err, "out of memory deriving the manifest directory");
        fr_manifest_free(&manifest);
        fr_registry_destroy(registry);
        return FR_ERR;
    }
```

Add `#include "config.h"` and `#include "config_json.h"` to `src/sync.c`. Leave the rest of `fr_sync` alone, and check its cleanup path at the end of the function: the registry is now created earlier, so make sure every early return after this point still destroys it exactly once.

- [ ] **Step 7: Run the whole suite**

Run: `ctest --test-dir build -C Release --output-on-failure`
Expected: PASS, including `test_e2e` and `test_e2e_languages`, which go through `fr_sync` and therefore now go through the seam.

- [ ] **Step 8: Update STRUCTURE.md**

Add to the section 2 table, above the `region.c` row:

```
| finding the manifest and choosing its format | `config.c` | a parser. It dispatches to a registered `fr_config_plugin` |
| reading a json manifest | `config_json.c` | the only possible format. It is one registered `fr_config_plugin` |
```

- [ ] **Step 9: Commit**

```bash
git add src/config.c src/config.h src/config_json.c src/config_json.h src/sync.c src/sync.h test/test_config.c test/fixtures/search STRUCTURE.md
git commit -m "feat(config): dispatch the manifest through a registered format plugin"
```

---

### Task 4: Vendor a TOML parser and read TOML manifests

**Files:**
- Create: `vendor/toml/toml.c`, `vendor/toml/toml.h`, `src/config_toml.c`, `src/config_toml.h`, `test/test_config_toml.c`, `test/fixtures/toml/types.toml`, `test/fixtures/toml/broken.toml`
- Modify: `CMakeLists.txt:18-19`, `src/sync.c` (register the format), `test/test_config.c`, `test/fixtures/search/both/daukle.toml`

**Interfaces:**
- Consumes: the config table (Task 2), the seam (Task 3).
- Produces: `extern const fr_config_plugin FR_CONFIG_TOML;`

- [ ] **Step 1: Vendor the parser**

Download `toml.c` and `toml.h` from `github.com/cktan/tomlc99` (MIT) at its current head into `vendor/toml/`. Then **read `vendor/toml/toml.h`** and confirm the accessor names used below exist: `toml_parse`, `toml_free`, `toml_key_in`, `toml_table_in`, `toml_array_in`, `toml_string_in`, `toml_int_in`, `toml_double_in`, `toml_bool_in`, `toml_array_nelem`, `toml_string_at`, `toml_int_at`, `toml_double_at`, `toml_bool_at`, `toml_table_at`, `toml_array_at`. If the vendored header spells them differently, adapt the accessor names in Step 5; the shape of the walk does not change.

- [ ] **Step 2: Add it to the vendor target**

In `CMakeLists.txt`, replace the `daukle_vendor` lines with:

```cmake
add_library(daukle_vendor STATIC vendor/cJSON/cJSON.c vendor/toml/toml.c)
target_include_directories(daukle_vendor PUBLIC vendor/cJSON vendor/toml)
```

Re-run `cmake -S . -B build` so the new source is picked up.

- [ ] **Step 3: Write the fixtures**

`test/fixtures/toml/types.toml`:

```toml
# a comment, which json cannot hold
name = "daukle"
count = 3
ratio = 1.5
enabled = true
tags = ["a", "b"]

[nested]
key = "value"

[[rows]]
id = "first"

[[rows]]
id = "second"
```

`test/fixtures/toml/broken.toml`:

```toml
name = "unterminated
```

- [ ] **Step 4: Write the failing test**

Create `test/test_config_toml.c`:

```c
#include "greatest.h"
#include "config_toml.h"
#include "region.h"

#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

static cJSON *parse_fixture(const char *path, int *status) {
    fr_error err;
    char *text = NULL;
    if (fr_file_read_text(path, &text, &err) != FR_OK) { *status = FR_ERR; return NULL; }
    cJSON *document = NULL;
    *status = FR_CONFIG_TOML.load(FR_CONFIG_TOML.state, text, path, ".", NULL, NULL, &document, &err);
    free(text);
    return document;
}

TEST maps_every_toml_type_onto_json(void) {
    int status = FR_ERR;
    cJSON *document = parse_fixture("test/fixtures/toml/types.toml", &status);
    ASSERT_EQ(FR_OK, status);
    ASSERT(cJSON_IsObject(document));

    ASSERT_STR_EQ("daukle", cJSON_GetObjectItemCaseSensitive(document, "name")->valuestring);
    ASSERT_EQ(3, cJSON_GetObjectItemCaseSensitive(document, "count")->valueint);
    ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(document, "enabled")));

    const cJSON *tags = cJSON_GetObjectItemCaseSensitive(document, "tags");
    ASSERT(cJSON_IsArray(tags));
    ASSERT_EQ(2, cJSON_GetArraySize(tags));

    const cJSON *nested = cJSON_GetObjectItemCaseSensitive(document, "nested");
    ASSERT_STR_EQ("value", cJSON_GetObjectItemCaseSensitive(nested, "key")->valuestring);

    const cJSON *rows = cJSON_GetObjectItemCaseSensitive(document, "rows");
    ASSERT(cJSON_IsArray(rows));
    ASSERT_EQ(2, cJSON_GetArraySize(rows));
    ASSERT_STR_EQ("second", cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(rows, 1), "id")->valuestring);

    cJSON_Delete(document);
    PASS();
}

TEST reports_a_broken_toml_file_with_its_origin(void) {
    fr_error err;
    char *text = NULL;
    fr_file_read_text("test/fixtures/toml/broken.toml", &text, &err);
    cJSON *document = NULL;
    ASSERT_EQ(FR_ERR, FR_CONFIG_TOML.load(FR_CONFIG_TOML.state, text,
                                          "test/fixtures/toml/broken.toml", ".", NULL, NULL,
                                          &document, &err));
    ASSERT(strstr(err.message, "broken.toml") != NULL);
    free(text);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(maps_every_toml_type_onto_json);
    RUN_TEST(reports_a_broken_toml_file_with_its_origin);
    GREATEST_MAIN_END();
}
```

- [ ] **Step 5: Run it and watch it fail, then implement the walk**

Run: `cmake --build build --config Release`
Expected: FAIL to compile, `config_toml.h` not found.

`src/config_toml.h`:

```c
#ifndef DAUKLE_CONFIG_TOML_H
#define DAUKLE_CONFIG_TOML_H

#include "registry.h"

extern const fr_config_plugin FR_CONFIG_TOML;

#endif
```

`src/config_toml.c`:

```c
#include "config_toml.h"

#include "error.h"

#include "cJSON.h"
#include "toml.h"

#include <stdlib.h>
#include <string.h>

static cJSON *table_to_json(toml_table_t *table);
static cJSON *array_to_json(toml_array_t *array);

static cJSON *datum_string(toml_datum_t datum) {
    if (!datum.ok) return NULL;
    cJSON *value = cJSON_CreateString(datum.u.s);
    free(datum.u.s);
    return value;
}

static cJSON *value_in_table(toml_table_t *table, const char *key) {
    toml_table_t *child = toml_table_in(table, key);
    if (child != NULL) return table_to_json(child);

    toml_array_t *array = toml_array_in(table, key);
    if (array != NULL) return array_to_json(array);

    toml_datum_t text = toml_string_in(table, key);
    if (text.ok) return datum_string(text);

    toml_datum_t boolean = toml_bool_in(table, key);
    if (boolean.ok) return cJSON_CreateBool(boolean.u.b);

    toml_datum_t integer = toml_int_in(table, key);
    if (integer.ok) return cJSON_CreateNumber((double) integer.u.i);

    toml_datum_t real = toml_double_in(table, key);
    if (real.ok) return cJSON_CreateNumber(real.u.d);

    /* A date has no json counterpart and daukle has no use for one, so it
       arrives as the text the user wrote. */
    toml_datum_t stamp = toml_timestamp_in(table, key);
    if (stamp.ok) {
        free(stamp.u.ts);
        return cJSON_CreateString("");
    }
    return NULL;
}

static cJSON *value_at_index(toml_array_t *array, int index) {
    toml_table_t *child = toml_table_at(array, index);
    if (child != NULL) return table_to_json(child);

    toml_array_t *nested = toml_array_at(array, index);
    if (nested != NULL) return array_to_json(nested);

    toml_datum_t text = toml_string_at(array, index);
    if (text.ok) return datum_string(text);

    toml_datum_t boolean = toml_bool_at(array, index);
    if (boolean.ok) return cJSON_CreateBool(boolean.u.b);

    toml_datum_t integer = toml_int_at(array, index);
    if (integer.ok) return cJSON_CreateNumber((double) integer.u.i);

    toml_datum_t real = toml_double_at(array, index);
    if (real.ok) return cJSON_CreateNumber(real.u.d);
    return NULL;
}

static cJSON *table_to_json(toml_table_t *table) {
    cJSON *object = cJSON_CreateObject();
    if (object == NULL) return NULL;
    for (int index = 0; ; index++) {
        const char *key = toml_key_in(table, index);
        if (key == NULL) break;
        cJSON *value = value_in_table(table, key);
        if (value == NULL) {
            cJSON_Delete(object);
            return NULL;
        }
        cJSON_AddItemToObject(object, key, value);
    }
    return object;
}

static cJSON *array_to_json(toml_array_t *array) {
    cJSON *items = cJSON_CreateArray();
    if (items == NULL) return NULL;
    int count = toml_array_nelem(array);
    for (int index = 0; index < count; index++) {
        cJSON *value = value_at_index(array, index);
        if (value == NULL) {
            cJSON_Delete(items);
            return NULL;
        }
        cJSON_AddItemToArray(items, value);
    }
    return items;
}

static int config_toml_load(void *state, const char *text, const char *origin, const char *base_dir,
                            fr_registry *registry, const cJSON *document,
                            cJSON **out, fr_error *err) {
    (void) state; (void) base_dir; (void) registry; (void) document;

    char *mutable_text = malloc(strlen(text) + 1);
    if (mutable_text == NULL) {
        fr_error_set(err, "out of memory reading \"%s\"", origin);
        return FR_ERR;
    }
    memcpy(mutable_text, text, strlen(text) + 1);

    char message[200];
    toml_table_t *table = toml_parse(mutable_text, message, sizeof message);
    free(mutable_text);
    if (table == NULL) {
        fr_error_set(err, "\"%s\" is not valid toml: %s", origin, message);
        return FR_ERR;
    }

    cJSON *root = table_to_json(table);
    toml_free(table);
    if (root == NULL) {
        fr_error_set(err, "\"%s\" holds a value daukle cannot represent", origin);
        return FR_ERR;
    }
    *out = root;
    return FR_OK;
}

const fr_config_plugin FR_CONFIG_TOML = { "daukle.config/toml", "daukle.toml", 0, config_toml_load, NULL };
```

- [ ] **Step 6: Register the format**

In `src/sync.c`, inside `fr_build_registry`, beside the JSON registration:

```c
    if (fr_registry_add_config(registry, &FR_CONFIG_TOML, err) != FR_OK) {
        fr_registry_destroy(registry);
        return FR_ERR;
    }
```

Add `#include "config_toml.h"`.

- [ ] **Step 7: Run the tests**

Run: `ctest --test-dir build -C Release -R "test_config" --output-on-failure`
Expected: PASS for `test_config_toml`.

- [ ] **Step 8: Finish the two manifests error**

Put a real manifest in `test/fixtures/search/both/daukle.toml`:

```toml
schema = 1
project = "forebay/search"
version = "1.0.0"

[modules]
[sources]
consumers = []
```

Remove the `SKIPm` line added in Task 3 Step 2 from `refuses_two_manifests_in_one_directory`.

Then replace the hardcoded name in `fr_config_find`'s final error with the registered list:

```c
    char expected[256];
    size_t written = 0;
    expected[0] = '\0';
    for (size_t index = 0; index < fr_registry_config_count(registry); index++) {
        const fr_config_plugin *plugin = fr_registry_config_at(registry, index);
        int added = snprintf(expected + written, sizeof expected - written,
                             written == 0 ? "%s" : ", %s", plugin->file_name);
        if (added < 0 || (size_t) added >= sizeof expected - written) break;
        written += (size_t) added;
    }
    fr_error_set(err, "\"%s\" holds no manifest: expected one of %s", directory, expected);
```

Update `reports_a_directory_with_no_manifest` to assert on `"daukle.toml"` as well, so the list is actually checked.

- [ ] **Step 9: Run the whole suite**

Run: `ctest --test-dir build -C Release --output-on-failure`
Expected: PASS.

- [ ] **Step 10: Update STRUCTURE.md**

Add below the `config_json.c` row:

```
| reading a toml manifest | `config_toml.c` | a json reader. It is a peer format, registered the same way |
```

and add `vendor/toml  TOML parsing` to the tree listing in section 1.

- [ ] **Step 11: Commit**

```bash
git add vendor/toml src/config_toml.c src/config_toml.h src/sync.c src/config.c CMakeLists.txt test/test_config_toml.c test/test_config.c test/fixtures/toml test/fixtures/search STRUCTURE.md
git commit -m "feat(config): read a manifest written in toml"
```

---

### Task 5: Prove TOML and JSON are the same manifest

**Files:**
- Create: `test/fixtures/three-ways/json/daukle.json`, `test/fixtures/three-ways/toml/daukle.toml`, and a producer both point at
- Modify: `test/test_e2e.c`

**Interfaces:**
- Consumes: everything from Tasks 1 to 4.
- Produces: the equivalence assertion later tasks extend with Lua.

- [ ] **Step 1: Write the fixtures**

Create `test/fixtures/three-ways/producer/daukle.json` by copying `test/fixtures/producer/daukle.json` unchanged.

`test/fixtures/three-ways/json/daukle.json`:

```json
{
  "schema": 1,
  "project": "forebay/three-ways",
  "version": "1.0.0",
  "modules": {},
  "sources": {
    "forebay/basekit": { "kind": "path", "path": "../producer" }
  },
  "consumers": [
    {
      "id": "stub",
      "language": "npm",
      "file": "package.json",
      "configuration": "dependencies",
      "dependencies": {
        "forebay/basekit": { "version": "^5.0.0", "modules": ["ir"] }
      }
    }
  ]
}
```

`test/fixtures/three-ways/toml/daukle.toml`:

```toml
schema = 1
project = "forebay/three-ways"
version = "1.0.0"

[modules]

[sources."forebay/basekit"]
kind = "path"
path = "../producer"

[[consumers]]
id = "stub"
language = "npm"
file = "package.json"
configuration = "dependencies"

  [consumers.dependencies."forebay/basekit"]
  version = "^5.0.0"
  modules = ["ir"]
```

Both directories need the consumer file the language plugin writes into. Copy `test/fixtures/consumer/package.json` (or the nearest existing npm consumer fixture) into `test/fixtures/three-ways/json/package.json` and `test/fixtures/three-ways/toml/package.json`. Add both `package.json` paths to `.gitignore` if the existing npm fixtures are ignored; check how `test/fixtures/consumer/build.gradle` is handled and follow it.

- [ ] **Step 2: Write the failing test**

Add to `test/test_e2e.c`:

```c
TEST the_same_manifest_in_json_and_toml_writes_the_same_file(void) {
    fr_error err;
    fr_sync_report report;

    ASSERT_EQ(FR_OK, fr_sync("test/fixtures/three-ways/json/daukle.json", 1, 0, &report, &err));
    fr_sync_report_free(&report);
    char *from_json = NULL;
    ASSERT_EQ(FR_OK, fr_file_read_text("test/fixtures/three-ways/json/package.json", &from_json, &err));

    ASSERT_EQ(FR_OK, fr_sync("test/fixtures/three-ways/toml/daukle.toml", 1, 0, &report, &err));
    fr_sync_report_free(&report);
    char *from_toml = NULL;
    ASSERT_EQ(FR_OK, fr_file_read_text("test/fixtures/three-ways/toml/package.json", &from_toml, &err));

    ASSERT_STR_EQ(from_json, from_toml);
    free(from_json);
    free(from_toml);
    PASS();
}
```

Register it in that file's `main`, and add `#include "region.h"` if `fr_file_read_text` is not already reachable there.

- [ ] **Step 3: Run it**

Run: `ctest --test-dir build -C Release -R test_e2e --output-on-failure`
Expected: PASS. If it fails on the dependency block, print both documents with `cJSON_Print` and compare: the usual cause is `[consumers.dependencies."forebay/basekit"]` attaching to the wrong table, which TOML resolves against the **last** `[[consumers]]` element.

- [ ] **Step 4: Commit**

```bash
git add test/fixtures/three-ways test/test_e2e.c .gitignore
git commit -m "test(config): prove json and toml manifests write identical output"
```

---

### Task 6: Vendor Lua and open a sandbox-free state

**Files:**
- Create: `vendor/lua/*` (library sources only), `src/luax.c`, `src/luax.h`, `test/test_luax.c`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces:
  - `lua_State *fr_lua_open(size_t memory_limit, fr_error *err)`
  - `void fr_lua_close(lua_State *state)`
  - `int fr_lua_run(lua_State *state, const char *text, const char *chunk_name, fr_error *err)`, which formats `chunk:line: message` plus up to five frames into `err`

- [ ] **Step 1: Vendor Lua**

Download `lua-5.4.7.tar.gz` from `https://www.lua.org/ftp/lua-5.4.7.tar.gz`. Copy every `src/*.c` and `src/*.h` into `vendor/lua/` **except `lua.c` and `luac.c`**, which are the standalone interpreter and compiler and pull in readline.

- [ ] **Step 2: Add it to the vendor target**

In `CMakeLists.txt`:

```cmake
file(GLOB DAUKLE_LUA_SOURCES CONFIGURE_DEPENDS vendor/lua/*.c)
add_library(daukle_vendor STATIC vendor/cJSON/cJSON.c vendor/toml/toml.c ${DAUKLE_LUA_SOURCES})
target_include_directories(daukle_vendor PUBLIC vendor/cJSON vendor/toml vendor/lua)
if(NOT WIN32)
    target_compile_definitions(daukle_vendor PRIVATE LUA_USE_POSIX)
    target_link_libraries(daukle_vendor PUBLIC m)
endif()
```

Re-run `cmake -S . -B build` and build. Expected: the vendor target compiles. It carries no warning flags, which is what makes this possible.

- [ ] **Step 3: Write the failing test**

Create `test/test_luax.c`:

```c
#include "greatest.h"
#include "luax.h"

#include <string.h>

TEST opens_and_runs_a_chunk(void) {
    fr_error err;
    lua_State *state = fr_lua_open(64u * 1024u * 1024u, &err);
    ASSERT(state != NULL);
    ASSERT_EQ(FR_OK, fr_lua_run(state, "local x = 1 + 1", "=test", &err));
    fr_lua_close(state);
    PASS();
}

TEST reports_a_syntax_error_with_its_chunk_name(void) {
    fr_error err;
    lua_State *state = fr_lua_open(64u * 1024u * 1024u, &err);
    ASSERT_EQ(FR_ERR, fr_lua_run(state, "local = =", "daukle.lua", &err));
    ASSERT(strstr(err.message, "daukle.lua") != NULL);
    fr_lua_close(state);
    PASS();
}

TEST reports_a_runtime_error_with_its_line(void) {
    fr_error err;
    lua_State *state = fr_lua_open(64u * 1024u * 1024u, &err);
    ASSERT_EQ(FR_ERR, fr_lua_run(state, "\nerror('boom')", "daukle.lua", &err));
    ASSERT(strstr(err.message, "boom") != NULL);
    ASSERT(strstr(err.message, "daukle.lua:2") != NULL);
    fr_lua_close(state);
    PASS();
}

TEST stops_a_script_that_allocates_without_end(void) {
    fr_error err;
    lua_State *state = fr_lua_open(1u * 1024u * 1024u, &err);
    ASSERT_EQ(FR_ERR, fr_lua_run(state,
        "local t = {} local i = 1 while true do t[i] = string.rep('x', 1024) i = i + 1 end",
        "daukle.lua", &err));
    fr_lua_close(state);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(opens_and_runs_a_chunk);
    RUN_TEST(reports_a_syntax_error_with_its_chunk_name);
    RUN_TEST(reports_a_runtime_error_with_its_line);
    RUN_TEST(stops_a_script_that_allocates_without_end);
    GREATEST_MAIN_END();
}
```

- [ ] **Step 4: Run it and watch it fail**

Run: `cmake --build build --config Release`
Expected: FAIL to compile, `luax.h` not found.

- [ ] **Step 5: Implement**

`src/luax.h`:

```c
#ifndef DAUKLE_LUAX_H
#define DAUKLE_LUAX_H

#include "types.h"

#include <stddef.h>

#include "lua.h"

lua_State *fr_lua_open(size_t memory_limit, fr_error *err);
void fr_lua_close(lua_State *state);
int fr_lua_run(lua_State *state, const char *text, const char *chunk_name, fr_error *err);

#endif
```

`src/luax.c`:

```c
#include "luax.h"

#include "error.h"

#include "lauxlib.h"
#include "lualib.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t limit;
    size_t used;
} fr_lua_budget;

static void *capped_alloc(void *ud, void *pointer, size_t old_size, size_t new_size) {
    fr_lua_budget *budget = ud;
    if (new_size == 0) {
        free(pointer);
        budget->used -= old_size;
        return NULL;
    }
    if (new_size > old_size && budget->used + (new_size - old_size) > budget->limit) return NULL;
    void *moved = realloc(pointer, new_size);
    if (moved == NULL) return NULL;
    budget->used += new_size - old_size;
    return moved;
}

static int add_traceback(lua_State *state) {
    const char *message = lua_tostring(state, 1);
    luaL_traceback(state, state, message == NULL ? "error" : message, 1);
    return 1;
}

lua_State *fr_lua_open(size_t memory_limit, fr_error *err) {
    fr_lua_budget *budget = calloc(1, sizeof *budget);
    if (budget == NULL) {
        fr_error_set(err, "out of memory creating the lua budget");
        return NULL;
    }
    budget->limit = memory_limit;
    lua_State *state = lua_newstate(capped_alloc, budget);
    if (state == NULL) {
        free(budget);
        fr_error_set(err, "could not create a lua state");
        return NULL;
    }
    luaL_openlibs(state);
    return state;
}

void fr_lua_close(lua_State *state) {
    if (state == NULL) return;
    void *budget = NULL;
    lua_getallocf(state, &budget);
    lua_close(state);
    free(budget);
}

int fr_lua_run(lua_State *state, const char *text, const char *chunk_name, fr_error *err) {
    if (luaL_loadbuffer(state, text, strlen(text), chunk_name) != LUA_OK) {
        fr_error_set(err, "%s", lua_tostring(state, -1));
        lua_pop(state, 1);
        return FR_ERR;
    }
    lua_pushcfunction(state, add_traceback);
    lua_insert(state, -2);
    if (lua_pcall(state, 0, 0, -2) != LUA_OK) {
        fr_error_set(err, "%s", lua_tostring(state, -1));
        lua_pop(state, 2);
        return FR_ERR;
    }
    lua_pop(state, 1);
    return FR_OK;
}
```

`fr_error`'s buffer is 512 bytes, so `fr_error_set` truncates a long traceback. That is the intended behaviour; Task 13 adds `--verbose` for the untruncated text.

- [ ] **Step 6: Run the tests**

Run: `ctest --test-dir build -C Release -R test_luax --output-on-failure`
Expected: PASS, 4 tests. If `stops_a_script_that_allocates_without_end` hangs rather than failing, the allocator is not being consulted; check that `lua_newstate` got `capped_alloc` and not `luaL_newstate`.

- [ ] **Step 7: Update STRUCTURE.md**

Add to section 2:

```
| the lua state, its memory cap and its errors | `luax.c` | the sandbox, which is `lua_sandbox.c`, nor the config format, which is `config_lua.c` |
```

and add `vendor/lua  the lua 5.4 library, no standalone interpreter` to the section 1 tree.

- [ ] **Step 8: Commit**

```bash
git add vendor/lua src/luax.c src/luax.h CMakeLists.txt test/test_luax.c STRUCTURE.md
git commit -m "feat(lua): open a memory capped lua state and report its errors"
```

---

### Task 7: cJSON to Lua

**Files:**
- Modify: `src/luax.c`, `src/luax.h`, `test/test_luax.c`

**Interfaces:**
- Produces: `int fr_lua_push_json(lua_State *state, const struct cJSON *value, fr_error *err)`, which pushes exactly one value onto the stack.

- [ ] **Step 1: Write the failing test**

Add to `test/test_luax.c`:

```c
TEST pushes_a_json_document_as_a_lua_table(void) {
    fr_error err;
    lua_State *state = fr_lua_open(64u * 1024u * 1024u, &err);
    cJSON *document = cJSON_Parse(
        "{\"name\":\"x\",\"count\":2,\"on\":true,\"off\":false,\"none\":null,"
        "\"list\":[\"a\",\"b\"],\"nested\":{\"key\":\"value\"}}");
    ASSERT(document != NULL);

    ASSERT_EQ(FR_OK, fr_lua_push_json(state, document, &err));
    lua_setglobal(state, "document");
    cJSON_Delete(document);

    ASSERT_EQ(FR_OK, fr_lua_run(state,
        "assert(document.name == 'x')\n"
        "assert(document.count == 2)\n"
        "assert(document.on == true)\n"
        "assert(document.off == false)\n"
        "assert(document.none == nil)\n"
        "assert(#document.list == 2 and document.list[2] == 'b')\n"
        "assert(document.nested.key == 'value')\n", "=check", &err));
    fr_lua_close(state);
    PASS();
}
```

Add `#include "cJSON.h"` to the test file and register the test in `main`.

- [ ] **Step 2: Run it and watch it fail**

Run: `cmake --build build --config Release`
Expected: FAIL, `fr_lua_push_json` undefined.

- [ ] **Step 3: Implement**

Declare in `src/luax.h` (add `struct cJSON;` above it):

```c
int fr_lua_push_json(lua_State *state, const struct cJSON *value, fr_error *err);
```

Implement in `src/luax.c` (add `#include "cJSON.h"`):

```c
int fr_lua_push_json(lua_State *state, const cJSON *value, fr_error *err) {
    if (value == NULL || cJSON_IsNull(value)) {
        lua_pushnil(state);
        return FR_OK;
    }
    if (cJSON_IsBool(value)) {
        lua_pushboolean(state, cJSON_IsTrue(value));
        return FR_OK;
    }
    if (cJSON_IsNumber(value)) {
        lua_pushnumber(state, value->valuedouble);
        return FR_OK;
    }
    if (cJSON_IsString(value)) {
        lua_pushstring(state, value->valuestring);
        return FR_OK;
    }
    if (cJSON_IsArray(value)) {
        lua_newtable(state);
        int position = 1;
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, value) {
            if (fr_lua_push_json(state, item, err) != FR_OK) return FR_ERR;
            lua_rawseti(state, -2, position++);
        }
        return FR_OK;
    }
    if (cJSON_IsObject(value)) {
        lua_newtable(state);
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, value) {
            if (fr_lua_push_json(state, item, err) != FR_OK) return FR_ERR;
            lua_setfield(state, -2, item->string);
        }
        return FR_OK;
    }
    fr_error_set(err, "cannot represent a json value of an unknown type in lua");
    return FR_ERR;
}
```

A JSON null becomes nil, which means a key set to null disappears from the table. daukle's schema has no nullable key, so nothing is lost; a future schema that grows one needs a sentinel instead.

- [ ] **Step 4: Run the tests**

Run: `ctest --test-dir build -C Release -R test_luax --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/luax.c src/luax.h test/test_luax.c
git commit -m "feat(lua): push a json document onto the lua stack"
```

---

### Task 8: Lua to cJSON

**Files:**
- Modify: `src/luax.c`, `src/luax.h`, `test/test_luax.c`

**Interfaces:**
- Produces: `int fr_lua_to_json(lua_State *state, int index, struct cJSON **out, fr_error *err)`, which reads the value at `index` without popping it.

- [ ] **Step 1: Write the failing test**

Add to `test/test_luax.c`:

```c
TEST reads_a_lua_table_back_as_json(void) {
    fr_error err;
    lua_State *state = fr_lua_open(64u * 1024u * 1024u, &err);
    ASSERT_EQ(FR_OK, fr_lua_run(state,
        "result = { name = 'x', count = 2, on = true, list = { 'a', 'b' },"
        " nested = { key = 'value' }, empty = {} }", "=build", &err));
    lua_getglobal(state, "result");

    cJSON *document = NULL;
    ASSERT_EQ(FR_OK, fr_lua_to_json(state, -1, &document, &err));
    ASSERT_STR_EQ("x", cJSON_GetObjectItemCaseSensitive(document, "name")->valuestring);
    ASSERT_EQ(2, cJSON_GetObjectItemCaseSensitive(document, "count")->valueint);
    ASSERT(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(document, "on")));
    ASSERT(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(document, "list")));
    ASSERT_EQ(2, cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(document, "list")));
    ASSERT_STR_EQ("value",
        cJSON_GetObjectItemCaseSensitive(
            cJSON_GetObjectItemCaseSensitive(document, "nested"), "key")->valuestring);
    ASSERT(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(document, "empty")));

    cJSON_Delete(document);
    fr_lua_close(state);
    PASS();
}

TEST rejects_a_table_key_that_is_not_a_string(void) {
    fr_error err;
    lua_State *state = fr_lua_open(64u * 1024u * 1024u, &err);
    fr_lua_run(state, "result = { [true] = 'x' }", "=build", &err);
    lua_getglobal(state, "result");
    cJSON *document = NULL;
    ASSERT_EQ(FR_ERR, fr_lua_to_json(state, -1, &document, &err));
    fr_lua_close(state);
    PASS();
}
```

Register both in `main`.

- [ ] **Step 2: Run it and watch it fail**

Run: `cmake --build build --config Release`
Expected: FAIL, `fr_lua_to_json` undefined.

- [ ] **Step 3: Implement**

Declare in `src/luax.h`:

```c
int fr_lua_to_json(lua_State *state, int index, struct cJSON **out, fr_error *err);
```

Implement in `src/luax.c`:

```c
/* An empty table is an object, and a table whose keys are exactly 1..n is an
   array: lua cannot tell the two apart and the schema's arrays are never
   empty at the point this runs. */
static int table_is_array(lua_State *state, int index) {
    lua_Integer expected = 1;
    lua_pushnil(state);
    while (lua_next(state, index) != 0) {
        if (!lua_isinteger(state, -2) || lua_tointeger(state, -2) != expected) {
            lua_pop(state, 2);
            return 0;
        }
        expected++;
        lua_pop(state, 1);
    }
    return expected > 1;
}

int fr_lua_to_json(lua_State *state, int index, cJSON **out, fr_error *err) {
    *out = NULL;
    int absolute = lua_absindex(state, index);

    switch (lua_type(state, absolute)) {
        case LUA_TNIL:
            *out = cJSON_CreateNull();
            break;
        case LUA_TBOOLEAN:
            *out = cJSON_CreateBool(lua_toboolean(state, absolute));
            break;
        case LUA_TNUMBER:
            *out = cJSON_CreateNumber(lua_tonumber(state, absolute));
            break;
        case LUA_TSTRING:
            *out = cJSON_CreateString(lua_tostring(state, absolute));
            break;
        case LUA_TTABLE: {
            int is_array = table_is_array(state, absolute);
            cJSON *container = is_array ? cJSON_CreateArray() : cJSON_CreateObject();
            if (container == NULL) {
                fr_error_set(err, "out of memory reading a lua table");
                return FR_ERR;
            }
            lua_pushnil(state);
            while (lua_next(state, absolute) != 0) {
                if (!is_array && lua_type(state, -2) != LUA_TSTRING) {
                    fr_error_set(err, "a config table key must be a string, found %s",
                                 lua_typename(state, lua_type(state, -2)));
                    lua_pop(state, 2);
                    cJSON_Delete(container);
                    return FR_ERR;
                }
                cJSON *value = NULL;
                if (fr_lua_to_json(state, -1, &value, err) != FR_OK) {
                    lua_pop(state, 2);
                    cJSON_Delete(container);
                    return FR_ERR;
                }
                if (is_array) cJSON_AddItemToArray(container, value);
                else cJSON_AddItemToObject(container, lua_tostring(state, -2), value);
                lua_pop(state, 1);
            }
            *out = container;
            break;
        }
        default:
            fr_error_set(err, "a config value may not be a %s",
                         lua_typename(state, lua_type(state, absolute)));
            return FR_ERR;
    }

    if (*out == NULL) {
        fr_error_set(err, "out of memory reading a lua value");
        return FR_ERR;
    }
    return FR_OK;
}
```

`table_is_array` walks the table with `lua_next`, and `lua_next` on a key that is being converted with `lua_tostring` corrupts the traversal. The conversion above only ever calls `lua_tostring` on a key already known to be a string, which is safe.

- [ ] **Step 4: Run the tests**

Run: `ctest --test-dir build -C Release -R test_luax --output-on-failure`
Expected: PASS, 7 tests.

- [ ] **Step 5: Commit**

```bash
git add src/luax.c src/luax.h test/test_luax.c
git commit -m "feat(lua): read a lua table back as a json document"
```

---

### Task 9: The sandbox

**Files:**
- Create: `src/lua_sandbox.c`, `src/lua_sandbox.h`, `test/test_lua_sandbox.c`
- Modify: `src/luax.c` (the instruction hook)

**Interfaces:**
- Consumes: `fr_lua_open`, `fr_lua_run`.
- Produces:
  - `int fr_lua_sandbox_install(lua_State *state, const char *base_dir, fr_error *err)`
  - `void fr_lua_set_instruction_limit(lua_State *state, long limit)` in `luax.h`

- [ ] **Step 1: Write the failing test**

Create `test/test_lua_sandbox.c`:

```c
#include "greatest.h"
#include "luax.h"
#include "lua_sandbox.h"

#include <string.h>

static lua_State *sandboxed(fr_error *err) {
    lua_State *state = fr_lua_open(64u * 1024u * 1024u, err);
    fr_lua_sandbox_install(state, "test/fixtures/lua", err);
    return state;
}

TEST names_a_removed_library_in_the_error(void) {
    fr_error err;
    lua_State *state = sandboxed(&err);
    ASSERT_EQ(FR_ERR, fr_lua_run(state, "io.open('secret', 'r')", "daukle.lua", &err));
    ASSERT(strstr(err.message, "io is not available") != NULL);
    fr_lua_close(state);
    PASS();
}

TEST keeps_the_libraries_a_config_needs(void) {
    fr_error err;
    lua_State *state = sandboxed(&err);
    ASSERT_EQ(FR_OK, fr_lua_run(state,
        "assert(string.rep('a', 3) == 'aaa')\n"
        "assert(table.concat({'a','b'}, ',') == 'a,b')\n"
        "assert(math.max(1, 2) == 2)\n", "daukle.lua", &err));
    fr_lua_close(state);
    PASS();
}

TEST stops_a_script_that_never_finishes(void) {
    fr_error err;
    lua_State *state = sandboxed(&err);
    fr_lua_set_instruction_limit(state, 100000);
    ASSERT_EQ(FR_ERR, fr_lua_run(state, "while true do end", "daukle.lua", &err));
    ASSERT(strstr(err.message, "too long") != NULL);
    fr_lua_close(state);
    PASS();
}

TEST includes_a_file_beside_the_manifest(void) {
    fr_error err;
    lua_State *state = sandboxed(&err);
    ASSERT_EQ(FR_OK, fr_lua_run(state,
        "daukle.include('helper.lua')\n"
        "assert(helper_ran == true)\n", "daukle.lua", &err));
    fr_lua_close(state);
    PASS();
}

TEST refuses_an_include_that_climbs_out(void) {
    fr_error err;
    lua_State *state = sandboxed(&err);
    ASSERT_EQ(FR_ERR, fr_lua_run(state, "daukle.include('../escape.lua')", "daukle.lua", &err));
    ASSERT(strstr(err.message, "outside") != NULL);
    fr_lua_close(state);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(names_a_removed_library_in_the_error);
    RUN_TEST(keeps_the_libraries_a_config_needs);
    RUN_TEST(stops_a_script_that_never_finishes);
    RUN_TEST(includes_a_file_beside_the_manifest);
    RUN_TEST(refuses_an_include_that_climbs_out);
    GREATEST_MAIN_END();
}
```

Create `test/fixtures/lua/helper.lua` containing `helper_ran = true`.

- [ ] **Step 2: Run it and watch it fail**

Run: `cmake --build build --config Release`
Expected: FAIL, `lua_sandbox.h` not found.

- [ ] **Step 3: Add the instruction limit to `luax.c`**

In `src/luax.h`:

```c
void fr_lua_set_instruction_limit(lua_State *state, long limit);
```

In `src/luax.c`:

```c
static void instruction_budget_spent(lua_State *state, lua_Debug *activation) {
    (void) activation;
    luaL_error(state, "this configuration ran for too long and was stopped");
}

void fr_lua_set_instruction_limit(lua_State *state, long limit) {
    lua_sethook(state, instruction_budget_spent, LUA_MASKCOUNT, (int) limit);
}
```

Call it from `fr_lua_open` with the default 50 million so every state carries a budget:

```c
    fr_lua_set_instruction_limit(state, 50000000);
```

The hook fires once and raises; it is not re-armed, which is correct, because the run is over.

- [ ] **Step 4: Implement the sandbox**

`src/lua_sandbox.h`:

```c
#ifndef DAUKLE_LUA_SANDBOX_H
#define DAUKLE_LUA_SANDBOX_H

#include "types.h"

#include "lua.h"

/* Replaces the global table with a curated one and publishes daukle.include.
   base_dir bounds every include, and is copied into the state's registry. */
int fr_lua_sandbox_install(lua_State *state, const char *base_dir, fr_error *err);

#endif
```

`src/lua_sandbox.c`:

```c
#include "lua_sandbox.h"

#include "error.h"
#include "region.h"

#include "lauxlib.h"

#include <stdlib.h>
#include <string.h>

#define FR_SANDBOX_BASE_DIR "daukle.base_dir"

static const char *KEPT[] = {
    "assert", "error", "ipairs", "pairs", "next", "select", "tonumber", "tostring",
    "type", "pcall", "xpcall", "setmetatable", "getmetatable", "rawget", "rawset",
    "rawequal", "rawlen", "string", "table", "math", "_VERSION"
};

static const char *REMOVED[] = {
    "io", "os", "package", "require", "dofile", "loadfile", "load", "debug",
    "collectgarbage", "print"
};

static int removed_name(lua_State *state) {
    const char *name = lua_tostring(state, 2);
    for (size_t index = 0; index < sizeof REMOVED / sizeof REMOVED[0]; index++) {
        if (name != NULL && strcmp(REMOVED[index], name) == 0) {
            return luaL_error(state, "%s is not available in a daukle configuration", name);
        }
    }
    lua_pushnil(state);
    return 1;
}

static int climbs_out(const char *relative_path) {
    if (relative_path[0] == '/' || relative_path[0] == '\\') return 1;
    if (relative_path[0] != '\0' && relative_path[1] == ':') return 1;
    for (const char *cursor = relative_path; *cursor != '\0'; cursor++) {
        if (cursor[0] == '.' && cursor[1] == '.') return 1;
    }
    return 0;
}

static int sandbox_include(lua_State *state) {
    const char *relative_path = luaL_checkstring(state, 1);
    if (climbs_out(relative_path)) {
        return luaL_error(state, "\"%s\" is outside the project directory", relative_path);
    }

    lua_getfield(state, LUA_REGISTRYINDEX, FR_SANDBOX_BASE_DIR);
    const char *base_dir = lua_tostring(state, -1);
    lua_pop(state, 1);

    char path[512];
    snprintf(path, sizeof path, "%s/%s", base_dir, relative_path);

    fr_error err;
    char *text = NULL;
    if (fr_file_read_text(path, &text, &err) != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }
    int status = luaL_loadbuffer(state, text, strlen(text), relative_path);
    free(text);
    if (status != LUA_OK) return lua_error(state);
    lua_call(state, 0, 0);
    return 0;
}

int fr_lua_sandbox_install(lua_State *state, const char *base_dir, fr_error *err) {
    lua_pushstring(state, base_dir);
    lua_setfield(state, LUA_REGISTRYINDEX, FR_SANDBOX_BASE_DIR);

    lua_newtable(state);
    for (size_t index = 0; index < sizeof KEPT / sizeof KEPT[0]; index++) {
        lua_getglobal(state, KEPT[index]);
        lua_setfield(state, -2, KEPT[index]);
    }

    lua_newtable(state);
    lua_pushcfunction(state, removed_name);
    lua_setfield(state, -2, "__index");
    lua_setmetatable(state, -2);

    lua_newtable(state);
    lua_pushcfunction(state, sandbox_include);
    lua_setfield(state, -2, "include");
    lua_setfield(state, -2, "daukle");

    lua_pushvalue(state, -1);
    lua_setfield(state, -2, "_G");
    lua_rawseti(state, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);

    (void) err;
    return FR_OK;
}
```

The metatable on the new globals table is what turns `io.open(...)` into a message naming `io` instead of "attempt to index a nil value".

- [ ] **Step 5: Run the tests**

Run: `ctest --test-dir build -C Release -R test_lua_sandbox --output-on-failure`
Expected: PASS, 5 tests. If `keeps_the_libraries_a_config_needs` fails, the globals swap happened before `luaL_openlibs` populated the old table; `fr_lua_sandbox_install` must run after `fr_lua_open`.

- [ ] **Step 6: Update STRUCTURE.md**

```
| what a configuration script may touch | `lua_sandbox.c` | a permission system. It curates one globals table and bounds daukle.include |
```

- [ ] **Step 7: Commit**

```bash
git add src/lua_sandbox.c src/lua_sandbox.h src/luax.c src/luax.h test/test_lua_sandbox.c test/fixtures/lua STRUCTURE.md
git commit -m "feat(lua): bound a configuration script to a curated environment"
```

---

### Task 10: The Lua overlay format

**Files:**
- Create: `src/config_lua.c`, `src/config_lua.h`, `test/test_config_lua.c`, `test/fixtures/lua-overlay/daukle.toml`, `test/fixtures/lua-overlay/daukle.lua`
- Modify: `src/sync.c` (register the format and shut the runtime down)

**Interfaces:**
- Consumes: Tasks 6 to 9.
- Produces:
  - `extern const fr_config_plugin FR_CONFIG_LUA;`
  - `void fr_lua_runtime_shutdown(void);`, which must be called **after** `fr_registry_destroy`, because plugins the script registered point at strings this runtime owns

- [ ] **Step 1: Write the fixtures**

`test/fixtures/lua-overlay/daukle.toml`:

```toml
schema = 1
project = "forebay/overlay"
version = "1.0.0"

[modules]
[sources]
consumers = []
```

`test/fixtures/lua-overlay/daukle.lua`:

```lua
daukle.config.version = "2.0.0"
daukle.config.modules.generated = { requires = {} }

if daukle.host.os ~= nil then
  daukle.config.project = "forebay/overlay"
end
```

- [ ] **Step 2: Write the failing test**

Create `test/test_config_lua.c`:

```c
#include "greatest.h"
#include "config.h"
#include "config_lua.h"
#include "registry.h"
#include "sync.h"

#include <string.h>

TEST a_script_beside_a_toml_manifest_changes_it(void) {
    fr_error err;
    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));
    fr_manifest manifest;
    ASSERT_EQ(FR_OK, fr_config_load_file("test/fixtures/lua-overlay/daukle.toml",
                                         registry, &manifest, &err));
    ASSERT_EQ(2, manifest.self.version.major);
    ASSERT(fr_project_module(&manifest.self, "generated") != NULL);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST a_failing_script_names_its_file(void) {
    fr_error err;
    fr_registry *registry = NULL;
    fr_build_registry(&registry, &err);
    fr_manifest manifest;
    ASSERT_EQ(FR_ERR, fr_config_load_file("test/fixtures/lua-broken/daukle.toml",
                                          registry, &manifest, &err));
    ASSERT(strstr(err.message, "daukle.lua") != NULL);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(a_script_beside_a_toml_manifest_changes_it);
    RUN_TEST(a_failing_script_names_its_file);
    GREATEST_MAIN_END();
}
```

Create `test/fixtures/lua-broken/daukle.toml` as a copy of the overlay one, and `test/fixtures/lua-broken/daukle.lua` containing `error("deliberate")`.

- [ ] **Step 3: Run it and watch it fail**

Run: `cmake --build build --config Release`
Expected: FAIL, `config_lua.h` not found.

- [ ] **Step 4: Implement**

`src/config_lua.h`:

```c
#ifndef DAUKLE_CONFIG_LUA_H
#define DAUKLE_CONFIG_LUA_H

#include "registry.h"

extern const fr_config_plugin FR_CONFIG_LUA;

/* The registry stores plugin structs by value and does not own their capability
   strings, so the runtime that owns them outlives the registry and is torn down
   only after it. */
void fr_lua_runtime_shutdown(void);

#endif
```

`src/config_lua.c`:

```c
#include "config_lua.h"

#include "error.h"
#include "luax.h"
#include "lua_sandbox.h"

#include "cJSON.h"
#include "lauxlib.h"

#include <stdlib.h>
#include <string.h>

static lua_State *runtime_state = NULL;

static const char *host_os(void) {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

static const char *host_arch(void) {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#else
    return "x86";
#endif
}

static int lua_env(lua_State *state) {
    const char *name = luaL_checkstring(state, 1);
    const char *value = getenv(name);
    if (value == NULL) {
        lua_pushnil(state);
        return 1;
    }
    lua_getglobal(state, "daukle");
    lua_getfield(state, -1, "_env_reads");
    lua_pushstring(state, value);
    lua_setfield(state, -2, name);
    lua_pop(state, 2);
    lua_pushstring(state, value);
    return 1;
}

static int lua_log(lua_State *state) {
    const char *message = luaL_checkstring(state, 1);
    fprintf(stderr, "daukle: %s\n", message);
    return 0;
}

static int publish_daukle_table(lua_State *state, const cJSON *document, fr_error *err) {
    lua_getglobal(state, "daukle");

    if (document != NULL) {
        if (fr_lua_push_json(state, document, err) != FR_OK) return FR_ERR;
    } else {
        lua_newtable(state);
    }
    lua_setfield(state, -2, "config");

    lua_newtable(state);
    lua_pushstring(state, host_os());
    lua_setfield(state, -2, "os");
    lua_pushstring(state, host_arch());
    lua_setfield(state, -2, "arch");
    lua_setfield(state, -2, "host");

    lua_newtable(state);
    lua_setfield(state, -2, "_env_reads");

    lua_pushcfunction(state, lua_env);
    lua_setfield(state, -2, "env");
    lua_pushcfunction(state, lua_log);
    lua_setfield(state, -2, "log");

    lua_pop(state, 1);
    return FR_OK;
}

static int config_lua_load(void *state_unused, const char *text, const char *origin,
                           const char *base_dir, fr_registry *registry, const cJSON *document,
                           cJSON **out, fr_error *err) {
    (void) state_unused; (void) registry;

    fr_lua_runtime_shutdown();
    runtime_state = fr_lua_open(64u * 1024u * 1024u, err);
    if (runtime_state == NULL) return FR_ERR;
    if (fr_lua_sandbox_install(runtime_state, base_dir, err) != FR_OK) return FR_ERR;
    if (publish_daukle_table(runtime_state, document, err) != FR_OK) return FR_ERR;

    if (fr_lua_run(runtime_state, text, origin, err) != FR_OK) return FR_ERR;

    lua_getglobal(runtime_state, "daukle");
    lua_getfield(runtime_state, -1, "config");
    int status = fr_lua_to_json(runtime_state, -1, out, err);
    lua_pop(runtime_state, 2);
    return status;
}

void fr_lua_runtime_shutdown(void) {
    if (runtime_state == NULL) return;
    fr_lua_close(runtime_state);
    runtime_state = NULL;
}

const fr_config_plugin FR_CONFIG_LUA = { "daukle.config/lua", "daukle.lua", 1, config_lua_load, NULL };
```

The state stays open after the load returns because Task 11 registers plugins whose closures live in it. `fr_lua_run` prefixes its error with the chunk name, which is `origin`, which is the path, so `a_failing_script_names_its_file` passes without extra wrapping.

- [ ] **Step 5: Register the format and shut the runtime down**

In `src/sync.c`, add the registration beside the other two:

```c
    if (fr_registry_add_config(registry, &FR_CONFIG_LUA, err) != FR_OK) {
        fr_registry_destroy(registry);
        return FR_ERR;
    }
```

and at the end of `fr_sync`, after `fr_registry_destroy(registry)`, add `fr_lua_runtime_shutdown();`. Check every early return in `fr_sync` that destroys the registry and add the shutdown call there too. Add `#include "config_lua.h"`.

- [ ] **Step 6: Run the tests**

Run: `ctest --test-dir build -C Release --output-on-failure`
Expected: PASS everywhere, including the e2e tests, which now run with three formats registered and no `daukle.lua` beside their fixtures.

- [ ] **Step 7: Update STRUCTURE.md**

```
| running a configuration script and reading it back | `config_lua.c` | the sandbox or the lua state, which are `lua_sandbox.c` and `luax.c` |
```

- [ ] **Step 8: Commit**

```bash
git add src/config_lua.c src/config_lua.h src/sync.c test/test_config_lua.c test/fixtures/lua-overlay test/fixtures/lua-broken STRUCTURE.md
git commit -m "feat(config): run a lua script over the manifest it sits beside"
```

---

### Task 11: Plugins declared in Lua

**Files:**
- Modify: `src/config_lua.c`, `test/test_config_lua.c`
- Create: `test/fixtures/three-ways/lua/daukle.lua`, `test/fixtures/three-ways/lua/package.json`, `test/fixtures/lua-plugin/daukle.toml`, `test/fixtures/lua-plugin/daukle.lua`, `test/fixtures/lua-plugin/deps.txt`
- Modify: `test/test_e2e.c`

**Interfaces:**
- Consumes: `fr_registry_add_language`, `fr_registry_add_source`, `fr_lua_to_json`, `fr_lua_push_json`.
- Produces: the `daukle.language{...}` and `daukle.source{...}` registration functions, backed by the two adapters `lua_language_apply` and `lua_source_load`.

- [ ] **Step 1: Write the fixtures**

`test/fixtures/lua-plugin/daukle.toml`:

```toml
schema = 1
project = "forebay/plugin"
version = "1.0.0"

[modules]

[sources."forebay/basekit"]
kind = "path"
path = "../producer"

[[consumers]]
id = "text"
language = "plaintext"
file = "deps.txt"
configuration = "deps"

  [consumers.dependencies."forebay/basekit"]
  version = "^5.0.0"
  modules = ["ir"]
```

`test/fixtures/lua-plugin/daukle.lua`:

```lua
daukle.language{
  name = "plaintext",
  apply = function(consumer, resolved, text)
    local lines = {}
    for index = 1, #resolved do
      lines[index] = resolved[index].project .. "/" .. resolved[index].module
    end
    return table.concat(lines, "\n") .. "\n"
  end,
}
```

`test/fixtures/lua-plugin/deps.txt`: an empty file.

- [ ] **Step 2: Write the failing test**

Add to `test/test_config_lua.c`:

```c
TEST a_script_registers_a_language_plugin(void) {
    fr_error err;
    fr_registry *registry = NULL;
    fr_build_registry(&registry, &err);
    fr_manifest manifest;
    ASSERT_EQ(FR_OK, fr_config_load_file("test/fixtures/lua-plugin/daukle.toml",
                                         registry, &manifest, &err));
    ASSERT(fr_registry_language(registry, "daukle.language/plaintext") != NULL);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST a_script_may_not_take_over_a_built_in_capability(void) {
    fr_error err;
    fr_registry *registry = NULL;
    fr_build_registry(&registry, &err);
    fr_manifest manifest;
    ASSERT_EQ(FR_ERR, fr_config_load_file("test/fixtures/lua-collide/daukle.toml",
                                          registry, &manifest, &err));
    ASSERT(strstr(err.message, "npm") != NULL);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}
```

Create `test/fixtures/lua-collide/daukle.toml` as a copy of the lua-overlay one, and `test/fixtures/lua-collide/daukle.lua`:

```lua
daukle.language{ name = "npm", apply = function() return "" end }
```

Register both tests in `main`.

- [ ] **Step 3: Run it and watch it fail**

Run: `ctest --test-dir build -C Release -R test_config_lua --output-on-failure`
Expected: FAIL, `attempt to call a nil value (field 'language')`.

- [ ] **Step 4: Implement the adapters**

Add to `src/config_lua.c`, above `publish_daukle_table`:

```c
#define FR_LUA_MAX_PLUGINS 32

typedef struct {
    char *capability;
    int callback;
} fr_lua_plugin_slot;

static fr_lua_plugin_slot plugin_slots[FR_LUA_MAX_PLUGINS];
static size_t plugin_slot_count;
static fr_registry *registering_into;

static fr_lua_plugin_slot *slot_for(const char *capability) {
    for (size_t index = 0; index < plugin_slot_count; index++) {
        if (strcmp(plugin_slots[index].capability, capability) == 0) return &plugin_slots[index];
    }
    return NULL;
}

static int lua_language_apply(void *state, const fr_consumer *consumer,
                              const fr_resolved *resolved, size_t count,
                              const char *original_text, char **out_text, fr_error *err) {
    fr_lua_plugin_slot *slot = state;
    lua_rawgeti(runtime_state, LUA_REGISTRYINDEX, slot->callback);

    lua_newtable(runtime_state);
    lua_pushstring(runtime_state, consumer->id);
    lua_setfield(runtime_state, -2, "id");
    lua_pushstring(runtime_state, consumer->configuration);
    lua_setfield(runtime_state, -2, "configuration");

    lua_newtable(runtime_state);
    for (size_t index = 0; index < count; index++) {
        lua_newtable(runtime_state);
        lua_pushstring(runtime_state, resolved[index].project);
        lua_setfield(runtime_state, -2, "project");
        lua_pushstring(runtime_state, resolved[index].module);
        lua_setfield(runtime_state, -2, "module");
        if (fr_lua_push_json(runtime_state, resolved[index].block, err) != FR_OK) return FR_ERR;
        lua_setfield(runtime_state, -2, "block");
        lua_rawseti(runtime_state, -2, (lua_Integer) index + 1);
    }

    lua_pushstring(runtime_state, original_text);

    if (lua_pcall(runtime_state, 3, 1, 0) != LUA_OK) {
        fr_error_set(err, "%s", lua_tostring(runtime_state, -1));
        lua_pop(runtime_state, 1);
        return FR_ERR;
    }
    const char *produced = lua_tostring(runtime_state, -1);
    if (produced == NULL) {
        fr_error_set(err, "the \"%s\" plugin returned %s, expected a string",
                     slot->capability, luaL_typename(runtime_state, -1));
        lua_pop(runtime_state, 1);
        return FR_ERR;
    }
    size_t length = strlen(produced) + 1;
    *out_text = malloc(length);
    if (*out_text == NULL) {
        fr_error_set(err, "out of memory taking the output of \"%s\"", slot->capability);
        lua_pop(runtime_state, 1);
        return FR_ERR;
    }
    memcpy(*out_text, produced, length);
    lua_pop(runtime_state, 1);
    return FR_OK;
}

static int lua_source_load(void *state, const char *project, const cJSON *block,
                           const char *base_dir, fr_project *out, fr_error *err) {
    fr_lua_plugin_slot *slot = state;
    lua_rawgeti(runtime_state, LUA_REGISTRYINDEX, slot->callback);
    lua_pushstring(runtime_state, project);
    if (fr_lua_push_json(runtime_state, block, err) != FR_OK) return FR_ERR;
    lua_pushstring(runtime_state, base_dir);

    if (lua_pcall(runtime_state, 3, 1, 0) != LUA_OK) {
        fr_error_set(err, "%s", lua_tostring(runtime_state, -1));
        lua_pop(runtime_state, 1);
        return FR_ERR;
    }
    cJSON *document = NULL;
    int status = fr_lua_to_json(runtime_state, -1, &document, err);
    lua_pop(runtime_state, 1);
    if (status != FR_OK) return FR_ERR;

    char *text = cJSON_PrintUnformatted(document);
    cJSON_Delete(document);
    if (text == NULL) {
        fr_error_set(err, "out of memory reading the project \"%s\" produced", project);
        return FR_ERR;
    }
    status = fr_project_parse(text, project, out, err);
    free(text);
    return status;
}

static int take_slot(lua_State *state, const char *prefix, const char *field, fr_error *err,
                     fr_lua_plugin_slot **out) {
    lua_getfield(state, 1, "name");
    const char *name = lua_tostring(state, -1);
    if (name == NULL) return luaL_error(state, "a %s needs a name", prefix);

    if (plugin_slot_count == FR_LUA_MAX_PLUGINS) {
        return luaL_error(state, "too many plugins declared in one configuration");
    }

    char capability[128];
    snprintf(capability, sizeof capability, "%s%s", prefix, name);
    lua_pop(state, 1);

    if (slot_for(capability) != NULL) {
        return luaL_error(state, "\"%s\" is declared twice", capability);
    }

    fr_lua_plugin_slot *slot = &plugin_slots[plugin_slot_count];
    size_t length = strlen(capability) + 1;
    slot->capability = malloc(length);
    if (slot->capability == NULL) return luaL_error(state, "out of memory");
    memcpy(slot->capability, capability, length);

    lua_getfield(state, 1, field);
    if (!lua_isfunction(state, -1)) {
        free(slot->capability);
        return luaL_error(state, "\"%s\" needs a %s function", capability, field);
    }
    slot->callback = luaL_ref(state, LUA_REGISTRYINDEX);
    plugin_slot_count++;
    (void) err;
    *out = slot;
    return 0;
}

static int lua_declare_language(lua_State *state) {
    luaL_checktype(state, 1, LUA_TTABLE);
    fr_lua_plugin_slot *slot = NULL;
    take_slot(state, "daukle.language/", "apply", NULL, &slot);

    fr_language_plugin plugin = { slot->capability, lua_language_apply, slot };
    fr_error err;
    if (fr_registry_add_language(registering_into, &plugin, &err) != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }
    return 0;
}

static int lua_declare_source(lua_State *state) {
    luaL_checktype(state, 1, LUA_TTABLE);
    fr_lua_plugin_slot *slot = NULL;
    take_slot(state, "daukle.source/", "load", NULL, &slot);

    fr_source_plugin plugin = { slot->capability, lua_source_load, slot };
    fr_error err;
    if (fr_registry_add_source(registering_into, &plugin, &err) != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }
    return 0;
}
```

Publish both in `publish_daukle_table`, beside `env` and `log`:

```c
    lua_pushcfunction(state, lua_declare_language);
    lua_setfield(state, -2, "language");
    lua_pushcfunction(state, lua_declare_source);
    lua_setfield(state, -2, "source");
```

Set `registering_into = registry;` at the top of `config_lua_load` and stop ignoring the parameter. Free the slots in `fr_lua_runtime_shutdown`:

```c
void fr_lua_runtime_shutdown(void) {
    if (runtime_state == NULL) return;
    fr_lua_close(runtime_state);
    runtime_state = NULL;
    for (size_t index = 0; index < plugin_slot_count; index++) free(plugin_slots[index].capability);
    plugin_slot_count = 0;
    registering_into = NULL;
}
```

Add `#include "manifest.h"` and `#include "resolve.h"` to `src/config_lua.c` for `fr_project_parse` and `fr_resolved`.

- [ ] **Step 5: Run the tests**

Run: `ctest --test-dir build -C Release -R test_config_lua --output-on-failure`
Expected: PASS, 4 tests. The collision test relies on `fr_registry_add_language` rejecting a duplicate capability, which it already does; the Lua function turns that `fr_error` into a Lua error naming `npm`.

- [ ] **Step 6: Finish the three-ways equivalence**

`test/fixtures/three-ways/lua/daukle.lua`:

```lua
daukle.config = {
  schema = 1,
  project = "forebay/three-ways",
  version = "1.0.0",
  modules = {},
  sources = { ["forebay/basekit"] = { kind = "path", path = "../producer" } },
  consumers = {
    {
      id = "stub",
      language = "npm",
      file = "package.json",
      configuration = "dependencies",
      dependencies = { ["forebay/basekit"] = { version = "^5.0.0", modules = { "ir" } } },
    },
  },
}
```

Copy the same `package.json` starting point into `test/fixtures/three-ways/lua/`. Extend the Task 5 test in `test/test_e2e.c` to run a third sync against `test/fixtures/three-ways/lua/daukle.lua` and assert its `package.json` equals the other two:

```c
    ASSERT_EQ(FR_OK, fr_sync("test/fixtures/three-ways/lua/daukle.lua", 1, 0, &report, &err));
    fr_sync_report_free(&report);
    char *from_lua = NULL;
    ASSERT_EQ(FR_OK, fr_file_read_text("test/fixtures/three-ways/lua/package.json", &from_lua, &err));
    ASSERT_STR_EQ(from_json, from_lua);
    free(from_lua);
```

Rename the test to `the_same_manifest_in_three_formats_writes_the_same_file` and update its registration in `main`.

- [ ] **Step 7: Run the whole suite**

Run: `ctest --test-dir build -C Release --output-on-failure`
Expected: PASS.

- [ ] **Step 8: Commit**

```bash
git add src/config_lua.c test/test_config_lua.c test/test_e2e.c test/fixtures
git commit -m "feat(lua): register source and language plugins declared in a script"
```

---

### Task 12: Splice edits into TOML

**Files:**
- Create: `src/tomledit.c`, `src/tomledit.h`, `test/test_tomledit.c`

**Interfaces:**
- Produces:
  - `int fr_toml_edit_set_dependency(const char *text, const char *consumer_id, const char *project, const char *range, const char *const *modules, size_t module_count, char **out, fr_error *err)`

One function, because `daukle add` needs exactly one operation: insert a dependency table for a consumer, or update the `version` of the one already there.

- [ ] **Step 1: Write the failing test**

Create `test/test_tomledit.c`:

```c
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

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(updates_the_range_of_a_dependency_already_there);
    RUN_TEST(appends_a_dependency_that_was_not_there);
    RUN_TEST(reports_a_consumer_that_does_not_exist);
    GREATEST_MAIN_END();
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `cmake --build build --config Release`
Expected: FAIL, `tomledit.h` not found.

- [ ] **Step 3: Implement**

`src/tomledit.h`:

```c
#ifndef DAUKLE_TOMLEDIT_H
#define DAUKLE_TOMLEDIT_H

#include "types.h"

#include <stddef.h>

/* Edits toml by splicing spans of its text, so comments, ordering and spacing
   outside the edited span survive. jsonedit.c does the same for json and the
   two stay separate: only the span finding differs, and it has nothing in
   common between the two grammars. */
int fr_toml_edit_set_dependency(const char *text, const char *consumer_id, const char *project,
                                const char *range, const char *const *modules, size_t module_count,
                                char **out, fr_error *err);

#endif
```

`src/tomledit.c`:

```c
#include "tomledit.h"

#include "error.h"
#include "strbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *line_after(const char *cursor) {
    const char *newline = strchr(cursor, '\n');
    return newline == NULL ? cursor + strlen(cursor) : newline + 1;
}

static int line_starts_with(const char *line, const char *prefix) {
    while (*line == ' ' || *line == '\t') line++;
    return strncmp(line, prefix, strlen(prefix)) == 0;
}

static const char *find_consumer(const char *text, const char *consumer_id) {
    char needle[256];
    snprintf(needle, sizeof needle, "id = \"%s\"", consumer_id);
    for (const char *cursor = text; *cursor != '\0'; cursor = line_after(cursor)) {
        if (line_starts_with(cursor, needle)) return cursor;
    }
    return NULL;
}

/* The consumer's span runs to the next [[consumers]] header, because every
   dependency table between the two belongs to it. */
static const char *end_of_consumer(const char *from) {
    for (const char *cursor = line_after(from); *cursor != '\0'; cursor = line_after(cursor)) {
        if (line_starts_with(cursor, "[[")) return cursor;
    }
    return from + strlen(from);
}

static const char *find_dependency(const char *from, const char *until, const char *project) {
    char needle[256];
    snprintf(needle, sizeof needle, "[consumers.dependencies.\"%s\"]", project);
    for (const char *cursor = from; cursor < until && *cursor != '\0'; cursor = line_after(cursor)) {
        if (line_starts_with(cursor, needle)) return cursor;
    }
    return NULL;
}

static const char *find_version_line(const char *from, const char *until) {
    for (const char *cursor = line_after(from); cursor < until && *cursor != '\0';
         cursor = line_after(cursor)) {
        if (line_starts_with(cursor, "version = ")) return cursor;
        if (line_starts_with(cursor, "[")) return NULL;
    }
    return NULL;
}

static void append_modules(fr_strbuf *buffer, const char *const *modules, size_t module_count) {
    fr_strbuf_append(buffer, "  modules = [");
    for (size_t index = 0; index < module_count; index++) {
        if (index > 0) fr_strbuf_append(buffer, ", ");
        fr_strbuf_append_format(buffer, "\"%s\"", modules[index]);
    }
    fr_strbuf_append(buffer, "]\n");
}

int fr_toml_edit_set_dependency(const char *text, const char *consumer_id, const char *project,
                                const char *range, const char *const *modules, size_t module_count,
                                char **out, fr_error *err) {
    *out = NULL;
    const char *consumer = find_consumer(text, consumer_id);
    if (consumer == NULL) {
        fr_error_set(err, "no consumer with id \"%s\"", consumer_id);
        return FR_ERR;
    }
    const char *consumer_end = end_of_consumer(consumer);
    const char *dependency = find_dependency(consumer, consumer_end, project);

    fr_strbuf buffer;
    fr_strbuf_init(&buffer);

    if (dependency != NULL) {
        const char *version_line = find_version_line(dependency, consumer_end);
        if (version_line == NULL) {
            fr_error_set(err, "\"%s\" under \"%s\" has no version line", project, consumer_id);
            fr_strbuf_free(&buffer);
            return FR_ERR;
        }
        fr_strbuf_append_bytes(&buffer, text, (size_t) (version_line - text));
        fr_strbuf_append_format(&buffer, "  version = \"%s\"\n", range);
        fr_strbuf_append(&buffer, line_after(version_line));
    } else {
        fr_strbuf_append_bytes(&buffer, text, (size_t) (consumer_end - text));
        fr_strbuf_append_format(&buffer, "\n  [consumers.dependencies.\"%s\"]\n", project);
        fr_strbuf_append_format(&buffer, "  version = \"%s\"\n", range);
        append_modules(&buffer, modules, module_count);
        fr_strbuf_append(&buffer, consumer_end);
    }

    *out = fr_strbuf_release(&buffer);
    if (*out == NULL) {
        fr_error_set(err, "out of memory editing the manifest");
        return FR_ERR;
    }
    return FR_OK;
}
```

The buffer's failure is sticky by design (see the comment in `src/strbuf.h`), which is why no append above is checked: `fr_strbuf_release` returns NULL if any append along the way could not allocate, and the single NULL check below is the whole error path.

- [ ] **Step 4: Run the tests**

Run: `ctest --test-dir build -C Release -R test_tomledit --output-on-failure`
Expected: PASS, 3 tests.

- [ ] **Step 5: Update STRUCTURE.md**

```
| rewriting a toml manifest in place | `tomledit.c` | a toml parser. It splices spans, as `jsonedit.c` does for json |
```

- [ ] **Step 6: Commit**

```bash
git add src/tomledit.c src/tomledit.h test/test_tomledit.c STRUCTURE.md
git commit -m "feat(tomledit): splice a dependency into a toml manifest"
```

---

### Task 13: The CLI surface

**Files:**
- Modify: `src/cli.c`, `src/cli.h`, `src/main.c`, `test/test_cli.c`

**Interfaces:**
- Consumes: `fr_config_find`, `fr_config_load_file`, `fr_toml_edit_set_dependency`, `fr_build_registry`.
- Produces: `FR_CLI_ADD` and `FR_CLI_CONFIG_PRINT` commands, and the fields `add_project`, `add_range`, `add_consumer`, `add_modules`, `verbose`, `instruction_limit`, `memory_limit` on `fr_cli_options`.

- [ ] **Step 1: Write the failing test**

Add to `test/test_cli.c`:

```c
TEST parses_an_add_command(void) {
    char *argv[] = { "daukle", "add", "forebay/basekit@^5.0.0", "--to", "stub", "--modules", "ir,core" };
    fr_cli_options options;
    fr_cli_parse(7, argv, &options);
    ASSERT_EQ(FR_CLI_ADD, options.command);
    ASSERT_STR_EQ("forebay/basekit", options.add_project);
    ASSERT_STR_EQ("^5.0.0", options.add_range);
    ASSERT_STR_EQ("stub", options.add_consumer);
    ASSERT_STR_EQ("ir,core", options.add_modules);
    PASS();
}

TEST rejects_an_add_without_a_range(void) {
    char *argv[] = { "daukle", "add", "forebay/basekit", "--to", "stub" };
    fr_cli_options options;
    fr_cli_parse(5, argv, &options);
    ASSERT_EQ(FR_CLI_USAGE, options.command);
    PASS();
}

TEST rejects_an_add_without_a_consumer(void) {
    char *argv[] = { "daukle", "add", "forebay/basekit@^5.0.0" };
    fr_cli_options options;
    fr_cli_parse(3, argv, &options);
    ASSERT_EQ(FR_CLI_USAGE, options.command);
    PASS();
}

TEST parses_config_print(void) {
    char *argv[] = { "daukle", "config", "print" };
    fr_cli_options options;
    fr_cli_parse(3, argv, &options);
    ASSERT_EQ(FR_CLI_CONFIG_PRINT, options.command);
    PASS();
}

TEST leaves_the_manifest_path_unset_so_the_directory_is_searched(void) {
    char *argv[] = { "daukle", "sync" };
    fr_cli_options options;
    fr_cli_parse(2, argv, &options);
    ASSERT_EQ(FR_CLI_SYNC, options.command);
    ASSERT(options.manifest_path == NULL);
    PASS();
}
```

Register all five in `main`. The last one changes existing behaviour: the default was the literal `"daukle.json"`. Check `test/test_cli.c` for a test asserting that default and update it, since searching the directory is now the point.

- [ ] **Step 2: Run it and watch it fail**

Run: `cmake --build build --config Release`
Expected: FAIL, `FR_CLI_ADD` undefined.

- [ ] **Step 3: Extend the options**

`src/cli.h`:

```c
typedef enum {
    FR_CLI_SYNC,
    FR_CLI_CHECK,
    FR_CLI_ADD,
    FR_CLI_CONFIG_PRINT,
    FR_CLI_VERSION,
    FR_CLI_USAGE
} fr_cli_command;

typedef struct {
    fr_cli_command command;
    const char *manifest_path;
    int use_cache;
    int verbose;
    long instruction_limit;
    size_t memory_limit;
    const char *add_project;
    const char *add_range;
    const char *add_consumer;
    const char *add_modules;
} fr_cli_options;
```

Add `#include <stddef.h>`.

- [ ] **Step 4: Extend the parser**

In `src/cli.c`, set `out->manifest_path = NULL;` instead of the `daukle.json` default and delete the `DEFAULT_MANIFEST` constant. Add the new options to the loop before the `argument[0] == '-'` rejection:

```c
        } else if (strcmp(argument, "--verbose") == 0) {
            out->verbose = 1;
        } else if (strcmp(argument, "--to") == 0 && index + 1 < argc) {
            out->add_consumer = argv[++index];
        } else if (strcmp(argument, "--modules") == 0 && index + 1 < argc) {
            out->add_modules = argv[++index];
        } else if (strcmp(argument, "--lua-instruction-limit") == 0 && index + 1 < argc) {
            out->instruction_limit = strtol(argv[++index], NULL, 10);
        } else if (strcmp(argument, "--lua-memory-limit") == 0 && index + 1 < argc) {
            out->memory_limit = (size_t) strtoul(argv[++index], NULL, 10);
```

and after the existing `sync` and `check` dispatch:

```c
    else if (strcmp(command, "add") == 0) {
        if (manifest_path == NULL || out->add_consumer == NULL) return;
        char *at = strchr(manifest_path, '@');
        if (at == NULL) return;
        *at = '\0';
        out->add_project = manifest_path;
        out->add_range = at + 1;
        out->command = FR_CLI_ADD;
        return;
    }
    else if (strcmp(command, "config") == 0) {
        if (manifest_path == NULL || strcmp(manifest_path, "print") != 0) return;
        out->command = FR_CLI_CONFIG_PRINT;
        return;
    }
```

`add` reuses the second positional slot, and writes a NUL into `argv` to split `project@range`. `argv` is writable by the C standard, and the test passes a writable array, so this is safe; note it in one line beside the split.

Initialise the new fields at the top of `fr_cli_parse`: `out->verbose = 0; out->instruction_limit = 0; out->memory_limit = 0; out->add_project = NULL; out->add_range = NULL; out->add_consumer = NULL; out->add_modules = NULL;`. Add `#include <stdlib.h>`.

- [ ] **Step 5: Wire the commands in `main.c`**

Add a `resolve_manifest_path` helper that calls `fr_config_find(".", registry, &path, &err)` when `options.manifest_path` is NULL, and hand the result to `fr_sync`. Add the two new cases:

```c
        case FR_CLI_CONFIG_PRINT:
            return print_config(options.manifest_path);
        case FR_CLI_ADD:
            return add_dependency(&options);
```

`print_config` builds the registry with `fr_build_registry`, loads through `fr_config_load_file`, prints `cJSON_Print(manifest.document)` to stdout, frees and shuts the Lua runtime down. `add_dependency` reads the manifest file's text, splits `options.add_modules` on commas into a `char **`, calls `fr_toml_edit_set_dependency`, and writes the result back with `fr_file_write_text`. If the resolved manifest does not end in `.toml`, it fails with `daukle add edits daukle.toml; this project uses "<path>"`.

Update the usage line:

```c
    fprintf(stderr, "usage: daukle [--version | sync [manifest] | check [manifest]"
                    " | add <project>@<range> --to <consumer> [--modules a,b]"
                    " | config print] [--no-cache] [--verbose]\n");
```

- [ ] **Step 6: Run the whole suite**

Run: `ctest --test-dir build -C Release --output-on-failure`
Expected: PASS. Then exercise it by hand:

```bash
./build/Release/daukle.exe config print test/fixtures/lua-overlay/daukle.toml
```

Expected: the post Lua document, with `"version": "2.0.0"`.

- [ ] **Step 7: Commit**

```bash
git add src/cli.c src/cli.h src/main.c test/test_cli.c
git commit -m "feat(cli): add the add and config print commands"
```

---

### Task 14: Close the agnostic core rule

**Files:**
- Modify: `cmake/check_agnostic.cmake`, `STRUCTURE.md`

**Interfaces:**
- Consumes: every module added by this plan.

- [ ] **Step 1: Watch the check fail on purpose**

Add `config.c` and `config.h` to `CORE_FILES` and `"daukle\\.config/[a-z]"` to `FORBIDDEN` in `cmake/check_agnostic.cmake`:

```cmake
set(CORE_FILES
    resolve.c resolve.h manifest.c manifest.h registry.c registry.h
    types.h sync.c sync.h main.c cli.c cli.h config.c config.h)

set(FORBIDDEN "\"gradle\"" "\"path\"" "\"npm\"" "daukle\\.source/[a-z]"
              "daukle\\.language/[a-z]" "daukle\\.config/[a-z]")
```

Run: `ctest --test-dir build -C Release -R agnostic_core --output-on-failure`
Expected: FAIL, because `config.c` builds the capability string `"daukle.config/%s"` and `sync.c` names none of the formats but `config.c`'s `snprintf` matches the pattern.

- [ ] **Step 2: Make it pass without weakening it**

The pattern must catch a hardcoded format and not the generic construction. Change `plugin_for` in `src/config.c` to build the prefix from a constant that does not match a lowercase letter after the slash:

```c
#define FR_CONFIG_PREFIX "daukle.config/"

    char capability[128];
    snprintf(capability, sizeof capability, FR_CONFIG_PREFIX "%s", file_path_extension);
```

`daukle.config/` followed by `"` does not match `daukle\.config/[a-z]`, so the generic form passes and a hardcoded `"daukle.config/toml"` in core would not. Apply the same treatment to any other core site that builds a capability, and confirm `sync.c` still names only the `FR_CONFIG_*` symbols.

Run: `ctest --test-dir build -C Release -R agnostic_core --output-on-failure`
Expected: PASS.

- [ ] **Step 3: Final pass over STRUCTURE.md**

Confirm every module this plan added has exactly one row in section 2 and that section 1's tree lists `vendor/toml` and `vendor/lua`. Update section 4, "State", to say that the config surface is TOML with an optional Lua layer, that published manifest assets remain JSON, and that F-7 in `spisor/docs/TASKS.md` is unaffected.

- [ ] **Step 4: Run the whole suite one last time**

Run: `ctest --test-dir build -C Release --output-on-failure`
Expected: PASS, every test.

- [ ] **Step 5: Commit**

```bash
git add cmake/check_agnostic.cmake src/config.c STRUCTURE.md
git commit -m "chore(config): hold the agnostic core rule over the format seam"
```

---

## Self-Review

**Spec coverage:**

| spec section | task |
| --- | --- |
| 2.1 one validator, three front ends | 1, 3, 4, 10 |
| 2.2 the schema does not change | 5 (equivalence), 11 (three formats) |
| 3.1 `config.c`, the format seam and search order | 3, extended in 4 Step 8 |
| 3.1 registry before manifest | 3 Step 6 |
| 3.2 `config_toml.c` and the type mapping | 4 |
| 3.3 `luax.c`, `config_lua.c` | 6, 7, 8, 10 |
| 3.4 the sandbox, caps, `daukle.include` | 9 |
| 3.5 the Lua plugin API and its adapters | 11 |
| 3.6 `daukle.host` and `daukle.env` | 10 Step 4 |
| 3.7 `tomledit.c` | 12 |
| 5 errors and `--verbose` | 6 Step 5, 13 |
| 6 CLI | 13 |
| 7 build, vendoring, agnostic check | 4 Step 2, 6 Step 2, 14 |
| 8 migration, JSON keeps working | 3 (JSON plugin), 5 (equivalence) |
| 9 testing, the equivalence fixture | 5, 11 Step 6, and one test file per module throughout |

Two spec items are deliberately thinner in the plan than in the spec, and both are noted where they land: the `daukle.json` deprecation notice (spec 3.1 case 2) is not implemented, because nothing in the plan makes JSON second class and printing a notice before `daukle migrate` exists would be noise; and `--verbose` prints the untruncated traceback only if `fr_lua_run` is extended to hand it back, which Task 13 Step 5 does not require. Both are follow-on work, not gaps in the tasks as written.

**Type consistency:** `fr_config_plugin.load` keeps one signature from Task 2 through Task 11. `fr_manifest_from_document` takes ownership in every outcome, which Tasks 3 and 10 rely on. `fr_lua_push_json` and `fr_lua_to_json` keep their signatures from Tasks 7 and 8 into Task 11's adapters. `fr_build_registry` is named the same in Tasks 3, 10, 11 and 13.

**Known adaptation point**, flagged rather than hidden: the tomlc99 accessor names (Task 4 Step 1) must be checked against the vendored header before the walk in that step compiles. Every other API the plan calls is either defined by an earlier task or verified against the headers in `src/` as they stand today.
