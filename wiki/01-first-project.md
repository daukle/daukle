# 1. Your first project

**Every file quoted on this page is a real file in some repository of this organization, and the
site build fails if a quote here stops matching it.** That is the whole reason these pages live in
repositories rather than in a wiki: a snippet nobody runs is a claim, and this project has spent
long enough removing claims.

The example on this page is `java-hello-jar`, and it lives in **`daukle/java`** rather than here,
because an example lives in the repository of the thing it demonstrates. You can copy it anywhere
and run it.

## What daukle is

daukle is **a plugin API and nothing else**. It knows no languages, ships no compiler and has no
build logic. Everything it can do for you arrives as a plugin, which is a Lua file it fetches and
pins by digest.

That has one consequence worth internalising before anything else: **the manifest declares, and
daukle acts**. You do not write build steps, and you keep no tool file: no `build.gradle`, no
`pom.xml`, no `package.json`, no `CMakeLists.txt`. `daukle.toml` is the only file of its kind in
the repository.

## The manifest

```toml file=daukle/java:examples/java-hello-jar/daukle.toml
schema = 1
project = "daukle/example-java-hello-jar"
version = "1.0.0"

[modules]

[resolvers.github]
url = "https://raw.githubusercontent.com/daukle/daukle/1f515141e06d16eace718a45944a39933611fa39/plugins/github-releases.lua"
sha256 = "3789865c266eb53fafcf7300d77a24461170c615195d92d0ddf88078c556e23e"

[plugins]
java = { resolver = "github", coordinate = "daukle/java@^1.0.0" }

[toolchains.java]
version = "21"
main = "com.example.Main"
```

Three things are going on, and they are the concepts the rest of the guide builds on.

**A resolver** turns a coordinate into a url. It is itself a plugin, pinned by digest, because the
thing that decides what gets downloaded is the last thing you want unpinned.

**A plugin** is an ability. `daukle/java` can provision a JDK, compile and package. Without it this
manifest does nothing at all.

**A toolchain** is a plugin put to work on this project. `[toolchains.java]` is its configuration:
which JDK, and which class to run. The plugin provisions that JDK itself, verified against a digest
it pins, so nothing has to be installed first.

## Running it

```sh
daukle sync       # bring the project into line with the manifest
daukle check      # exit non-zero if a sync would do work
daukle java:run   # compile and run
```

`sync` writes; `check` changes nothing and tells you whether a sync would. `check` is what belongs
in CI. `daukle tasks` lists every task the plugins declared and their order.

## What you get

Everything daukle generates or builds lands under `build/daukle/<toolchain>/`, never at the
project root: `daukle java:jar` packages `build/daukle/java/example-java-hello-jar.jar`. **A file
you are not supposed to edit never sits where you would find it**, and deleting `build/` loses
nothing the next run cannot recreate from `daukle.toml`.

## What this page does not cover

**Dependencies.** This project depends on nothing. Page 4 adds one, pinned by digest, and shows
the resolver that writes the pins for you.

**Why the plugins are pinned the way they are.** Page 2.

## Next

- [[daukle/02-plugins]]
- [[daukle/03-logic-layer]]
- [[daukle/04-dependencies-and-compiling]]
