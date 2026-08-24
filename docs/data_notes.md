# Data notes

## Two baseline result sets

Two baseline result sets are present in the project records.

1. **Paired baseline**  
   This baseline comes from the same experiment set as the threshold/hot-line runs and is the baseline used in the project figures. It is therefore used for the normalized comparisons in this repository.

2. **Extra standalone baseline**  
   A later baseline run has the same operation counts and transferred bytes but substantially different runtime and average WRITE latency.

| Workload | Paired runtime (µs) | Extra runtime (µs) | Extra/paired runtime | Paired WRITE latency (µs) | Extra WRITE latency (µs) |
|---|---:|---:|---:|---:|---:|
| P100 | 5,225,833 | 1,425,129 | 0.273× | 1059.59 | 278.01 |
| P500 | 24,188,805 | 8,448,512 | 0.349× | 1080.49 | 358.91 |
| P1000 | 40,558,639 | 12,827,356 | 0.316× | 1067.52 | 321.59 |

The project records do not identify the cause of this difference. The extra baseline is therefore kept separate and is not used in the 3×3 normalized comparison.

See `results/summary/baseline_run_comparison.csv`.
