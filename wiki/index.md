# daukle

A build tool that is **only a plugin API**. Core knows nothing about any language, compiler or
build system: it reads a manifest, acquires plugins, and runs what they declare. Every ability in
this site arrives through one of the plugins in the sidebar.

## A project

```toml
schema = 1
project = "example/greeter"
version = "1.0.0"

[modules]

[plugins]
java = "daukle/java@^1"

[toolchains.java]
sourceRoot = "src/main/java"
main = "example.Main"
```

That file and your sources are the whole project. There is no build file, no wrapper properties
and nothing generated at the root.

## Getting daukle

A release carries three platform binaries plus a **wrapper**, so a project never needs an installed
daukle. Commit the wrapper and its pin, and the first run fetches the exact version the pin names:

```sh
./daukle sync
```

The wrapper verifies the binary against a sha256 before running it. Every acquisition in daukle is
pinned the same way: there is exactly one unpinned fetch in the whole system, it belongs to the
resolver, and it is refused outside an explicit `--resolve` run.

## Commands

| command | what it does |
| --- | --- |
| `sync` | bring the project into line with its manifest |
| `check` | report whether it is in sync, changing nothing |
| `tasks` | list the tasks the plugins declare |
| `<task>` | run one by the name `tasks` lists |
| `clean` | remove what sync generated |
| `init` | write a starter `daukle.toml` |
| `plugin update` | refetch plugins, ignoring the cache |

`--resolve` allows a task to fetch an unpinned url and report its digest. `--no-cache` refetches.

## Where things are written

Described in full on the page *Dependencies and compiling*, in this wiki, which is where the table
that used to sit here went. The rule it states is that a file you are not supposed to edit never
sits at the project root.

## The logic layer

Described in full on the page *The logic layer*, in this wiki, which is where the two sentences
that used to sit here went. Two descriptions of one subject written days apart is how both of them
go stale.

## What core deliberately does not know

Any language, any compiler, any build system, and what `build` means. The last one is a plugin
too: `lifecycle` owns the standard task vocabulary so that core reserving those names never
becomes the one hardcoded language concept.
