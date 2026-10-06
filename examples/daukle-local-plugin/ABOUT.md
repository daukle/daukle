# daukle-local-plugin

A plugin that lives inside the project it serves, as a single Lua file named by a path rather than
by a published coordinate. It is the shape every daukle plugin starts as: you write
`plugins/banner.lua`, point `[plugins]` at it, and the toolchain it declares is available
immediately with nothing published and nothing fetched.

```console
$ daukle sync
daukle: updated
$ cat build/daukle/banner/banner.h
#define BANNER "good morning from daukle/example-local-plugin"
```

## What to look at

**`daukle.plugin` must be the first call in the chunk.** The pre-pass that reads `uses` runs before
anything else, so a `daukle.require` above that line is refused rather than quietly accepted.

**`generate` is a pure function of the manifest.** It returns a table of relative path to file
text and daukle performs every write, which is why the plugin sandbox has no write verb at all.
It may not call `exec` or `tool`, because `daukle check` runs `generate` and a check must never
start your compiler.

**The generated file lands under `build/daukle/banner/`, never at the project root.** That is the
file-location rule: anything the user is not supposed to edit does not sit where they will find it.

**The block is `context.config` in `generate` and `context.toolchain.config` in a task**, which is
the one confusion worth knowing about before you write a second plugin. Real plugins keep a
one-line `config_of` helper so the two cannot be mixed up.

**Naming a path here is correct rather than a shortcut.** Every other example in this organization
names a published coordinate so that it is self contained; this one is *about* a plugin you have
not published, and it stays self contained because the plugin travels inside the example. Give the
same file a coordinate and nothing in the plugin itself changes.

## What this example cannot show

**A plugin of more than one file.** That needs `daukle.require` and a `lib/` directory, and an
`exports` list in the `daukle.plugin` call saying which of them a consumer may load. Every plugin
in this organization larger than this one is built that way.

**A plugin acquired by coordinate.** Publishing turns the path into a resolver and a coordinate,
which is how every other example here names its plugins. Nothing in `banner.lua` would change.

**Anything that runs a program.** This toolchain declares no task and `daukle tasks` reports that
the project has none, because a task needs `exec` or `provision` in the chunk's `uses`. A chunk
holding either of those may not also declare a language, which is why `daukle/node` and
`daukle/npm` are two repositories rather than one.

**A toolchain that provisions its own tool.** Downloading a compiler, verifying it against a pinned
digest and running it is the larger half of what a real toolchain does, and none of it appears here.
