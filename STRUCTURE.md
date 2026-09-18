# Structure: daukle

A cross-language package coordinate resolver, in C11. It reads a manifest, resolves each project it
names from a source, and writes that project's dependency coordinates into a consumer's build file
in the form that consumer's language expects.

**This file exists to stop a second module being written for a job one module already has.** Read
section 2 before adding a source, a language or a helper: every row names the thing that already
owns that concern, and adding a source or a language means registering a plugin, never touching the
resolver.

Living document. Update it in the same commit as the change it describes.

Verified against disk on 2026-09-18.

---

## 1. Where it lives

`F:/Documents/GitHub/daukle/daukle`, its own org folder since 2026-09-17, because more related repos
are expected beside it. The git remote is still `github.com/intisy/daukle`: the folder tier and the
GitHub organisation are independent, as they are for the rest of this tree.

It has been renamed three times. It was `ferrule`, then `tiestone`, then `terko`, then `daukle`, and
anything naming the first three means this repo.

```
daukle/daukle
  CMakeLists.txt        C11, warnings are errors (/W4 /WX, -Wall -Wextra -Werror)
  src/                  the whole program, one concern per pair of .c/.h files
  test/                 one test file per src module, plus a real HTTP server fixture
  vendor/cJSON          JSON parsing
  vendor/toml           TOML parsing
  vendor/lua            the lua 5.4 library, no standalone interpreter
  vendor/greatest       the test harness
  cmake/  build/        build machinery and output
  plugin.json           id daukle, category tool, tech c
```

`docs/` is gitignored except for `docs/superpowers/`, which `.gitignore` re-includes: the design
spec and its plan are tracked and are part of the deliverable. This file still sits at the repo root
rather than under `docs/`, because it is the map a contributor reads first.

**It is a TOOL, which means it is outside every ecosystem and terminal.** Nothing may reference it.
It is invoked, never linked. That is why it may carry whatever dependencies it likes and owes none of
the client tier's rules.

## 2. The ownership index

| concern | owned by | it is NOT |
| --- | --- | --- |
| reading a project or manifest file into structs | `manifest.c` | parsing JSON, which is `jsonx` over vendored cJSON |
| choosing which module of which project answers a coordinate | `resolve.c` | fetching anything. It resolves, the source fetches |
| the three plugin tables, source, language and config | `registry.c` | a plugin. It holds `fr_source_plugin`, `fr_language_plugin` and `fr_config_plugin` |
| fetching a project from GitHub releases | `source_github.c` | the only possible source. It is one registered `fr_source_plugin` |
| taking a project from a local path | `source_path.c` | a fallback for GitHub. It is a peer source |
| emitting npm, Gradle and C coordinates | `lang_npm.c`, `lang_gradle.c`, `lang_c.c` | resolvers. Each is one registered `fr_language_plugin` |
| finding the manifest and choosing its format | `config.c` | a parser. It dispatches to a registered `fr_config_plugin` |
| reading a json manifest | `config_json.c` | the only possible format. It is one registered `fr_config_plugin` |
| reading a toml manifest | `config_toml.c` | a json reader. It is a peer format, registered the same way |
| rewriting a marked region of a file in place | `region.c` | a JSON editor. `jsonedit.c` is, for files that are JSON |
| rewriting a toml manifest in place | `tomledit.c` | a toml parser. It splices spans, as `jsonedit.c` does for json |
| the whole write pass over a manifest | `sync.c` | per-language. It drives the language plugins |
| HTTP, per platform | `http.c` over `http_curl.c` and `http_winhttp.c` | two implementations to keep in step. One interface, one backend per platform |
| caching a resolved artifact | `cache.c` | keyed by project and version alone. The artifact string is part of the key, deliberately |
| version ranges and ordering | `semver.c` | date or tag ordering |
| URL building and escaping | `url.c` | HTTP |
| growable strings | `strbuf.c` | a general container library |
| the command surface | `cli.c`, `main.c` | logic. Everything it calls lives in a module beside it |
| errors | `error.c`, `fr_error` | logging. The caller decides what to print |
| the lua state, its memory cap and its errors | `luax.c` | the sandbox, which is `lua_sandbox.c`, nor the config format, which is `config_lua.c` |
| what a configuration script may touch | `lua_sandbox.c` | a permission system. It curates one globals table and bounds daukle.include |
| verifying a spliced manifest still parses | `tomledit.c`, through `FR_CONFIG_TOML` | a second parser. It reads its own output back so no caller is handed text it would be wrong to write |
| running a configuration script, reading it back, and registering any source or language plugin the script declares | `config_lua.c` | the sandbox or the lua state, which are `lua_sandbox.c` and `luax.c` |

