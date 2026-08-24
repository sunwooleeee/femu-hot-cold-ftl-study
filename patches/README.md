# Patch provenance

A reliable `hotcold_ftl.patch` requires the exact FEMU source revision that the local experiment tree was based on.

That historical commit hash was not recorded in the supplied project materials, so this repository intentionally does **not** manufacture an "original vs. modified" patch against an unverified base.

The nearest upstream snapshot found immediately before the July 2026 experiment period is:

```text
MoatLab/FEMU
66067fabcbe6c2ae99fc83929ca9d2f33c8bbb50
```

Its `ftl.c` / `ftl.h` structure closely matches the supplied modified files, so it is a useful **reference candidate**, but it is not labeled as the exact historical base without additional evidence.

Once the actual base revision is recovered, generate the patch with:

```bash
git diff <BASE_COMMIT> -- hw/femu/bbssd/ftl.c hw/femu/bbssd/ftl.h \
  > patches/hotcold_ftl.patch
```

Until then, the authoritative artifact for the implemented policy is the complete modified source under `src/modified/`.
