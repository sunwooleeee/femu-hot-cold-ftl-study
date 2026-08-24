# Data Notes

## Two Baseline Result Sets

Two distinct baseline result sets were supplied.

1. **Paired baseline**  
   Comes from the same archived experiment set as the threshold/hot-line runs. This is the baseline used by the project slides and is therefore used for all normalized comparisons in this repository.

2. **Extra standalone baseline**  
   Supplied later as six standalone CSV files. It has the same operation counts and transferred bytes, but substantially different runtime and average WRITE latency.

| Workload | Paired runtime (µs) | Extra runtime (µs) | Extra/paired runtime | Paired WRITE latency (µs) | Extra WRITE latency (µs) |
|---|---:|---:|---:|---:|---:|
| P100 | 5,225,833 | 1,425,129 | 0.273× | 1059.59 | 278.01 |
| P500 | 24,188,805 | 8,448,512 | 0.349× | 1080.49 | 358.91 |
| P1000 | 40,558,639 | 12,827,356 | 0.316× | 1067.52 | 321.59 |

The exact cause of this difference cannot be established from the supplied files alone. Possible explanations such as a different FEMU timing configuration, build, machine state, or experiment run are not documented, so this repository does not choose among them.

For that reason, the extra baseline is preserved but excluded from the 3×3 normalized comparison.

See:

- `results/summary/baseline_run_comparison.csv`
