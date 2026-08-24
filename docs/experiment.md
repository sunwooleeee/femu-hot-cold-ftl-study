# Experiment design and results

## Experimental matrix

| Threshold | Hot lines |
|---:|---:|
| 8 | 20 |
| 8 | 24 |
| 8 | 28 |
| 10 | 20 |
| 10 | 24 |
| 10 | 28 |
| 12 | 20 |
| 12 | 24 |
| 12 | 28 |

Each configuration contains P100, P500, and P1000 replay results.

## Paired baseline

The baseline used for normalization is the baseline from the same experiment set as the threshold/hot-line runs.

| Workload | Writes | Flushes |
|---|---:|---:|
| P100 | 4,841 | 4,686 |
| P500 | 21,800 | 20,609 |
| P1000 | 37,335 | 34,304 |

## Internal FTL metrics

Baseline:

- WAF: 1.019527
- GC program pages: 2,628

Minimum observed GC result:

- Configuration: `th8_hot24` / `th8_hot28`
- WAF: 1.015886 (-0.36% vs. baseline)
- GC program pages: 2,138 (-18.65% vs. baseline)

### Hot-line count

All hot-line-20 settings increased GC program pages relative to baseline:

- T8/H20: +19.37%
- T10/H20: +15.14%
- T12/H20: +15.14%

With 24 or 28 hot lines, most tested configurations reduced GC program pages relative to baseline.

## Host-visible metrics

The host-level figures are normalized to the paired baseline for each workload.

The best P500 runtime in the tested sweep was `th12_hot28`:

- Runtime change: -1.82%
- Average WRITE latency change: -1.23%

Across P100, P500, and P1000, the sweep did not show a consistent host-visible speedup.

## Interpretation

The observed results are consistent with the hot/cold placement hypothesis at the FTL level: several configurations reduced GC program pages and WAF relative to baseline. The result depends on the hot-line allocation, as the 20-line cases increased GC work.

The experiment does not establish a consistent end-to-end performance improvement, and it does not isolate the cause of the difference between FTL-internal and host-visible results.

Repeated-trial variance data is not available, so the reported differences are descriptive results from the completed runs rather than statistical significance claims.
