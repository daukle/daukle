# Structure: daukle

A cross-language package coordinate resolver, in C11. It reads a manifest, resolves each project it
names from a source, and writes that project's dependency coordinates into a consumer's build file
in the form that consumer's language expects.

**This file exists to stop a second module being written for a job one module already has.** Read
section 2 before adding a source, a language or a helper: every row names the thing that already
owns that concern, and adding a source or a language means registering a plugin, never touching the
resolver.

Living document. Update it in the same commit as the change it describes.

Verified against disk on 2026-09-29.

---

## 1. Where it lives

`F:/Documents/GitHub/daukle/daukle`, its own org folder since 2026-09-17, because more related repos
are expected beside it. The git remote is `github.com/daukle/daukle`, moved out of the `intisy`
organisation on 2026-09-18 so that the folder tier and the GitHub organisation agree. daukle is its
own org: it keeps its own queue and its own docs repo, and spisor merely happens to use it.

`daukle/docs` beside this repo is that docs repo, holding the task queue and the design documents.
Nothing under `docs/` is ever tracked inside a code repo.

It has been renamed three times. It was `ferrule`, then `tiestone`, then `terko`, then `daukle`, and
anything naming the first three means this repo.

```
daukle/daukle
  CMakeLists.txt        C11, warnings are errors (/W4 /WX, -Wall -Wextra -Werror)
  src/                  the whole program, one concern per pair of .c/.h files
    cli/                the entry point and command dispatch
    config/             the authoring surface: toml, lua and json manifests, and the editors for each
    lua/                the lua state, the configuration sandbox, and the per-plugin verb environment
    plugin/             acquiring, resolving and registering plugins, and the dependencies between them
    net/                http and urls, one backend per platform
    archive/            recognising, reading and unpacking tar, tar.gz and zip
    exec/               starting a program, resolving a tool, and reporting which one was used
    provision/          fetching a pinned toolchain into the content-addressed store
    project/            what daukle does to a repository: generation, the derived tree, tasks and sync
    cache/              where everything above puts bytes on disk, and the atomic-replace guarantee
    util/               the primitives with no subsystem of their own
  plugins/*.lua         the five plugins that were built in, staged until spec section 6's repos exist
  test/                 one test file per src module, plus a real HTTP server fixture
  vendor/cJSON          JSON parsing
  vendor/toml           TOML parsing
  vendor/lua            the lua 5.4 library, no standalone interpreter
  vendor/greatest       the test harness
  cmake/  build/        build machinery and output
  plugin.json           id daukle, category tool, tech c
```

`docs/` is gitignored and holds nothing tracked in this repository, not even a `docs/superpowers/`:
there is no re-include, and no `docs/` directory exists here at all. The design spec, its plan and
the task queue live in the separate `daukle/docs` repository, cloned beside `daukle/daukle`, as
section 1's opening paragraph says. This file still sits at the repo root rather than under `docs/`,
because it is the map a contributor reads first.

**It is a TOOL, which means it is outside every ecosystem and terminal.** Nothing may reference it.
It is invoked, never linked. That is why it may carry whatever dependencies it likes and owes none of
the client tier's rules.

## 2. The ownership index

