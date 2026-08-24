# Patch status

The exact FEMU commit used as the base of the experiment was not recorded.

A reliable `hotcold_ftl.patch` requires that exact base revision. Creating a patch against a different FEMU revision would mix unrelated upstream changes with the Hot/Cold FTL changes, so no patch is included here.

The actual modified source used for the experiment is preserved under:

```text
src/modified/ftl.c
src/modified/ftl.h
```
