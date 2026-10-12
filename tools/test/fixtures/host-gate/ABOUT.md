# host-gate

A fixture whose needs-tools NAMES the hosts it runs on. The test suite rewrites that file per case,
because which hosts are the wrong ones depends on the runner: the same fixture has to prove the
allowed host, the refused host and the unrecognised name.

```console
$ daukle check
daukle: in sync
```
