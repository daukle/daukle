# daukle-wrapper-bootstrap

The one example run by an INSTALLED daukle rather than the one the harness compiled: `daukle init`
writes the wrapper with no network, and from then on the project's own `./daukle` downloads the
pinned daukle, checks it against a digest and runs it.

```console
$ daukle init
daukle: wrote .daukle/wrapper.toml
$ ./daukle sync
in sync
```

PowerShell runs the second line as `.\daukle.ps1 sync`.

## What to look at

**This directory commits no wrapper.** `init` writes all three files: the two scripts are compiled
into the binary, and `.daukle/wrapper.toml` is the pin the release ships beside it, so the project
pins exactly the daukle that ran `init`. `daukle.toml` already exists here, so `init` leaves it alone
and writes only the wrapper.

**`uses-release` puts the latest release's archive on the harness's PATH**, unpacked for the host,
in place of the build under test. That is what "installed" means here: the archive is what every
install channel carries, a binary and its `wrapper.toml` side by side.

**No version is asserted.** The wrapper's digest check is what proves the binary is the pinned one,
and a version in this file was one more thing a release had to come back and edit. It was left red
by `0.3.0` and again by `0.4.0`.

**The manifest is the smallest in this repository on purpose.** What is being demonstrated is the
bootstrap, so the manifest acquires one plugin by coordinate and stops.

## What this cannot show

- **It covers every cell that exists, and that is only three.** CI runs it on `ubuntu-latest`,
  `windows-latest` and `macos-latest`, the last of them arm64. `linux/aarch64` and
  `windows/aarch64` are unpublished, so nothing here says what the wrapper does on them beyond
  refusing them by name.
- **It cannot show a first-run failure mode.** The hostile cases, a wrong digest and a host with no
  downloader, live in `daukle/daukle`'s `wrapper/test/probe.sh` inside a deliberately bare image.
- **It runs the release, not `development`**, so an `init` change shows here only once released.
  `cli_commands` under ctest holds the unreleased `init`.