| concern | owned by | it is NOT |
| --- | --- | --- |
| reading a project or manifest file into structs | `src/config/manifest.c` | parsing JSON, which is `jsonx` over vendored cJSON |
| choosing which module of which project answers a coordinate | `src/plugin/resolve.c` | fetching anything. It resolves, the source fetches |
| the three plugin tables, source, language and config | `src/plugin/registry.c` | a plugin. It holds `fr_source_plugin`, `fr_language_plugin` and `fr_config_plugin` |
| the five plugins that were built in | `plugins/*.lua` | staged here until the repositories in spec section 6 exist. Not fixtures: they are the content those repositories will carry, and `cmake/check_agnostic.cmake` holds every fixture copy byte identical to them |
| finding the manifest and choosing its format | `src/config/config.c` | a parser. It dispatches to a registered `fr_config_plugin` |
| reading json text into the document model | `fr_json_parse` in `src/config/jsonx.c` | a manifest format, nor a registered `fr_config_plugin`. It is reachable only through the `daukle.json_parse` verb. It lived in a `config_json.c` until 2026-10-01, whose name asserted the opposite |
| reading a toml manifest | `src/config/config_toml.c` | the whole config table. `src/config/config_lua.c` is the overlay beside it, registered the same way |
| rewriting a marked region of a file in place | `src/project/region.c` | a JSON editor. `src/config/jsonedit.c` is, for files that are JSON |
| rewriting a toml manifest in place | `src/config/tomledit.c` | a toml parser. It splices spans, as `src/config/jsonedit.c` does for json |
| the whole write pass over a manifest | `src/project/sync.c` | per-language. It drives the language plugins |
| the derived directory: its path, the ledger of what daukle generated, write-if-changed, the sweep, and clean | `src/project/derived.c`, tested by `test/test_derived.c` | the generation pass, which is `src/project/generate.c`. It is handed a file set and decides only what happens on disk |
| the generation pass: resolving each declared toolchain, building what its `generate` receives, and validating what it returns | `src/project/generate.c`, tested by `test/test_generate.c` | what happens on disk, which is `src/project/derived.c` |
| the task graph: collecting a plugin-declared task together with the manifest task blocks that add edges to it, planning a goal's transitive closure in dependency order, running that plan, and, for a publish, both halves of the destination sweep: `fr_tasks_check_publish` plans every selected destination before any of them runs, and `fr_tasks_run_publish` then runs them in the order the manifest declares. The two live together because they are one guarantee, and the command layer that held the second of them has no test binary | `src/project/tasks.c`, `src/project/tasks.h`, tested by `test/test_tasks.c` and, end to end, `test/test_e2e.c` | starting a process itself. `fr_tasks_run` ensures a run-bearing task's derived directory exists and calls its plugin's `run`; that callback is what calls `daukle.exec`, not `src/project/tasks.c` |
| HTTP, per platform | `src/net/http.c` over `src/net/http_curl.c` and `src/net/http_winhttp.c` | two implementations to keep in step. One interface, one backend per platform |
| where the cache lives on disk, the format-neutral name a cached manifest is stored under, and the atomic-replace guarantee everything under it is written through | `src/cache/cache.c` | any particular key shape. `fr_plugin_fetch` keys on a url hash and `src/provision/provision.c` keys on a content digest, each laying out its own directory shape beneath the root `src/cache/cache.c` hands back |
| version ranges and ordering | `src/util/semver.c` | date or tag ordering |
| URL building and escaping | `src/net/url.c` | HTTP |
| growable strings | `src/util/strbuf.c` | a general container library |
| the command surface | `src/cli/cli.c`, `src/cli/main.c` | logic. Everything it calls lives in a module beside it |
| errors | `src/util/error.c`, `fr_error` | logging. The caller decides what to print |
| the lua state, its memory cap and its errors | `src/lua/luax.c` | the sandbox, which is `src/lua/lua_sandbox.c`, nor the config format, which is `src/config/config_lua.c` |
| what a configuration script may touch | `src/lua/lua_sandbox.c` | a permission system. It curates one globals table and bounds daukle.include |
| verifying a spliced manifest still parses | `src/config/tomledit.c`, through `FR_CONFIG_TOML` | a second parser. It reads its own output back so no caller is handed text it would be wrong to write |
| the one lua state's lifetime, running a configuration script and reading it back, and what the daukle table a script sees carries | `src/config/config_lua.c` | the verbs a plugin declares with, which are `src/config/config_declare.c`, nor the sandbox or the lua state, which are `src/lua/lua_sandbox.c` and `src/lua/luax.c` |
| the declaration verbs, `daukle.language`, `source`, `toolchain`, `task`, `publisher` and `resolver`: what each one refuses, the slot it fills and what it registers | `src/config/config_declare.c` | calling any of them back, which is `src/config/config_dispatch.c`, nor the registry they land in, which is `src/plugin/registry.c` |
| calling a registered plugin back: the protected frame each dispatch runs under, the table it builds for the callback, and what it reads out of the answer | `src/config/config_dispatch.c` | declaring or registering anything, which is `src/config/config_declare.c`. It is handed the slot and calls the callback the slot names |
| `daukle.require`: what a module name resolves to, the per-load memo, the owner frame a module of another plugin runs under, and the cycle report | `src/lua/lua_modules.c` | where a module's bytes come from, which is `src/plugin/plugin_modules.c`, nor who may reach across a plugin boundary, which is the `requires` graph in `src/plugin/plugin_deps.c` |
| what a plugin may touch, and building one environment per plugin | `src/lua/lua_verbs.c` | the configuration sandbox, which is `src/lua/lua_sandbox.c`. One is per plugin, the other is the state's globals |
| reading `[plugins]`, and acquiring a plugin by path, url or resolver | `src/plugin/plugins.c` | fetching bytes, which is `src/plugin/plugin_fetch.c`, or turning a coordinate into a url, which is a resolver plugin through `src/plugin/resolvers.c` |
| the declaration pass: running a chunk far enough to read its `daukle.plugin` table, and validating the `uses`, `exports` and `requires` it names | `src/plugin/plugin_declaration.c` | loading the plugin, which is `src/plugin/plugins.c`. The chunk is run twice on purpose, once for its declaration and once for real |
| what `daukle config print` says about each plugin and each dependency: the rows, their owned copies of borrowed strings, their sort order, and the declared resolvers nothing named | `src/plugin/plugin_report.c` | deciding anything. It records what the load pass already did, and `src/plugin/plugins_internal.h` is the only way to append to it |
| fetching and caching the bytes at a plugin URL; knows no coordinate, version or host | `src/plugin/plugin_fetch.c` | resolving a coordinate to a URL, or interpreting the bytes it fetches, which are the plugin loader's question |
| the `[resolvers]` table, and acquiring, running and invoking a resolver plugin | `src/plugin/resolvers.c` | fetching bytes, which is `src/plugin/plugin_fetch.c`, or loading a plugin, which is `src/plugin/plugins.c` |
| the ustar header format itself: `fr_tar_read_header` parses one 512-byte block once, and is consumed by two readers, its own bounded in-memory one (`fr_tar_read`, bytes in, an ordered member list out, nothing unpacked and no filesystem call at all) and `src/archive/archive.c`'s streaming one, called from `archive.c:292` | `src/archive/tar.c`, tested by `test/test_tar.c` | the 64-member and 1 MiB-per-member caps, or the no-prefix-field refusal; those are `fr_tar_read`'s own policy as the plugin-module loader's reader, not a limit the header parser imposes on `src/archive/archive.c`'s caller. Nor does it know anything about lua, plugins or daukle: whether a member must be `.lua`, and whether its name may climb out, are daukle's rules and live in `src/plugin/plugin_modules.c` |
| where a plugin's code comes from, with one interface over a single chunk, an archive held in memory and a local directory; also the `daukle.require` module-name rule | `src/plugin/plugin_modules.c`, tested by `test/test_plugin_modules.c` | running a module, which is `src/lua/lua_modules.c`, and parsing an archive, which is `src/archive/tar.c` |
| a sha-256 digest | `src/util/sha256.c` | a general crypto library |
| the shared exec logic, joining a program and its argument vector into the one command line `CreateProcess` requires, and freeing an `fr_exec_result` | `src/exec/exec.c`, `src/exec/exec.h`, tested by `test/test_exec_quote.c` | spawning a process. That is `src/exec/exec_posix.c` and `src/exec/exec_win32.c`. Reachable from Lua through `daukle.exec`, which takes a `daukle.tool` handle, never a path string |
| spawning a process on POSIX with `fork`/`execv`, capturing its streams up to `FR_EXEC_CAPTURE_LIMIT` and reporting its exit code | `src/exec/exec_posix.c`, tested by `test/test_exec.c` | building the command line, which stays in `src/exec/exec.c` because Windows needs it too |
| spawning a process on Windows with `CreateProcessA`, capturing its streams up to `FR_EXEC_CAPTURE_LIMIT` and reporting its exit code | `src/exec/exec_win32.c`, tested by `test/test_exec.c` | building the command line, which it calls into `src/exec/exec.c` for |
| resolving an executable name to an absolute path by searching the host's `PATH` | `src/exec/tool.c`, tested by `test/test_tool.c` | provisioning a missing tool. Discovery only, per spec section 3.1; child spec 4 extends the same `fr_tool_resolve` with that later. Reachable from Lua through `daukle.tool`, which returns an unforgeable full-userdata handle, never the path itself. A name that resolves to a `.bat` or `.cmd`, whether by extension search or literally, is refused naming the file, since starting one needs `cmd.exe`; `fr_tool_is_batch_file` is that test and a provisioned root's member is held to the same one by `:tool`. `:path` is not, because naming a file starts nothing; it is held to `fr_tool_is_regular_file` instead |
| enumerating and decompressing a tar, a tar.gz or a zip: bytes sniffed for their kind in, an ordered stream of members out | `src/archive/archive.c`, tested by `test/test_archive.c` | deciding where a member lands or writing it anywhere. It never opens a destination file; that decision and every write belong to `src/archive/unpack.c` |
| every path decision and every write when an archive is expanded onto disk: containment across members as well as within one, so a member reachable only through a symlink an earlier member created is refused; what a symlink, a directory and a plain file each become; and the byte and member-count limits in `FR_UNPACK_DEFAULTS`, counted for every member kind because the reader has already expanded each one | `src/archive/unpack.c`, tested by `test/test_unpack.c` | telling a tar header from a zip central directory record, or any other format detail. It reads an already-opened `fr_archive` and knows nothing of what is inside it beyond a member's name, kind and bytes |
| composing a fetch, a digest verification and an unpack behind one pinned call, and the content-addressed cache of provisioned toolchains that keys on the digest alone so two urls serving the same bytes share one tree | `src/provision/provision.c`, tested by `test/test_provision.c` | any Lua surface. `daukle.provision` and the root handle's `:tool` and `:path` methods are `src/lua/lua_verbs.c`'s, which is `src/provision/provision.c`'s only caller. The two methods share one containment check, `root_member_path`, and differ only after it: `:tool` demands an executable and returns an opaque handle, `:path` demands a file and returns its absolute path as a string, so a plugin can pass a provisioned script to a provisioned interpreter |
| what a provisioned or installed tool's report line says, the links an unpack left out under it, and deduping it to one line per distinct tool per run | `src/exec/toolreport.c`, tested by `test/test_provision.c` | deciding what tool to use or whether to provision one at all. That decision is made before anything reaches `src/exec/toolreport.c`; it only renders the fact of it |
| decompressing and enumerating a zip's central directory | `vendor/miniz`, compiled with `MINIZ_NO_STDIO` | opening or creating a file. Without stdio it cannot, even by a caller's own mistake, which is what keeps every path decision and every write inside `src/archive/unpack.c` |

