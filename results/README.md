# Results

`summary/` contains compact tables extracted from the experiment results.

- `experiment_summary.csv`: host replay counts and latency/runtime metrics for the paired baseline and 3×3 parameter grid
- `normalized_host_metrics.csv`: runtime and WRITE-latency normalization against the paired baseline
- `gc_waf_summary.csv`: FTL-internal counters, GC program pages, WAF, and hot/cold program counts
- `baseline_run_comparison.csv`: paired baseline versus the separate later baseline run

Raw per-operation logs and replay-input files are not included in this repository.
