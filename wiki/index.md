# daukle

A build tool that is **only a plugin API**. Core knows nothing about any language, compiler or
build system: it reads a manifest, acquires plugins, and runs what they declare. Every ability in
this site arrives through one of the plugins in the sidebar.

## A project

A `daukle.toml` and your sources are the whole of it: no build file, no wrapper properties and
nothing generated at the root. [[daukle/01-first-project]] builds one from scratch and is where
the manifest that used to be copied out here now lives.

## Getting daukle

**A project never needs an installed daukle.** Three committed files are what make it own none:
`daukle`, `daukle.ps1` and `.daukle/wrapper.toml`. The first run fetches the exact binary the pin
names and verifies it against a sha256 before running it:

```sh
./daukle sync
```

### Starting from nothing

All three files are release assets, so getting them needs something that is not daukle. There is no
installer: you download three text files, read them, and commit them.

```sh
base=https://github.com/daukle/daukle/releases/latest/download
mkdir -p .daukle
curl -fsSL -o daukle "$base/daukle"
curl -fsSL -o daukle.ps1 "$base/daukle.ps1"
curl -fsSL -o .daukle/wrapper.toml "$base/wrapper.toml"
chmod +x daukle
./daukle init
```

```powershell
$base = 'https://github.com/daukle/daukle/releases/latest/download'
New-Item -ItemType Directory -Force .daukle > $null
Invoke-WebRequest "$base/daukle" -OutFile daukle
Invoke-WebRequest "$base/daukle.ps1" -OutFile daukle.ps1
Invoke-WebRequest "$base/wrapper.toml" -OutFile .daukle/wrapper.toml
.\daukle.ps1 init
```

**This is deliberately not `curl | sh`.** The pin is a version and a digest per host a reviewer reads
before anything executes, and piping a script into a shell would throw that away at the one moment
it is worth most. Commit all three files; `./daukle wrapper update [version]` replaces them later,
and it is explicit rather than automatic so that daukle's own bootstrap never becomes the one thing
in the system that updates itself.

**It is also the one unpinned step a project ever takes.** Whatever downloads the wrapper has no
digest to check it against yet, because the digests are what it is downloading. Everything after it
is pinned: the wrapper refuses a binary whose sha256 does not match the pin, and inside daukle there
is exactly one unpinned fetch, it belongs to the resolver, and it is refused outside an explicit
`--resolve` run.

### Which route works on which host

A release carries the two wrapper scripts, the pin, and **three platform binaries** covering four
hosts, because one of them serves both Macs. The wrapper composes a host string from the operating
system and the architecture and looks it up in the pin, so a host with no entry is refused by name
rather than failing on a download later:

| host | binary | wrapper | build from source |
| --- | --- | --- | --- |
| linux/x86_64 | `daukle-linux-x86_64`, statically linked | yes | yes |
| macos/aarch64 | `daukle-macos-universal` | yes | yes |
| macos/x86_64 | `daukle-macos-universal`, the same file | yes | yes |
| windows/x86_64 | `daukle-windows-x86_64.exe` | yes | yes |
| linux/aarch64 | none published | refused by name | yes |
| windows/aarch64 | none published | refused by name | yes |

**Both Macs are served by one universal binary** rather than two downloads, which is why the two
rows name the same file. A host with no row at all is refused by name before anything is
downloaded, and building from source needs a C compiler, CMake and libcurl's development headers.

A POSIX shell on Windows (Git Bash, MSYS2, Cygwin) runs the Windows binary through `./daukle`,
rather than being sent to `daukle.ps1`.

**There is no Homebrew, winget, Scoop or apt package, and that is a decision rather than a gap.**
Each ecosystem would be another place the version can go stale, and the wrapper already means a
project never needs an installed daukle at all, so the only audience for a package is somebody
evaluating daukle outside a project. One downloaded binary serves that for a fraction of the
permanent cost.

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

Described in full on [[daukle/04-dependencies-and-compiling]], which is where the table that used
to sit here went. The rule it states is that a file you are not supposed to edit never
sits at the project root.

## The logic layer

Described in full on [[daukle/03-logic-layer]], which is where the two sentences that used to sit
here went. Two descriptions of one subject written days apart is how both of them
go stale.

## What core deliberately does not know

Any language, any compiler, any build system, and what `build` means. The last one is a plugin
too: `lifecycle` owns the standard task vocabulary so that core reserving those names never
becomes the one hardcoded language concept.
