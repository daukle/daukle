# Design: the TOML and Lua configuration surface

Date: 2026-09-18
Status: proposed
Repo: `daukle/daukle` (`github.com/intisy/daukle`)

## 1. Problem

daukle's configuration is a JSON document (`daukle.json`, schema 1) read by `src/manifest.c` over
vendored cJSON. JSON is the wrong surface for humans to author: no comments, no trailing commas, no
way to express a value that depends on the host, and no room for the plugin authoring that a large
plugin API needs. As daukle grows toward driving language modules rather than only resolving
coordinates, the configuration has to carry both data and, occasionally, logic.

JSON is, however, the right surface for machines. The manifests daukle fetches from GitHub releases
are produced and consumed by tools, never hand written.

## 2. Decision

Three layers, each with one job.

| file | role | authored by | format |
| --- | --- | --- | --- |
| `daukle.toml` | the manifest: project, version, modules, sources, consumers | humans, and daukle's own edit commands | TOML 1.0 |
| `daukle.lua` | logic: host conditionals, computed values, plugin registration | humans, optional, absent in most projects | Lua 5.4 |
| published manifest asset | what a project publishes for others to resolve against | tools | JSON, unchanged |

**Lua is loaded only when `daukle.lua` exists.** A project without one never starts an interpreter.

### 2.1 The spine: one validator, three front ends

cJSON stays the internal document model. The TOML reader builds a cJSON tree; the Lua reader
converts the script's table into a cJSON tree; the JSON reader keeps doing what it does. All three
hand the same tree to the same validation and struct-filling code in `manifest.c`.

This is the load bearing decision of the design. It means `resolve.c`, `sync.c` and every
`lang_*.c` plugin, which all read `const cJSON *` blocks today, need no change at all, and it means
a malformed TOML manifest and a malformed JSON manifest produce the same diagnostics from the same
code path.

A neutral value tree (`fr_value`) was considered and rejected: it would touch every module in the
repo to buy a decoupling from cJSON that nothing has asked for. TOML's only type without a JSON
counterpart is the date, which daukle has no use for and which the reader renders as a string.

### 2.2 The schema does not change

`schema = 1` in TOML means exactly what `"schema": 1` means in JSON. The data model is identical;
only the notation differs. The fixture `test/fixtures/consumer/daukle-github.json` becomes:

```toml
schema = 1
project = "forebay/stub-translator"
version = "1.2.0"

[modules]

[sources."forebay/basekit"]
kind = "github-releases"
repo = "forebay/basekit"
version = "5.0.0"

[[consumers]]
id = "stub"
language = "gradle"
file = "build.gradle"
configuration = "githubImplementation"

  [consumers.dependencies."forebay/basekit"]
  version = "^5.0.0"
  modules = ["ir"]
```

Compiler concepts (targets, toolchains, build rules) are deliberately absent. They get their own
spec, written against the Lua API this one defines.

## 3. Components

### 3.1 `config.c` / `config.h`: the format seam

A third plugin table joins the two in `registry.c`:

```c
typedef struct {
    const char *capability;              /* daukle.config/toml, daukle.config/json, ... */
    int (*load)(void *state, const char *text, const char *origin,
                const char *base_dir, struct cJSON **out, fr_error *err);
    void *state;
} fr_config_plugin;
```

Capabilities are namespaced as the existing two tables are, so a format is
`daukle.config/toml` beside today's `daukle.language/npm` and `daukle.source/github-releases`, and
`config.c` builds that string from the file extension without naming a format itself.

`config.c` owns the search order and the dispatch; it does not know the name of any format. The
search order in a project directory, with `--manifest <path>` overriding it and dispatching on the
path's extension:

1. `daukle.toml` exists: it is the manifest. If `daukle.lua` also exists, it runs afterwards as the
   logic layer.
2. otherwise `daukle.json` exists: it is the manifest, and daukle prints a one line notice naming
   `daukle migrate` (see section 8). `daukle.lua` still runs afterwards if present.