**A `daukle.lua` runs against a curated globals table, not Lua's own.** The two lists that define it
are `KEPT` and `REMOVED` at the top of `src/lua/lua_sandbox.c`, and reading a removed name raises an
error naming it rather than returning nil. Section 3.4 of the design spec explains each removal;
the source lists are the authority and the spec follows them.

**The registry is destroyed before the lua runtime is shut down, always.** A `daukle.lua` may
register plugins, and `src/config/config_lua.c` owns their capability strings while the registry stores the
plugin structs by value. So the order is: build the registry, load the configuration, destroy the
registry, then `fr_lua_runtime_shutdown()`. `fr_lua_runtime_begin`, which `config_lua_load`
delegates to, returns `FR_OK` for a load into the registry the open phase already belongs to, an
idempotent no-op, and refuses a load into a different one while that registry still holds the
first one's plugins rather than freeing what it would go on reading.

It refuses a differing **base directory** on the same terms, and for a sharper reason. The sandbox
is installed once, when the state is opened, so reusing the state for a second directory would leave
`daukle.read` bounded by the first caller's directory with nothing saying so. The comparison is
between canonical paths, which `fr_lua_sandbox_install` hands back so there is one resolution rather
than two that could disagree, and the refusal names both directories.

**Adding a source or a language means writing a Lua plugin and declaring it in `[plugins]`.**
`config_lua.c:337` and `config_lua.c:355` are the only places that fill in an `fr_language_plugin`
or an `fr_source_plugin` and register it, turning a `daukle.language{}` or `daukle.source{}`
declaration into a registry entry; no other file in `src/` calls `fr_registry_add_source` or
`fr_registry_add_language`, whose only direct callers in `test/` are `test_registry.c`'s unit tests of
the registry and the one stub source `test_resolve.c:75` injects to watch a plugin get its own state.
Neither touches `src/plugin/resolve.c`, and a change that does touch it for a new source or language is the
signal that the seam was bypassed.

