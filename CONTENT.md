## What daukle is

A build tool that is **only a plugin API**. Core knows nothing about any language, compiler or
build system. It reads a manifest, acquires the plugins that manifest names, and runs what they
declare; every ability arrives through a plugin, and a language daukle has never heard of needs a
plugin rather than a change to core.

A project holds `daukle.toml` and its sources. **No build file, no wrapper properties and nothing
generated at the project root**, because a file you are not supposed to edit does not sit where you
will find it.

## You do not install it

A release ships three platform binaries plus a **wrapper**, so a project never needs an installed
daukle. Commit the wrapper and its pin and the first run fetches the exact version the pin names,
verifying it against a sha256 before running it:

```sh
./daukle sync
```

That closes the bootstrap daukle otherwise has: before it, a project using daukle to avoid
installing CMake had to install CMake to build daukle.

## Everything is pinned, and there is exactly one exception

Every acquisition in the system is pinned by sha256: plugins, provisioned toolchains, dependencies.
**There is one unpinned fetch in the whole of daukle**, it belongs to the resolver, and it is
refused outside an explicit `--resolve` run. That is what makes a clone reproducible without a lock
file at the root.

## What core deliberately does not know

Any language, any compiler, any build system, and what `build` means. The last of those is a
plugin, `daukle/lifecycle`, which registers a vocabulary and nothing else.

## Where the rest is

`wiki/` holds the documentation, rendered with every plugin's at
<https://daukle.github.io/guide/>: start at *Your first project*. `STRUCTURE.md` maps the source
tree, `examples/` holds projects that run and are executed in CI, and `tools/run-examples.sh` is
the example harness every repository in the organization uses.