3. otherwise `daukle.lua` exists alone: it is both the manifest and the logic layer.
4. otherwise: an error naming all three.

`daukle.toml` and `daukle.json` together is an error, not a precedence rule. Two manifests in one
directory is a mistake worth reporting rather than silently resolving.

One ordering constraint falls out of this and is easy to get wrong: `build_registry` in `sync.c`
currently runs after the manifest is read, but a Lua configuration registers plugins while it is
being read. The registry is therefore created and filled with built ins **before** the config layer
runs, and the config layer is handed the registry to add to. Reading a manifest and registering a
plugin become one pass.

### 3.2 `config_toml.c`: TOML to cJSON

Wraps a vendored TOML parser and walks its tree into cJSON. Vendor `arp242/toml-c` (MIT, single
translation unit, TOML 1.0 compliant, a maintained fork of `cktan/tomlc99`), added to the existing
`daukle_vendor` target beside cJSON. `cktan/tomlc17` is the fallback if that fork stalls.

Mapping: table to object, array to array, string to string, integer and float to number, boolean to
boolean, date and time to string. Nothing else exists in TOML 1.0.

### 3.3 `config_lua.c`, `luax.c`, `lua_sandbox.c`: the logic layer

`luax.c` is to Lua what `jsonx.c` is to cJSON: the thin helper every other module goes through. It
owns the bidirectional cJSON conversion, which both the config reader and the plugin adapters need:

```c
int fr_lua_push_json(lua_State *state, const struct cJSON *value, fr_error *err);
int fr_lua_to_json(lua_State *state, int index, struct cJSON **out, fr_error *err);
```

The table to cJSON direction has one ambiguity worth naming: an empty Lua table could be an empty
object or an empty array. Rule: an empty table becomes an empty object, and a table with a
contiguous integer key sequence starting at 1 becomes an array. Where a schema position demands an
array, the validator already reports the mismatch.

`config_lua.c` runs the script and reads the result back:

1. Create a sandboxed state (3.4).
2. Publish the `daukle` global: `daukle.config` (the manifest as a table, populated from the TOML or
   JSON layer, or empty when Lua is the only manifest), `daukle.host`, `daukle.env`,
   `daukle.source`, `daukle.language`, `daukle.log`.
3. `lua_pcall` the chunk.
4. Convert `daukle.config` back to cJSON and hand it to the same validator every other format uses.

The script mutates `daukle.config` in place. It does not return the config, because a script that
also registers plugins would otherwise have two ways to say what it is doing.

### 3.4 The sandbox

The configuration of a repository you cloned should not be able to read your files. A daukle
configuration declares; daukle acts.

Available: `assert`, `error`, `ipairs`, `pairs`, `next`, `select`, `tonumber`, `tostring`, `type`,
`setmetatable`, `getmetatable`, `_VERSION`, and the `string`, `table` and `math` libraries whole.
The list that ships is `KEPT` in `src/lua_sandbox.c`, and it is the authority.

Removed: `io`, `os`, `package`, `require`, `dofile`, `loadfile`, `load`, `debug`, `collectgarbage`,
`print`, `coroutine`, `utf8`, `pcall`, `xpcall`, `rawget`, `rawset`, `rawequal` and `rawlen`.
`print` is replaced by `daukle.log`, which routes through the CLI's output rather than straight to
stdout, so the CLI stays the only layer that decides what the user sees.

Three of those removals are narrower than they look and are worth the sentence each:

- `os` goes whole, `os.time` and `os.date` included, because 3.6 claims a configuration's output is
  a pure function of the manifest, the script and a closed set of host facts. A clock is not in that
  set, and a manifest that resolves differently on Tuesday is the bug that claim exists to prevent.
- `pcall` and `xpcall` go because they catch the error the instruction budget raises, which would
  let a script loop past its own cap.
- `rawget`, `rawset`, `rawequal` and `rawlen` go because they reach past the metatable that reports
  a removed name, which is how a script would be told it cannot have `io` rather than silently
  reading nil.

Reading a removed name raises an error naming it, rather than returning nil, so a script that wants
one fails where it asks instead of somewhere later.