**A source or a language plugin may not declare `exec`.** `lua_declare_language` and
`lua_declare_source` in `src/config/config_lua.c` both refuse with "daukle.exec is available only to a
toolchain plugin" when `fr_lua_verbs_env_declared_exec` (`src/lua/lua_verbs.c`) reports that the environment
the plugin's chunk is running in included `exec`. The flag is reset at the top of every
`fr_lua_verbs_push_env` call, so it can never carry a stale answer from a previously loaded plugin.
Only a toolchain plugin, not yet built, may declare `exec`.

That check fires when a plugin says what kind it is, which is too late on its own: a plugin that
calls `daukle.exec` at the top of its chunk and declares afterwards has already run the program.
So `verb_exec` refuses again at the call itself, for the whole of any plugin chunk
(`fr_lua_plugin_exec_is_refused` in `src/config/config_lua.c`), since no plugin kind that may exec exists yet.
The two together are what make the refusal fail closed.

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

Most modules have a dedicated test file; the rest are covered through the tests of the module that
drives them, such as `fr_json_parse` through `test_lua_verbs.c`'s `json_parse` tests.

**daukle no longer tests what npm, Gradle or C output looks like.** `test_lang_npm.c`,
`test_lang_gradle.c`, `test_lang_c.c` and `test_source_github.c` were deleted with the modules they
covered, taking with them `empties_the_region_for_no_modules` and `fails_without_a_configuration`
for both gradle and c, `names_the_module_whose_block_has_no_coordinate`,
`names_the_module_whose_block_has_no_url`, the `carries_the_expected_capability` string assertions,
and github's url and coordinate assertions, which the github swap's commit body lists by name for
recovery from history. `fails_when_the_target_carries_no_region` is lost for c's case only; gradle's
equivalent is still driven end to end by `reports_a_build_file_without_markers`
(`test/test_sync.c:78-87`).
After the extraction that output is not daukle's behaviour, so the coverage belongs to `daukle/npm`,
`daukle/gradle`, `daukle/c` and `daukle/github`. daukle's suite will not catch a regression in Gradle
coordinate formatting. Accepted in spec section 7.

