# Reproducibility status

## Preserved

- complete modified `ftl.c` / `ftl.h`
- final 3x3 parameter grid: threshold 8/10/12 x hot-line count 20/24/28
- paired host-visible summary metrics
- final FTL GC/WAF counters
- original Figure I-Q media extracted from the project PPT
- repository-side helper scripts that expose the required build/replay inputs explicitly

## Not preserved in the supplied materials

- exact historical FEMU base commit hash
- original PostgreSQL trace-to-replay conversion program
- original converted replay-input CSV files
- exact original FEMU launch/build command used for every run
- repeated trials/variance data (the supplied experiment appears to contain one completed replay per configuration)

Because of these gaps, this repository should be read as a documented experiment archive and implementation record, not as a one-command reproduction package.

The scripts in `scripts/` intentionally require environment variables for the missing site-specific commands instead of inventing them.
