# 3. The logic layer

**daukle's configuration is not only a static file.** `daukle.toml` carries the data, and an
optional `daukle.lua` beside it carries logic. It is daukle's answer to a Gradle build script, and
a project without one never starts an interpreter.

The example on this page is `daukle-logic-layer`, in this repository.

## When you need it

You do not, usually. Reach for it when the manifest would otherwise have to repeat itself, or
when a value is not knowable until daukle runs.

```lua file=examples/daukle-logic-layer/daukle.lua
-- The LOGIC half, and daukle's answer to a Gradle build script.
--
-- Everything here is something a static manifest cannot say. It is optional:
-- a project with no daukle.lua never starts an interpreter, and most projects
-- do not need one.

-- One list, named once. In TOML the same package appears in the consumer's
-- dependency table and again in its module list, and the two drift. Here
-- adding a package is one line and both follow.
local WANTED = {
  { project = "example/greeter", version = "^2.0.0", modules = { "cli" } },
}

-- Not knowable when the file is written: a library being developed wants its
-- dependency in devDependencies, a release wants it in dependencies, and the
-- same checkout has to do both. TOML has no way to express "it depends".
local section = daukle.env("DAUKLE_EXAMPLE_DEV") == "1" and "devDependencies"
                                                        or "dependencies"

local dependencies = {}
for index = 1, #WANTED do
  local entry = WANTED[index]
  dependencies[entry.project] = { version = entry.version, modules = entry.modules }
end

daukle.config.consumers = {
  {
    id = "node",
    language = "npm",
    file = "package.json",
    configuration = section,
    dependencies = dependencies,
  },
}

daukle.log("writing " .. #WANTED .. " dependency into " .. section
           .. " for " .. daukle.host.os .. "/" .. daukle.host.arch)
```

## The two things TOML cannot do

**Say a thing once.** The packages are named in one list. In TOML the same package appears in the
dependency table and again in the module list, and the two drift the first time somebody edits one
of them.

**Decide at read time.** Run the SAME checkout twice:

```sh
daukle sync                        # cowsay lands in dependencies
DAUKLE_EXAMPLE_DEV=1 daukle sync   # the same checkout puts it in devDependencies
```

A static manifest has to pick one, and a project that needs both keeps two manifests.

`daukle.host` is published the same way, with `os` and `arch`, so a dependency set that differs by
platform is three more lines.

## What it is allowed to do

The config sandbox is the plugin sandbox: `string`, `table`, `math`, plus `daukle.host`,
`daukle.env`, `daukle.log` and `daukle.include`. No `io`, no `os`, no `require`.

**So it computes; it does not inspect.** It cannot read the producer's manifest and enumerate it,
which is the first thing most people try. Its whole input surface is literals, the host and the
environment.

## `daukle.include`, and why it matters more than it looks

`daukle.include("some/file.lua")` runs another Lua file in the same sandbox. That is how
**generated** configuration reaches a manifest without being pasted into it:

```lua
-- daukle.lua, the one line a human writes
daukle.include("daukle/maven/classpath.lua")
```

`daukle.toml` keeps what you wrote. The twenty resolved dependency pins live in a generated file
marked do-not-edit, and nothing ever asks you to touch it. Page 4 is that story.

**One sharp edge**: `daukle.include` raises if the file is not there, and it raises at config
load, so EVERY command fails rather than just the build. That is why what it includes is
committed.

## What this page does not cover

**What a large `daukle.lua` looks like.** Everything here fits on a screen, and no project in this
org has yet needed a hundred lines of it.

## Next

- [[daukle/04-dependencies-and-compiling]]