What daukle still covers, and must keep covering, is the **mechanism**: that a language plugin's
returned text reaches the consumer file, that a source plugin's returned table becomes an
`fr_project`, and that a missing capability names the plugin that would provide it.
`test_e2e_languages.c` is the broadest test of real language output end to end: it is the only test
exercising c's output at all, and the only one exercising npm's formatting depth, through
multi-entry ordering, the optional-`sha256` mix, one space before each package, the absent trailing
newline, idempotence, the npm untouched-text and ledger-sort cases, and the one case the ledger
exists for: a package the ledger owns and the resolver dropped leaves the target, while a package the
user added by hand and no ledger claims stays. `test_sync.c` and `test_e2e.c` also assert real gradle
output, but neither touches c or npm's formatting rules.
`test_e2e_languages.c` does its work through byte-identical copies of `plugins/` inside its own
fixture directories rather than through `plugins/` itself, and Task 14 must replace those copies with
minimal fixtures rather than delete the test. No test loads a staged plugin directly, which is why the
`agnostic_core` ctest entry also compares each `plugins/*.lua` against every same-named copy under
`test/fixtures/`: an edit to the staged file, or to one copy and not the rest, would otherwise be
invisible for as long as the staging directory exists.

**`fr_registry_add_source`'s and `fr_registry_add_language`'s duplicate-capability refusal is now
unreachable from every production path.** Both callers sit behind `take_slot` in `src/config/config_lua.c`,
which refuses a duplicate first, and every production path destroys the registry and the Lua runtime
together, so reaching the registry's own refusal needs a registry that outlives a runtime shutdown.
It is asserted only by direct unit tests in `test/test_registry.c`. That refusal is the reason each
of the five plugin swaps had to be a single atomic commit, so the work ends by leaving the guard that
shaped it reachable only from tests.

## 4. State

The resolver, the cache, the sync pass and the CLI are all implemented and tested. Every source and
every language now arrives through a plugin: `fr_build_registry` in `src/project/sync.c` registers only the
config formats, and the registry's source and language tables start empty. The configuration surface
is TOML with an optional Lua overlay: `src/config/config.c` finds the manifest and dispatches to a registered
`fr_config_plugin` by a capability string built from the file's extension, and `src/config/config_toml.c` and
`src/config/config_lua.c` are the only two registered formats, chosen the same way, with neither named in
`src/config/config.c` itself. TOML is the format a manifest is authored in going forward, and it is also the
asset name the `daukle/github` plugin fetches by default from a release. JSON is no longer a manifest
format at all: `fr_json_parse` registers nothing and survives only so a plugin can read its own data
file through `daukle.json_parse`, such as npm recovering its ledger from a `package.json`.

