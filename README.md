# FEMU Hot/Cold FTL Study

A small systems-research project that traces a database-heavy application workload down to storage I/O, replays the workload on FEMU, modifies the BlackBox FTL, and evaluates whether LPN hotness-aware placement can reduce garbage-collection overhead.

> **Base project:** [MoatLab/FEMU](https://github.com/MoatLab/FEMU)  
> **Modified files in this repository:** `ftl.c`, `ftl.h`  
> **Base FEMU commit:** not recorded in the supplied project materials

![Workload replay](figures/workload_replay.svg)

## 1. Motivation

The project started from an end-to-end runtime question in a Demand Responsive Transportation (DRT) simulator. Application-level profiling showed that PostgreSQL DB execute/commit work accounted for a substantial fraction of runtime. System-call tracing then showed repeated `pwrite64` and `fsync`/`fdatasync` activity, motivating a storage-level investigation.

The PostgreSQL write trace was converted into NVMe `WRITE` / `FLUSH` requests and replayed on FEMU. LPN-level analysis showed a skewed access distribution: some logical pages were repeatedly rewritten while many were touched less frequently.

This led to the hypothesis:

**Separating frequently updated (hot) and less frequently updated (cold) pages into different physical line regions can localize invalid pages, reduce valid-page copying during GC, and lower write amplification.**

## 2. FTL Modification

![Hot/Cold policy](figures/hot_cold_policy.svg)

The modified FTL adds four main mechanisms:

1. **Per-LPN access counting**  
   Each write increments `lpn_access_cnt[lpn]`.

2. **Threshold-based hotness classification**  
   An LPN becomes hot when its access count reaches `THRESHOLD`.

3. **Independent write pointers and line regions**  
   Hot and cold data use separate `Hwp` / `Cwp` write pointers and disjoint physical line ranges.

4. **Region-aware GC relocation**  
   During GC, valid pages are reclassified by LPN hotness and relocated through the corresponding hot or cold write pointer.

The implementation also records runtime FTL statistics including host writes, NAND programs, GC programs, WAF, erase count, and hot/cold-specific program counts.

See [`docs/implementation.md`](docs/implementation.md) for the code-level walkthrough.

## 3. Experiment Design

The experiment sweeps:

| Parameter | Values |
|---|---|
| Hotness threshold | 8, 10, 12 |
| Hot line count | 20, 24, 28 |
| Workload size | P100, P500, P1000 |

The threshold candidates were selected from the observed LPN frequency distribution. In the project analysis, LPNs at or above thresholds 8–12 accounted for roughly 91–96% of writes, while the fraction of LPNs classified as hot was roughly 48–57%.

Metrics:

- End-to-end replay runtime
- Average WRITE latency
- GC program pages
- Write amplification factor (WAF)

The host-visible comparison uses the **paired baseline contained in the same experiment archive** as the threshold runs.

## 4. Results

### Host-visible performance

![Runtime](figures/runtime_normalized.png)

![Write latency](figures/write_latency_normalized.png)

The Hot/Cold policy did **not** produce a consistent host-visible speedup across workloads and parameter combinations.

For example, the best P500 runtime among the tested configurations was `th12_hot28`, at **-1.82%** relative to the paired baseline, while the best tested P100 and P1000 runtime configurations were still approximately baseline-level or slightly slower.

This is the central negative result of the study: improving internal flash-management efficiency does not automatically translate into lower end-to-end latency.

### GC efficiency

![GC program pages](figures/gc_program_pages.png)

The paired baseline generated **2,628 GC program pages**.

The minimum observed value was **2,138**, a **18.65% reduction**, at `th8_hot24`. The `th8_hot24` and `th8_hot28` configurations produced the same completed-workload GC/WAF values in the supplied data.

Hot-line count 20 was consistently too small for this workload family: GC program pages increased by approximately 15–19% depending on threshold.

### Write amplification

![WAF](figures/waf.png)

Baseline WAF was **1.019527**. The best observed WAF was **1.015886**, corresponding to approximately **0.36% lower WAF**.

The absolute WAF change is small, but the GC-copy reduction is much larger because host programs dominate total NAND programs in this trace.

## 5. Main Takeaway

The experiment supports two conclusions:

1. **Hot/cold separation can reduce internal GC work when the hot region is sufficiently provisioned.**
2. **Device-level efficiency gains were not sufficient to guarantee host-visible performance gains in this workload.**

That gap motivates a broader systems question: **under what conditions do storage-internal optimizations propagate to application-level performance?**

Possible next steps include host-side synchronization analysis, queueing effects, filesystem behavior, and workload-aware coordination across the application–OS–NVMe–FTL stack.

## 6. Repository Layout

```text
.
├── src/modified/                 # Modified FEMU FTL source
├── results/
│   ├── README.md                  # Data-retention note
│   └── summary/                   # Compact tables used by the figures
├── figures/
├── docs/
│   ├── implementation.md
│   ├── experiment.md
│   ├── data_notes.md
│   └── reproducibility.md
└── scripts/
    └── summarize_results.py
```

## 7. Data Provenance Note

An additional set of standalone baseline CSV files was supplied after the original experiment archives. Those files have the same operation counts but substantially lower runtime and average WRITE latency than the paired baseline used in the 3×3 comparison.

Because the exact reason for that difference is not recorded, the additional baseline is **not mixed into the normalized comparison**. It is quantified separately in `results/summary/baseline_run_comparison.csv`.

See [`docs/data_notes.md`](docs/data_notes.md). Full per-operation raw logs are intentionally omitted from this public portfolio repository; see [`results/README.md`](results/README.md).

## 8. Reproducibility Status

This repository contains:

- Modified `ftl.c` / `ftl.h` and implementation documentation
- Compact experiment summary tables
- FTL GC/WAF summary statistics
- Derived figures

The supplied materials do **not** include:

- The exact FEMU base commit hash
- The PostgreSQL trace-to-replay conversion script
- The replay program / workload input CSVs

Therefore, the repository documents and preserves the completed experiment but is not yet a one-command reproduction package.

## 9. License and Attribution

FEMU is released under the **GNU General Public License v2.0**. The modified FEMU source files in this repository are redistributed under the same license. See [`LICENSE`](LICENSE) and [`NOTICE.md`](NOTICE.md).

Original FEMU project: https://github.com/MoatLab/FEMU