**A `daukle.lua` runs against a curated globals table, not Lua's own.** The two lists that define it
are `KEPT` and `REMOVED` at the top of `src/lua_sandbox.c`, and reading a removed name raises an
error naming it rather than returning nil. Section 3.4 of the design spec explains each removal;
the source lists are the authority and the spec follows them.

**The registry is destroyed before the lua runtime is shut down, always.** A `daukle.lua` may
register plugins, and `config_lua.c` owns their capability strings while the registry stores the
plugin structs by value. So the order is: build the registry, load the configuration, destroy the
registry, then `fr_lua_runtime_shutdown()`. `config_lua_load` refuses a second load while a registry
still holds the previous one's plugins rather than freeing what that registry would go on reading.

**Adding a source means adding an `fr_source_plugin` and registering it.** Adding a language means
adding an `fr_language_plugin` and registering it. Neither touches `resolve.c`, and a change that
does touch it for a new source or language is the signal that the seam was bypassed.

**The cache key includes the artifact, and that is load bearing.** Two manifests can name one project
id and one version and resolve them from different repositories. Keyed by project and version alone,
the cache serves one for the other and emits silently wrong coordinates.

## 3. Tests

One test file per src module, under `test/`, on the `greatest` harness. Beyond the per-module tests
there are `test_e2e.c` and `test_e2e_languages.c` for whole-pass behaviour, and `test_http.c`,
`test_network.c` and `test_redirect.c` run against `test/http_server.c`, a real local server rather
than a mock, so redirect and transport behaviour is exercised as it will be in use.

`test/fixtures/` holds consumer build files. Several are gitignored, because they are written by the
tests themselves.

`config_json.c` is the one module with no dedicated test file: it is exercised through
`test_config.c`'s json-manifest test instead.

## 4. State

The resolver, the two sources, the three languages, the cache, the sync pass and the CLI are all
implemented and tested. The configuration surface is TOML with an optional Lua overlay: `config.c`
finds the manifest and dispatches to a registered `fr_config_plugin` by a capability string built
from the file's extension, and `config_json.c`, `config_toml.c` and `config_lua.c` are all
registered formats, chosen the same way, with none named in `config.c` itself. TOML is the format a
manifest is authored in going forward; JSON keeps working, both as a manifest format and as the
asset a source publishes, since `source_github.c` still fetches `daukle.json` by default from a
release. Task **F-7** in `spisor/docs/TASKS.md` is unaffected by this work: it says both of the
phase-2 manifest writers are proven against fixtures only, because nothing in the ecosystem
publishes a manifest release asset yet. basekit's 5.0.0 release carries eight jars and no manifest.
Until something publishes one, the writers have never met real input.

`daukle add` edits TOML only, and `main.c` names that format on purpose: it checks the manifest path
ends in `.toml` and calls `fr_toml_edit_set_dependency` directly, which is a deliberate, accepted
exception to the agnostic-core rule (ruling R29). The general form would be an edit hook on
`fr_config_plugin`, so every format supplies its own editor and `main.c` names none of them, but that
would touch a plugin struct four completed tasks already depend on and would buy nothing until a JSON
or Lua manifest needs editing in place too, so it is deferred rather than built now.

`daukle add` appends a dependency that is not there yet and updates the version of one that is. It
does not rewrite an existing module list, so `--modules` for a dependency that already exists is an
error rather than a silent discard, and whatever it produces is read back through the TOML reader
before anything is written.