Task **F-7** in `spisor/docs/TASKS.md` is unaffected by this work: it says both of the phase-2
manifest writers are proven against fixtures only, because nothing in the ecosystem publishes a
manifest release asset yet. basekit's 5.0.0 release carries eight jars and no manifest. Until
something publishes one, the writers have never met real input.

`daukle add` edits TOML only, and `src/cli/main.c` names that format on purpose: it checks the manifest path
ends in `.toml` and calls `fr_toml_edit_set_dependency` directly, which is a deliberate, accepted
exception to the agnostic-core rule (ruling R29). The general form would be an edit hook on
`fr_config_plugin`, so every format supplies its own editor and `src/cli/main.c` names none of them, but that
would touch a plugin struct four completed tasks already depend on and would buy nothing until a JSON
or Lua manifest needs editing in place too, so it is deferred rather than built now.

`daukle add` appends a dependency that is not there yet and updates the version of one that is. It
does not rewrite an existing module list, so `--modules` for a dependency that already exists is an
error rather than a silent discard, and whatever it produces is read back through the TOML reader
before anything is written.

A manifest's `[plugins]` table registers a source or a language by running each declared plugin in a
`src/lua/lua_verbs.c` environment scoped to exactly the verbs it declared, through `src/plugin/plugins.c`. It is now
the only route into the registry's source and language tables:
`fr_build_registry` registers only the config formats, and the five files that used to compile a
source or a language directly into the binary, `source_path.c`, `source_github.c`, `lang_npm.c`,
`lang_gradle.c` and `lang_c.c`, are gone. `FR_SOURCE_[A-Z]` and `FR_LANGUAGE_[A-Z]` are in the
agnostic check's `FORBIDDEN` list, so `src/config/config.c` cannot silently regain one.
`check_agnostic.cmake` enforces two rules over two file sets: no plugin name in `CORE_FILES`, and no
forge host, `api.github.com`, `github.com`, `gitlab` or `bitbucket`, anywhere in `src/*.c` or
`src/*.h`, which is why the second rule reaches `src/plugin/plugins.c` and `src/plugin/resolvers.c` that the first excludes.
`daukle plugin update [label]` always reads the current manifest first and removes a
remote plugin's cached copies scoped to what THAT manifest declares: one label's entry, or, with no
label, every remote entry it declares, and nothing outside it, so the plugin cache root, which is
shared across every project on the machine, is never touched beyond this manifest's own plugins. The
next run re-resolves whatever was removed. `daukle config print` shows every loaded plugin's label,
its resolved version or local path, its declared verbs and its sha-256 digest, pinned or not, so
adopting a pin is a copy of that printed digest rather than a separate lookup.

`daukle <task>` plans the named task's transitive closure through `fr_tasks_plan` before syncing or
running anything, so an unknown goal never triggers the write a real one would have caused; naming a
goal nothing declares exits 2 and names the plugin count, so "you typed the name wrong" reads
differently from "this project has no plugins at all". `daukle tasks` lists every task
`fr_tasks_collect` gathers, from a plugin or from a manifest `[tasks]` block alike, tagging the
manifest ones `(from the manifest)`; for each task it also prints `pulls in` (what runs before it) and
`needed by` (what runs after it), the two directions `fr_tasks_joiners` computes.

`daukle plugin update` reads a lua-rooted manifest as readily as a toml one. It used to refuse an
overlay format outright, on the grounds that reading a lua manifest means executing it and executing
is the opposite of what a command discarding a plugin's cache wants. Reading one does execute it,
but nothing that execution needs comes from the cache being cleared: the chunk runs before
`fr_plugins_load`, which is what resolves and executes declared plugins and which
`read_manifest_plugins` never calls at all. Every other subcommand already executed the same file,
so the refusal was costing lua-rooted projects one command and buying nothing.

What the refusal was standing in for is the **registry**, and that is a real constraint rather than
a dissolved one. A lua format reopens the runtime through `fr_lua_runtime_begin`, which refuses a
registry that is not the one already open; the old code passed `NULL`, so removing the refusal alone
fails with `another registry's plugins are still registered`. `plugin_update` therefore opens the
runtime on the manifest's own directory **before** reading it and hands both the directory and the
registry down to `read_manifest_plugins` (`src/cli/main.c`).
