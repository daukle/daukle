# 3. The logic layer

**daukle's configuration is not only a static file.** `daukle.toml` carries the data, and an
optional `daukle.lua` beside it carries logic. It is daukle's answer to a Gradle build script, and
a project without one never starts an interpreter.

The example on this page is `daukle-logic-layer`, in this repository.

## When you need it

You do not, usually. Reach for it when the manifest would otherwise have to repeat itself, or
when a value is not knowable until daukle runs.

```lua file=examples/daukle-logic-layer/daukle.lua
local FEATURES = { "logging", "metrics" }

local mode = daukle.env("DAUKLE_EXAMPLE_DEV") == "1" and "debug" or "release"

daukle.config.toolchains = {
  banner = { mode = mode, features = FEATURES },
}

daukle.log("generating a " .. mode .. " banner with " .. #FEATURES .. " features for "
           .. daukle.host.os .. "/" .. daukle.host.arch)
```

## The two things TOML cannot do

**Say a thing once.** The features are named in one list, and the generated header's count and
flags all follow from it. In TOML the list and anything derived from it are written separately,
and they drift the first time somebody edits one of them.

**Decide at read time.** Run the SAME checkout twice:

```sh
daukle sync                        # BUILD_MODE is "release"
DAUKLE_EXAMPLE_DEV=1 daukle sync   # the same checkout generates a debug banner
```

A static manifest has to pick one, and a project that needs both keeps two manifests.

`daukle.host` is published the same way, with `os` and `arch`, so a configuration that differs by
platform is three more lines.

## What it is allowed to do

The config sandbox is the plugin sandbox: `string`, `table`, `math`, plus `daukle.host`,
`daukle.env`, `daukle.log` and `daukle.include`. No `io`, no `os`, no `require`.

**So it computes; it does not inspect.** It cannot read the source tree and enumerate it, which is
the first thing most people try. Its whole input surface is literals, the host and the environment.

## `daukle.include`, and why it matters more than it looks

`daukle.include("some/file.lua")` runs another Lua file in the same sandbox, for Lua of your own
split across files.

**A resolver's generated pins do NOT need it.** Core loads every committed `daukle/<dir>/<file>.lua`
by itself, after `daukle.toml` and `daukle.lua`, so `daukle.toml` keeps what you wrote, the pins
live in a generated file marked do-not-edit, and nothing asks you to write a line for them. Page 4
is that story. An include of such a path, left from before, still runs the file where it sits, once,
and does nothing when the file is gone.

**One sharp edge**: `daukle.include` raises if the file is not there, and it raises at config load,
so EVERY command fails rather than just the build.

## What this page does not cover

**What a large `daukle.lua` looks like.** Everything here fits on a screen, and no project in this
org has yet needed a hundred lines of it.

## Next

- [[daukle/04-dependencies-and-compiling]]
