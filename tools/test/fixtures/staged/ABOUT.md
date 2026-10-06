# staged

A fixture naming a coordinate that cannot resolve. Its harness root holds no plugin.lua, so there
is no working tree to stage and the second pass never runs. The first pass must therefore fail,
which is what proves the first pass really does resolve what the example commits.

```console
$ daukle tasks
banner
```
