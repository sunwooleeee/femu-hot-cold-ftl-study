# Reproducibility status

## Included in this repository

- complete modified `ftl.c` / `ftl.h`
- final 3×3 parameter grid: threshold 8/10/12 × hot-line count 20/24/28
- paired host-visible summary metrics
- final FTL GC/WAF counters
- Figure I-Q extracted from the project PPT

## Not included in the project archive

- exact FEMU base commit used for the experiments
- original PostgreSQL trace-to-replay conversion program
- converted replay-input files
- exact original FEMU launch/build command for every run
- repeated-trial/variance data

Because these items are not available, this repository documents the completed experiment and modified FTL implementation but does not claim one-command reproducibility.