Scripts split across files use `daukle.include("path")`, which resolves only inside the project
directory, rejects `..` traversal and absolute paths, and executes in the same sandboxed state.

Two resource caps, both because an unbounded loop in a fetched configuration is a denial of service
with a friendly face:

- an instruction budget enforced by a `lua_sethook` count hook, default 50 million, raised by
  `--lua-instruction-limit`
- a memory cap enforced by the custom `lua_Alloc` passed to `lua_newstate`, default 64 MiB, raised by
  `--lua-memory-limit`

Neither cap is configurable from inside the sandbox.

Process execution is not in this layer at all. A language plugin that must invoke a compiler
declares it; running it belongs to the (not yet specified) build driver, which is where a
capability grant will be designed when there is something to grant it to.

### 3.5 The Lua plugin API

The point of Lua is that a language module stops requiring a C compiler and a shared object. Two
registration functions cover the two existing plugin tables:

```lua
daukle.source{
  name = "https-tarball",
  load = function(project, block, base_dir)
    return { version = "1.2.0", modules = { ir = { npm = { package = "@x/ir", range = "^1.2.0" } } } }
  end,
}

daukle.language{
  name = "zig",
  apply = function(consumer, resolved, text)
    return text .. "\n"
  end,
}
```

Each is implemented by exactly one C adapter, `lua_source_load` and `lua_language_apply`, that
marshals through `luax.c` and is registered into the existing `fr_source_plugin` and
`fr_language_plugin` tables. **The C plugin ABI does not widen.** A Lua language plugin and a C
language plugin are indistinguishable to `sync.c`, and the core stays agnostic of both.

Name collisions with a built in capability are an error naming both, not an override. Overriding
`npm` from a fetched configuration is not a feature.

### 3.6 `daukle.host` and `daukle.env`

Lua may compute, and Lua may not perform I/O, so a configuration's output is a pure function of the
TOML, the script, and a closed set of host facts. That set is:

- `daukle.host.os`: `"windows"`, `"linux"`, `"macos"`
- `daukle.host.arch`: `"x86_64"`, `"aarch64"`, `"x86"`
- `daukle.env(name)`: a read of one environment variable, returning nil when unset

`daukle.host` carries `os` and `arch` and nothing else. A version field was considered and not
built: a configuration that branches on daukle's own version resolves differently under two daukle
builds, which is the same purity the clock argument above rules out.

Every `daukle.env` read is recorded, name and value, on the in-memory config result, and
`daukle config print` prints the list. Nothing writes it to a file in this spec. The point is that a
future lockfile can record what a configuration actually depended on without a second pass over
this code.

### 3.7 `tomledit.c`: edits that keep the file

`daukle add forebay/basekit@^5.0.0 --to stub` must rewrite `daukle.toml` without discarding the
user's comments, ordering or spacing. No TOML library round trips comments, so this module does what
`jsonedit.c` already does for JSON: it locates the span of text to change and splices it, leaving
every other byte untouched. `jsonedit.c` is the template, and the two modules stay siblings rather
than one generalised editor, because their span finding has nothing in common.

Version 1 supports exactly the operations `daukle add` needs: insert a dependency table under a
named consumer, and update the `version` of an existing one. Removal follows the same seam later.

## 4. Data flow

```
daukle.toml ──► config_toml ──┐
daukle.json ──► config_json ──┼──► cJSON document ──► daukle.lua? ──► config_lua ──► cJSON document
daukle.lua  ──► config_lua  ──┘                          (mutates)
                                                                              │
                                                                              ▼
                                                    manifest.c validate and fill fr_manifest
                                                                              │
                                                                              ▼
                                            resolve.c ──► sources ──► sync.c ──► languages
```

Everything below the validator is unchanged code.

## 5. Errors

Lua failures reach the user through `fr_error` like everything else. A message handler installed for
`lua_pcall` formats `chunk:line: message` followed by up to five stack frames; the result is written
into `fr_error`'s 512 byte buffer and truncated with an ellipsis if it does not fit. `--verbose`
prints the untruncated traceback to stderr from the CLI layer, which is the only layer that decides
what to print.

