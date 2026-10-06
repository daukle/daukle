# secondpass

A fixture whose committed plugin succeeds and whose staged working tree fails, which is the only
shape that can tell "the second pass ran" apart from "the second pass did nothing". The harness
must report a failure naming the working tree.

```console
$ daukle secondpass:greet
```
