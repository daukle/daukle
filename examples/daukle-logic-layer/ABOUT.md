# daukle-logic-layer

```console
$ daukle sync
writing 1 dependency into dependencies
```

**daukle's configuration is not only a static file.** `daukle.toml` carries the data and an
optional `daukle.lua` beside it carries the logic, which is what Gradle uses Groovy or Kotlin for.
A project without a `daukle.lua` never starts an interpreter, so this costs nothing to projects
that do not want it.

This example splits one project across both halves and uses the Lua half for two things TOML
cannot express.

## One list, used twice

The packages this project consumes are named **once**. In TOML the same package appears in the
consumer's dependency table and again in its module list, and the two drift apart the first time
somebody edits one of them. Here adding a package is one line and everything follows from it.

## A value that is not knowable when the file is written

Which npm section the dependency lands in is decided when daukle runs:

```sh
daukle sync                        # cowsay lands in dependencies
DAUKLE_EXAMPLE_DEV=1 daukle sync   # the same checkout puts it in devDependencies
```

That is the point of the layer. A static manifest has to pick one and a project that needs both
keeps two manifests.

`daukle.host` is published the same way, so a dependency set that differs by operating system or
architecture is the same three lines.

## What the sandbox allows

A configuration that declares is safe to run; a configuration that acts is not. The Lua here gets
`string`, `table` and `math`, plus `daukle.host`, `daukle.env`, `daukle.log` and
`daukle.include`. It does **not** get `io`, `os`, `require` or `load`, so cloning a repository and
running `daukle check` cannot read your files or start a process.

## What this example cannot show

**That the logic layer scales to a real build script.** Everything here fits on one screen, and
the interesting question is what a hundred line `daukle.lua` looks like, which no project in this
org has yet.

**Reading anything.** The sandbox has no `io`, so the Lua cannot look at the producer's manifest
and enumerate its modules, which is the obvious next thing a reader will try. It computes from
literals, `daukle.host` and `daukle.env`, and that is the whole input surface.

**`daukle.include`.** The other half of the layer, which pulls in a second Lua file, is how a
resolver's generated pins reach a manifest without being pasted into it. `daukle/maven` uses it
and nothing here does.

## What it demonstrates

- the `daukle.lua` logic layer, which nothing else in this repository shows
- `daukle/path` resolving a producer manifest from a directory
- `daukle/npm` writing the result into a `package.json` it does not own