A sandbox violation is an ordinary Lua error, so `io.open` fails with `attempt to index a nil value
(global 'io')`. That message is accurate but unhelpful, so the sandbox installs an `__index`
metamethod on the globals table that reports removed names specifically: `io is not available in a
daukle configuration`.

## 6. CLI

Added:

- `daukle add <project>@<range> --to <consumer> [--modules a,b]`, editing `daukle.toml` in place
- `daukle config print`, writing the post Lua configuration as JSON to stdout

`config print` earns its place twice over: it is how a user sees what their script actually did, and
it is how the tests assert on the Lua layer without reaching inside it.

Added flags: `--lua-instruction-limit`, `--lua-memory-limit`, `--verbose`.

## 7. Build

Vendor Lua 5.4.7 under `vendor/lua` and the TOML parser under `vendor/toml`, both added to the
existing `daukle_vendor` static library. That target deliberately carries no
`DAUKLE_WARNING_FLAGS`, which is what makes vendoring Lua possible under `/W4 /WX` and `-Werror`;
the precedent is cJSON, already there.

LuaJIT is rejected: it is Lua 5.1 semantics, larger, and unavailable on some targets daukle already
builds for, in exchange for a speed nobody needs to read a config file.

Define `LUA_USE_POSIX` off Windows and link `m`. Only the library is built, never the standalone
interpreter, so the readline dependency never appears.

`cmake/check_agnostic.cmake` gains `config.c` and `config.h` in `CORE_FILES`, and
`daukle\.config/[a-z]` in `FORBIDDEN`, matching the two patterns already there for languages and
sources. A bare `"json"` is deliberately not forbidden: core code contains the word in error
messages such as `is not valid json`, and the rule is about capability literals, not about the
internal document model.

## 8. Migration

`daukle.json` is not deprecated as a wire format and never will be. Published manifest assets, the
thing `source_github.c` fetches and `fr_project_parse` reads, stay JSON, which protects the work
behind spisor task F-7.

What changes is the authoring surface. A local `daukle.json` keeps working, prints one notice, and
is converted by `daukle migrate` (a follow on, not in this spec's scope) once anyone wants it
converted.

## 9. Testing

Per repo convention, one test file per module under `test/`, on `greatest`:

- `test_config.c`: search order, the two manifests error, extension dispatch
- `test_config_toml.c`: the type mapping, and a malformed file's diagnostic
- `test_luax.c`: cJSON to Lua and back, including the empty table rule
- `test_config_lua.c`: mutation of `daukle.config`, plugin registration, collision with a built in
  capability
- `test_lua_sandbox.c`: `io.open` reports the specific message; an unbounded loop aborts on the
  instruction budget; a memory hog aborts on the allocator cap; `daukle.include` rejects `..`
- `test_tomledit.c`: an insert and an update each leave every unrelated byte, including comments,
  identical

The decisive test is an equivalence fixture: `test/fixtures/three-ways/` holds the same manifest as
`daukle.json`, `daukle.toml` and `daukle.lua`, and `test_e2e.c` asserts that all three produce
byte identical consumer files. That is the one assertion that proves the spine in section 2.1.

## 10. Out of scope

Named so they are not silently assumed:

- the compiler itself: targets, toolchains, build rules, the build driver
- the lockfile (section 3.6 makes it possible; it does not define it)
- `daukle migrate` and `daukle remove`
- capability grants for plugins that need to spawn processes
- fetching plugins as packages; a Lua plugin in this spec is a local file

## 11. Consequences

`STRUCTURE.md` gains rows for `config.c`, `config_toml.c`, `config_lua.c`, `luax.c`,
`lua_sandbox.c` and `tomledit.c`, in the same commit as the code, as that file requires.

The repo grows two vendored dependencies and roughly 35 additional translation units in the vendor
target. Nothing in `src/` gains a dependency on Lua except the four modules named above.
