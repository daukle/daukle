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
| `migrate` | turn a project built by another tool into a daukle project |
| `migrate --done` | delete the old tool's files a finished migration kept |

`--resolve` allows a task to fetch an unpinned url and report its digest. `--no-cache` refetches.

## Migrating a project built by another tool

`daukle migrate` runs in a directory that holds no `daukle.toml` or `daukle.lua`, and refuses one
that does. It knows no build tool. It reads **catalogs**, lists of plugins each with the files that
make a project theirs, detects which of those plugins the directory needs, and asks each one for the
project's model. Where a plugin answers with something daukle cannot express, it writes nothing and
prints every such blocker, grouped by who reported it.

When nothing blocks, it writes `daukle.toml`, builds the project with the old tool and with daukle,
compares the artifacts each migrator named, and only then moves the old tool's files to
`build/daukle/migrate/old`. Run `daukle migrate --done` to delete them once you are satisfied.

| exit | meaning |
| --- | --- |
| 0 | migrated; the old files are in `build/daukle/migrate/old` |
| 3 | blocked; nothing was written |
| 4 | the proof failed; `daukle.toml` and the migrator's files are kept for inspection and nothing was moved |
| 1 | an error in migrate itself, such as a fetch, a plugin error or a malformed catalog |

- **`--catalog <location>`** adds a catalog, an https url or a path, and can be repeated. A catalog
  pins the plugin code migrate runs, so a plain `http://` url is refused unless its host is
  `127.0.0.1`, `localhost` or `[::1]`. The same holds for `DAUKLE_CATALOGS`.
- **`--with <id>`** uses the catalog plugin with that id at the project root even if no rule matched,
  and can be repeated. An id no catalog lists is an error.
- **`DAUKLE_CATALOGS`** holds whitespace-separated locations. Each `--catalog` is added after them.
  With no location at all, migrate stops and says so.
- **The wrapper** sets `DAUKLE_CATALOGS` from the `catalogs` list in `.daukle/wrapper.toml` when the
  variable is unset or empty. A `DAUKLE_CATALOGS` you set yourself replaces that default, and a pin
  with no `catalogs` line leaves the variable alone.
- **`migrate --done`** takes no `--catalog` or `--with`, and fails when there is nothing to finish.
- **`daukle clean` refuses** while `build/daukle/migrate/old` exists, because that directory holds
  the only copy of what the migration replaced.

A catalog is read at run time and never cached. It is a JSON document with a `plugins` list; each
entry has an `id`, an `asset` of `{ url, sha256 }` that migrate pins into the `[plugins]` table of
the manifest it writes, `detect` rules of `files`, optional `contains` patterns and a `scope` of
`root` or `tree`, and a `replaces` map of tool, then kind, then foreign plugin ids. An optional
`skip` is a list of directory names the detection walk never enters, at any depth; it is the union
across every plugin of every catalog loaded, so one entry can keep installed dependencies out of
all the rules. A malformed entry is dropped with a warning, and an id listed twice keeps the
earlier catalog's entry. A plugin with no `asset` that migrate would need is a blocker, since there
is nothing to pin.

The plugin side is on [[daukle/02-plugins]].

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
