# daukle-wrapper-bootstrap

The only example here that does not need daukle installed. Every other one is run by a daukle the
harness compiled from source; this one is run by a daukle that its own `./daukle` downloaded,
checked against a digest and executed.

```console
$ ./daukle --version
daukle 0.6.0
```

PowerShell runs the same thing as `.\daukle.ps1 --version`, and `./daukle sync` is the ordinary
command a project would use.

The first run fetches the pinned binary into the daukle cache. Every run after that is a digest
check and an `exec`.

## What to look at

**Three files and no binary.** `daukle`, `daukle.ps1` and `.daukle/wrapper.toml` are committed,
readable and reviewable. The pin is a digest in a text file, which is the thing `gradlew` cannot
do: it needs `gradle-wrapper.jar` because only JVM code can download in a JVM world, so every
Gradle repository carries an unverified binary. daukle is a native program, so the host's own
shell does the download and the check.

**Copy all three, not two.** `.daukle/wrapper.toml` carries the version and one digest per host,
and it is the only file an upgrade touches. A missing pin is reported by path rather than guessed
at.

**This example pins `0.6.0`, so it runs the RELEASE and not `development`.** That is the one way
it differs from its neighbours, and it is the point rather than an oversight: a project using the
wrapper is pinned to a published daukle until someone edits one line. The console block above
holds the harness to it, so if the wrapper ever runs something other than the pinned version this
example fails rather than passing quietly. **That version is a file a core release has to follow**,
which is exactly what went wrong when `0.3.0` AND `0.4.0` were published and left this
example red, twice.

**The manifest is the smallest in this repository on purpose.** What is being demonstrated is the
bootstrap, so the manifest acquires one plugin by coordinate and stops. Anything more would be a
second example wearing this one's name.

**All three files here are a COPY, and a test holds them to it.** They are byte compared against
`wrapper/` in this same repository by the `wrapper_example_matches_wrapper` entry under ctest, so a
copy that drifts fails rather than demonstrating something nobody ships. **The pin is compared too,
which it could not be before.** While the example lived in another repository its pin could
legitimately name an older release than core's working tree, so there was no right answer and the
comparison was skipped; in one repository a release updates both in the same commit.

## What this cannot show

- **It covers every cell that exists, and that is only three.** CI runs it on
  `ubuntu-latest`, `windows-latest` and `macos-latest`, and the last of those is arm64, so
  this example is where the wrapper's macOS branch and its only aarch64 asset actually run.
  `linux/aarch64` and `windows/aarch64` are unpublished, so nothing here says what the
  wrapper does on them beyond refusing them by name.
- **It cannot show a first-run failure mode**, because by the time CI reaches a second runner the
  release is warm in no shared cache but the network is the same network. The hostile cases, a
  wrong digest and a host with no downloader, live in `daukle/daukle`'s `wrapper/test/probe.sh`
  inside a deliberately bare image.
- **It says nothing about upgrading.** Nothing here checks whether a newer daukle exists; the
  pinned version is the version until a person edits the pin.
