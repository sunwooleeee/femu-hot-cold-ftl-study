# Implementation notes

The complete modified source is checked in under `src/modified/ftl.c` and `src/modified/ftl.h`.

## Hotness state

Each logical page has a cumulative access counter (`lpn_access_cnt`). A write increments the counter, and `is_hot()` classifies the LPN as hot when its count is greater than or equal to `THRESHOLD`.

The classifier is cumulative: it does not decay old accesses or use a sliding window. Once an LPN reaches the threshold, it remains classified as hot for the rest of the run.

## Physical partition

`HOT_LINE_CNT` divides the physical line ID space into a hot range and a cold range. The modified `struct ssd` carries two write pointers (`Hwp` and `Cwp`), and free-line allocation is constrained to the corresponding range.

In the recorded experiment, `HOT_LINE_CNT=20` increased GC program pages for all tested thresholds, while several 24- and 28-line configurations reduced them.

## Host writes

The modified write path:

1. invalidates the old mapping if the LPN was already mapped,
2. increments `lpn_access_cnt[lpn]`,
3. classifies the LPN,
4. performs range-aware foreground GC when needed,
5. allocates a new PPA through the hot or cold write pointer,
6. updates mapping/reverse-mapping state,
7. records host program statistics, and
8. advances the selected write pointer within its range.

## Garbage collection

`do_gc_range()` selects victims from the requested physical line range. During `gc_write_page()`, the valid LPN is classified again and relocated through `Hwp` or `Cwp` accordingly.

## Instrumentation

`struct ftl_stats` tracks host/NAND program counts, GC program pages, erase counts, GC calls, and hot/cold program counts. The implementation can write CSV statistics through `FEMU_FTL_STATS_FILE` (default `/tmp/femu_ftl_stats.csv`).
