# Results

`summary/` contains the compact tables used to document the completed experiment.

- `experiment_summary.csv`: host replay counts and latency/runtime metrics for paired baseline and the 3x3 parameter grid
- `normalized_host_metrics.csv`: runtime and WRITE-latency normalization against the paired baseline
- `gc_waf_summary.csv`: final FTL-internal counters, GC program pages, WAF, and hot/cold program counts
- `baseline_run_comparison.csv`: a separate later baseline run retained for provenance; it is not mixed into the paired normalized comparison

The large per-operation/raw run logs are intentionally not committed to this portfolio repository. The original workload inputs are also not available in the supplied materials; see `../workloads/README.md`.
