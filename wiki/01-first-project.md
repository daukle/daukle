# 1. Your first project

**Every file quoted on this page is a real file in some repository of this organization, and the
site build fails if a quote here stops matching it.** That is the whole reason these pages live in
repositories rather than in a wiki: a snippet nobody runs is a claim, and this project has spent
long enough removing claims.

The example on this page is `npm-dependency-ledger`, and it lives in **`daukle/npm`** rather than
here, because an example lives in the repository of the thing it demonstrates. Each fence below
names the repository it is quoting. You can copy the example anywhere and run it.

## What daukle is

daukle is **a plugin API and nothing else**. It knows no languages, ships no compiler and has no
build logic. Everything it can do for you arrives as a plugin, which is a Lua file it fetches and
pins by digest.

That has one consequence worth internalising before anything else: **the manifest declares, and
daukle acts**. You do not write build steps.

## The manifest

```toml file=daukle/npm:examples/npm-dependency-ledger/daukle.toml
schema = 1
project = "daukle/example-dependency-ledger"
version = "1.0.0"

[modules]

# Where the producer's manifest comes from. daukle/path reads
# <path>/daukle.toml, which is the shape to use while a producer publishes no
# release asset of its own; daukle/github reads one from a release.
[sources."example/greeter"]
kind = "path"
path = "./producer"

[resolvers.github]
url = "https://raw.githubusercontent.com/daukle/daukle/1f515141e06d16eace718a45944a39933611fa39/plugins/github-releases.lua"
sha256 = "3789865c266eb53fafcf7300d77a24461170c615195d92d0ddf88078c556e23e"

[plugins]
path = { resolver = "github", coordinate = "daukle/path@^1.0.0" }
npm = { resolver = "github", coordinate = "daukle/npm@^1.0.0" }

[[consumers]]
id = "node"
language = "npm"
file = "package.json"
configuration = "dependencies"

  [consumers.dependencies."example/greeter"]
  version = "^2.0.0"
  modules = ["cli"]
```

Four things are going on, and they are the four concepts the rest of the guide builds on.

**A source** says where a dependency's description comes from. Here it is a directory; it could be
a GitHub release.

**A resolver** turns a coordinate into a url. It is itself a plugin, pinned by digest, because the
thing that decides what gets downloaded is the last thing you want unpinned.

**A plugin** is an ability. `daukle/npm` can write a `package.json`; `daukle/path` can read a
producer from a directory. Without them this manifest does nothing at all.

**A consumer** is a file daukle keeps in step. It is not a file daukle owns: your `package.json`
keeps everything you put in it.

## The producer

The thing being depended on describes itself, once, for every language:

```toml file=daukle/npm:examples/npm-dependency-ledger/producer/daukle.toml
schema = 1
project = "example/greeter"
version = "2.0.0"

# One module, and what it is called on npm. A producer states this once and
# every consumer language reads the same table: the npm name and range live
# here rather than in each consumer's own build file.
[modules.cli]

  [modules.cli.npm]
  package = "cowsay"
  range = "^1.6.0"
```

**This is the point of the indirection.** The consumer asks for `example/greeter`'s `cli` module.
It never names `cowsay`. If the greeter renames its npm package, consumers do not change.

## Running it

```sh
daukle sync     # bring the project into line with the manifest
daukle check    # exit non-zero if a sync would do work
```

`sync` writes; `check` changes nothing and tells you whether a sync would. `check` is what belongs
in CI.

## What you get

Starting from a `package.json` holding only your own `left-pad`:

```json file=daukle/npm:examples/npm-dependency-ledger/expected/package.json
{
  "name": "example-dependency-ledger",
  "private": true,
  "dependencies": {
    "left-pad": "^1.3.0",
    "cowsay": "^1.6.0"
  },
  "daukle": {
    "managed": {
      "dependencies": [
        "cowsay"
      ]
    }
  }
}
```

**`left-pad` is untouched.** daukle added `cowsay` and wrote down, under `daukle.managed`, that
`cowsay` is the line it owns. Remove the dependency from the manifest and the next sync removes
exactly that line and nothing else. **That ledger is why daukle can edit a file it does not own
without ever deleting something you wrote.**

## What this page does not cover

**Compiling anything.** This project writes a dependency into a file; it provisions no toolchain
and runs no compiler. Page 4 does that.

**Why the plugins are pinned the way they are.** Page 2.

## Next

- [[daukle/02-plugins]]
- [[daukle/03-logic-layer]]
- [[daukle/04-dependencies-and-compiling]]
