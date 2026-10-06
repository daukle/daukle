# 4. Dependencies and compiling

This page compiles and runs a real Java program against a real dependency, with **no JDK
installed**, and then shows what it costs and how to stop paying it.

The example on this page is `java-pinned-classpath`, in the `daukle/java` repository, because a
classpath belongs to that toolchain rather than to core.

## Compiling with no toolchain installed

```toml file=daukle/java:examples/java-pinned-classpath/daukle.toml
schema = 1
project = "daukle/example-pinned-classpath"
version = "1.0.0"

[modules]

[resolvers.github]
url = "https://raw.githubusercontent.com/daukle/daukle/1f515141e06d16eace718a45944a39933611fa39/plugins/github-releases.lua"
sha256 = "3789865c266eb53fafcf7300d77a24461170c615195d92d0ddf88078c556e23e"

[plugins]
java = { resolver = "github", coordinate = "daukle/java@^1.0.0" }

[toolchains.java]
version = "17"
main = "example.Main"

# A dependency, by hand. Every classpath entry is a url and the sha256 of the
# bytes that url must serve: there is no unpinned form. Writing these out is
# exactly what daukle/maven exists to stop you doing, and page 4 says so.
[[toolchains.java.classpath]]
url = "https://repo1.maven.org/maven2/org/slf4j/slf4j-api/1.7.36/slf4j-api-1.7.36.jar"
sha256 = "d3ef575e3e4979678dc01bf1dcce51021493b4d11fb7f1be8ad982877c16a1c0"
as = "slf4j-api 1.7.36"
```

```sh
daukle java:run
```

**`version = "17"` provisions a JDK.** daukle downloads one for your OS and architecture, pins it
by digest, caches it by content and runs `javac` out of it. Nothing is installed on the machine
and nothing is on your `PATH`. A different project on the same machine can use a different JDK
and neither knows about the other.

`java:run` prints `hello from example.Main`, which is what CI asserts.

## The part that does not scale

That manifest pins **one** jar, and it had to state the url and the digest. A real library does
not have one dependency.

Measured on `intisy/libs/java-utils`, a real project: **eight declared coordinates resolve to
twenty jars.** Twelve of the twenty appear in no file in the repository at all; they are reached
only transitively. Transcribing those by hand means finding twenty urls, computing twenty digests
and settling three version conflicts, and nothing tells you a conflict is due.

**This is what `daukle/maven` is for.**

## Resolving instead

```sh
daukle maven:resolve --resolve
```

`--resolve` is not decoration. `daukle.pin`, the verb that fetches something nobody has a digest
for yet, is **refused without it**, and the message says so. Every other acquisition in daukle is
pinned before it happens; this is the one that computes the pin, so it is the one that has to be
asked for explicitly. An ordinary `daukle sync` can never reach it.

What comes out is a generated Lua file of twenty pinned entries, and you include it:

```lua
-- daukle.lua
daukle.include("daukle/maven/classpath.lua")
```

**Your `daukle.toml` never grows the twenty blocks.** Generated data lives in a generated file;
the file you wrote keeps only what you wrote.

## Two directories, and why there are two

| directory | holds | committed |
| --- | --- | --- |
| `build/daukle/<toolchain>/` | what a tool produced on the way to an artifact | no, and `daukle clean` deletes it |
| `daukle/<toolchain>/` | what a RESOLVE produced | **yes** |

They have opposite lifetimes, which is the whole reason they are not one directory. Build output
is rebuilt from nothing. Resolved pins must survive a clone, because **a fresh clone may not
fetch anything unpinned**, so without them it cannot build at all.

Neither is the project root. If you are not meant to edit it, it does not sit where you work.

## What this page does not cover

**The resolve is not exercised by this repository's CI**, and that is a real gap rather than an
omission: `daukle/maven` has no release yet, so a project here cannot name it by coordinate, and
a multi-file plugin cannot be pinned by a single url. The resolver IS exercised, by
`daukle/maven`'s own suite on three runners, including the twenty-module closure and the two
conflict rules. This page will run it here once that plugin is released.

**Tests, a sources jar and javadoc.** `daukle/java` owns the first two; javadoc it does not.

**Anything about `daukle/gradle`.** A Gradle project is the case daukle most wants to take over
and the plugin for it is not built.

## Next

Back to [1. Your first project](01-first-project.md), or read `daukle/examples` for the
cross-plugin cases and each plugin's own `AUTHORING.md` for its full surface.
