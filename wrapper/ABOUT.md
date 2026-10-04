# The daukle wrapper

A project that uses daukle should not require daukle to be installed first. These three files are
how: committed to a project root, they download a pinned daukle on first use, verify it against a
digest and run it.

```
cp wrapper/daukle wrapper/daukle.ps1 <project>/
cp -r wrapper/.daukle <project>/
cd <project> && ./daukle sync
```

On Windows, `.\daukle.ps1 sync`.

## What is committed, and what is not

| file | what it is |
| --- | --- |
| `daukle` | POSIX shell, mode `100755` |
| `daukle.ps1` | PowerShell |
| `.daukle/wrapper.toml` | the version and one digest per host |

**No binary is committed.** That is the one thing this does differently from `gradlew`, and the
reason is concrete: `gradlew` needs `gradle-wrapper.jar` because only JVM code can download in a
JVM world, so every Gradle repository carries an unverified binary and the org needs a separate
check to validate it. daukle is a native program, so the download and the digest check are things
the host's shell already does. The pin is a line of text a reviewer can read.

**An upgrade is a one-file diff.** The version and the digests live in `.daukle/wrapper.toml` and
nowhere else, so bumping daukle never touches a script.

**The canonical pin is written by a release, not by a person** (`D-67`). Digests exist for the
first time in the release job, once every cell has uploaded, so `cmake/write_wrapper_pin.cmake`
runs there and the result is published as the `wrapper.toml` asset beside `daukle` and
`daukle.ps1`. The copy in this directory is held to the latest published release by the ctest entry
`wrapper_pin_matches_release`, which asks the forge rather than reading a local file.

That is what makes a stale pin fail. Before it, nothing did: the probe tests whatever the pin says
and `daukle/examples` pinned the same version, so a forgotten bump stayed green everywhere. The
host-to-asset mapping is still hand written, because nothing in `daukle-macos-arm64` says it serves
`macos/aarch64`, and an asset no mapping names now refuses the release.

## This does not break the no-files-at-the-root rule

daukle's hardest constraint is that it writes no generated files to a project root, and a
Gradle-style wrapper is three files at a project root. The collision is apparent rather than real:
**the rule forbids daukle WRITING files there, not the project HAVING them.** `daukle.toml` sits at
the root too, authored and committed by you. The wrapper is the same kind of object, and daukle
never touches it at run time.

What the rule does bind is the wrapper's *output*, and the downloaded binary goes to the cache,
never beside your sources.

## What the host must supply

Measured on a bare `ubuntu:24.04` with nothing installed, which has `sha256sum`, `tar` and `gzip`
and **no downloader at all**:

- **`curl` or `wget`.** Neither is present on a minimal image, and a hosted CI runner would never
  show you that. With neither, the wrapper names the package to install rather than reporting
  `command not found`.
- **`sha256sum`, or `shasum` on macOS**, where `sha256sum` is not standard. PowerShell has
  `Get-FileHash` built in, so Windows needs no fallback.
- Nothing else. The published assets are bare binaries, so there is no unpacking step and `tar`,
  `gzip` and `unzip` are not required.

**A trust store comes with the downloader**, so it is not a separate prerequisite: installing
`curl` installs `ca-certificates`, and Alpine's busybox `wget` verifies through `ssl_client`
against anchors the base image already carries. Both were measured. Integrity does not rest on
that anyway, because the digest is checked after the download regardless of how it arrived.

## What happens when something is wrong

| situation | what you get |
| --- | --- |
| host not in the pin | the host named, and the cells this release does cover |
| no `curl` and no `wget` | the packages to install, per distribution |
| digest does not match after download | both digests printed, the file deleted, nothing run |
| cached file does not match | treated as a corrupt cache and downloaded again |
| no pin file | the path it was looked for at |

The last two are deliberately different. A cached file that fails its digest is almost always a
truncated download, so replacing it is right. A **fresh** download that fails its digest is the pin
doing its job, so it is fatal.

## Line endings, which bite twice

**The POSIX script must reach a POSIX host with LF endings.** A shell reads the carriage return as
part of the first line and answers `end of file unexpected`, naming a line far from the problem.
This repository pins `wrapper/daukle`, `*.sh` and the pin file to `eol=lf` in `.gitattributes`,
because `core.autocrlf` is on where this was written and a branch switch reintroduced CRLF into
files that had been committed LF.

**The pin file is read defensively anyway**, because you will commit these three files into your
own repository under your own git settings and this one cannot reach them. A CR on a section
header used to make the lookup miss and then report the host it had just been handed: "no
published daukle for linux/x86_64. This release covers: linux/x86_64". The parser strips it, and
`test/probe.sh` holds it to that with a deliberately CRLF pin.

## Where the binary lands

In daukle's own cache, honouring `DAUKLE_CACHE_DIR`, then `XDG_CACHE_HOME`, then
`$HOME/.cache/daukle`, and `%LOCALAPPDATA%/daukle/cache` on Windows, under
`wrapper/<version>/<os>/<arch>/`.

**That rule now exists in three places**: `cache_root_dir` in `src/cache/cache.c`, and once in each
script. It is a real duplication and the likeliest way the wrapper drifts from core. It is written
out in both scripts rather than left to be discovered, and no check enforces it, because a check
that compares the variable NAMES would pass a change to what they mean, which is the drift that
would actually hurt.

## How this was verified

`test/probe.sh` runs fifteen checks against the **real published release** inside a bare
`ubuntu:24.04`:

```
docker run --rm -v "//$PWD/wrapper:/w:ro" -v "//$PWD/wrapper/test:/s:ro" ubuntu:24.04 sh /s/probe.sh
```

It is not a ctest entry, because it needs Docker and the network and ctest has neither. The two
checks that delete `curl` and `wget` run LAST: `apt-get` will not restore a binary that was
deleted rather than uninstalled, and an earlier removal silently starves every later check, which
cost one confusing run.

**A consumer exercises it on every platform.** `daukle/examples`' `wrapper-bootstrap` is a
project that owns no daukle: its harness runs it through this wrapper on all three runners,
byte compares its copy of these scripts against the pair here, and fails if the version the
wrapper fetched is not the one pinned. That is where the macOS branch runs.

Run by hand beyond that script: the `wget` fallback and `busybox ash` on `alpine:3.21`, and both
PowerShell paths on Windows, where the pin refusal and the exit-code propagation were checked
natively.

## What this cannot show

- **Three cells, not six.** `linux/x86_64`, `macos/aarch64` and `windows/x86_64` are published.
  `linux/aarch64`, `macos/x86_64` and `windows/aarch64` are not, and the wrapper refuses them by
  name. Hosted runners cover the three that exist.
- **Every published cell is now exercised**, as of 2026-10-04, which was not true when this
  file was written. `daukle/examples`' `wrapper-bootstrap` runs the wrapper on all three
  runners, and `macos-latest` is arm64, so `Darwin`, the `shasum -a 256` fallback and an
  aarch64 asset are covered. What remains unexercised is the two cells that **do not exist**,
  `linux/aarch64` and `windows/aarch64`, and there the only behaviour is the refusal.
- **No proxy, no mirror, no private release.** The URL is hardcoded to this repository's releases
  and there is no override, because an override is a second place a pin can be defeated.
- **It does not upgrade itself.** Nothing here checks whether a newer daukle exists; the pinned
  version is the version, until a person edits the pin. That is a refusal rather than a gap: an
  auto-updating wrapper would make daukle's own bootstrap the one unpinned thing in a system where
  every other acquisition carries a sha256. An explicit `wrapper update` is `D-68`, and the release
  assets it needs exist as of `D-67`.
