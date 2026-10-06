# crlf-block

A fixture whose ABOUT.md is CRLF, pinned that way by .gitattributes so it is CRLF on every runner
rather than only on Windows. Core does not pin `* -text` the way the plugin repositories do, so its
own examples are CRLF on Windows and an unstripped carriage return makes the block read as absent.

```console
$ daukle check
daukle: in sync
```
