# daukle-logic-layer

A `daukle.lua` beside `daukle.toml`, computing a toolchain's block that a static manifest could
not state. The toolchain is a local plugin, `plugins/banner.lua`, that generates one header from
its block.

```console
$ daukle sync
daukle: updated
$ cat build/daukle/banner/banner.h
#define BUILD_MODE "release"
#define FEATURE_COUNT 2
#define FEATURE_METRICS 1
$ DAUKLE_EXAMPLE_DEV=1 daukle sync
daukle: updated
$ cat build/daukle/banner/banner.h
#define BUILD_MODE "debug"
```

## What to look at

**`daukle.toml` holds no `[toolchains.banner]` table.** `daukle.lua` writes it into
`daukle.config` after the TOML is read, so the manifest daukle acts on is the TOML plus whatever
the script set.

**The features are named once.** The header carries a count and one flag per feature, both
derived from the one list, so adding a feature cannot leave the count behind.

**The same checkout builds two ways.** `DAUKLE_EXAMPLE_DEV=1` turns the release banner into a
debug one with no second manifest. `daukle check` run without the variable after a debug sync
reports the header out of date, which is correct: the manifest it reads is a release one.

## What this example cannot show

**Anything the sandbox withholds.** The script sees literals, `daukle.host` and `daukle.env`; it
cannot read files or start a process, so it cannot discover the features from the source tree.
