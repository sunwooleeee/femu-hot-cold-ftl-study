# FEMU Hot/Cold FTL Study

This project traces storage I/O from a PostgreSQL-heavy application workload, replays the resulting NVMe `WRITE` / `FLUSH` requests on FEMU, modifies the BlackBox FTL with hot/cold LPN placement, and evaluates both device-internal and host-visible effects.

> **Upstream project:** [MoatLab/FEMU](https://github.com/MoatLab/FEMU)  
> **Modified source:** [`src/modified/ftl.c`](src/modified/ftl.c), [`src/modified/ftl.h`](src/modified/ftl.h)  
> **Base revision:** the exact FEMU commit used for the experiments was not recorded, so this repository does not include a patch against an assumed base revision.

## 1. From application I/O to FEMU replay

The project began with an end-to-end runtime investigation of a Demand Responsive Transportation simulator. Profiling showed frequent PostgreSQL write/commit activity, and system-call tracing showed repeated `pwrite64` and `fdatasync`/`fsync` operations. The PostgreSQL I/O trace was converted into NVMe `WRITE` / `FLUSH` requests and replayed on FEMU so that the FTL path could be modified and measured.

![Figure I: PostgreSQL I/O workload replay path](figures/figure_I_workload_replay.png)

The replayed workloads contain:

| Workload | WRITE | FLUSH | Dominant WRITE size |
|---|---:|---:|---|
| P100 | 4,841 | 4,686 | ~95% 8 KB |
| P500 | 21,800 | 20,609 | ~95% 8 KB |
| P1000 | 37,335 | 34,304 | ~95% 8 KB |

Additional workload counts are summarized in [`workloads/README.md`](workloads/README.md).

## 2. Hot/Cold placement hypothesis

Baseline FEMU places frequently updated and less frequently updated pages through the same write pointer and physical line pool. The experiment tested whether separating these pages could reduce valid-page copying during garbage collection.

![Figure J: baseline mixed placement](figures/figure_J_baseline_mixed_placement.png)

The modified policy classifies an LPN as hot when its cumulative write count reaches a threshold and allocates hot and cold pages through separate write pointers and line ranges.

![Figure K: modified hot/cold separation](figures/figure_K_hot_cold_separation.png)

The hypothesis was:

**If hot and cold pages are separated physically, GC copy work and write amplification can decrease by localizing invalid pages.**

## 3. FTL implementation

The complete modified source is included under [`src/modified/`](src/modified/). The main changes are:

- per-LPN access counter: `lpn_access_cnt[lpn]`
- threshold classifier: `is_hot()`
- hot/cold physical line ranges controlled by `HOT_LINE_CNT`
- independent write pointers: `Hwp` and `Cwp`
- range-aware free-line allocation and GC victim selection
- GC relocation through the hot or cold write pointer according to the current LPN classification
- FTL statistics for host/NAND programs, GC programs, WAF, erase count, and hot/cold program counts

![Figure L: baseline and modified FTL policy](figures/figure_L_ftl_policy_comparison.png)

Code-level notes are in [`docs/implementation.md`](docs/implementation.md).

## 4. Parameter selection and experiment matrix

The replay traces show a skewed LPN update-frequency distribution. Thresholds 8, 10, and 12 were used, with hot-line counts 20, 24, and 28.

![Figure M: LPN write-frequency distribution and threshold candidates](figures/figure_M_threshold_distribution.png)

![Figure N: hot-LPN share under each threshold](figures/figure_N_hot_lpn_share.png)

| Parameter | Values |
|---|---|
| Hotness threshold | 8, 10, 12 |
| Hot line count | 20, 24, 28 |
| Workload | P100, P500, P1000 |

Host-visible metrics were replay runtime and average WRITE latency. Device-internal metrics were GC program pages and WAF.

## 5. Host-visible result

![Figure O: baseline-normalized total runtime](figures/figure_O_host_runtime.png)

![Figure P: baseline-normalized WRITE latency](figures/figure_P_write_latency.png)

The Hot/Cold policy did **not** produce a consistent host-visible improvement across the tested workloads and parameter combinations. The best P500 runtime in the tested grid was `TH=12, HOT_LINE_CNT=28`, approximately **1.82% below** the paired baseline. The best tested P100 and P1000 runtime configurations were approximately baseline-level or slower.

## 6. Device-internal result

![Figure Q: WAF and GC program pages](figures/figure_Q_waf_gc_heatmap.png)

The paired baseline generated **2,628 GC program pages**. The minimum was **2,138** at `TH=8, HOT_LINE_CNT=24/28`, a **18.65% reduction**. Baseline WAF was **1.019527** and the minimum observed WAF was **1.015886**, approximately **0.36% lower**.

At `HOT_LINE_CNT=20`, GC program pages increased for all three tested thresholds. With 24 or 28 hot lines, most tested configurations reduced GC program pages relative to the paired baseline.

## 7. Conclusion

In this experiment, hot/cold placement reduced FTL-internal GC copy work in several configurations, but the reduction did not consistently translate into lower host-visible runtime or WRITE latency.

This led to the follow-up question:

> **Under what conditions does a device-level optimization propagate to host-visible performance?**

The current experiment does not isolate the cause of the gap between FTL-internal and host-visible results.

## 8. Repository layout

```text
.
├── src/modified/
│   ├── ftl.c
│   └── ftl.h
├── patches/
│   └── README.md
├── workloads/
│   └── README.md
├── figures/                      # Figures extracted from the project PPT
├── results/summary/
├── docs/
├── LICENSE
└── NOTICE.md
```

## 9. Reproducibility status

This repository includes the modified FTL source, the figures used in the project presentation, and summary tables from the completed experiment. The exact FEMU base commit, original trace-conversion/replay program, converted replay-input files, and repeated-trial/variance data are not included in the project archive.

For that reason, the reported results should be read as results from the completed experiment rather than as a one-command reproduction package. See [`docs/reproducibility.md`](docs/reproducibility.md) and [`patches/README.md`](patches/README.md).

## 10. License and attribution

FEMU is released under the GNU General Public License v2.0. The modified FEMU source files in this repository are redistributed under the same license. See [`LICENSE`](LICENSE) and [`NOTICE.md`](NOTICE.md).
