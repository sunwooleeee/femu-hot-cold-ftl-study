# Experiment Design and Results

## Experimental Matrix

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

Each configuration contains P100, P500, and P1000 replay outputs.

## Paired Baseline

The baseline used for normalization is the baseline bundled with the original experiment archive. Its operation counts are:

| Workload | Writes | Flushes |
|---|---:|---:|
| P100 | 4,841 | 4,686 |
| P500 | 21,800 | 20,609 |
| P1000 | 37,335 | 34,304 |

## Internal FTL Metrics

Baseline:

- WAF: 1.019527
- GC program pages: 2,628

Best observed GC result:

- Configuration: `th8_hot24`
- WAF: 1.015886 (-0.36% vs. baseline)
- GC program pages: 2,138 (-18.65% vs. baseline)

### Effect of hot-region capacity

All hot-line-20 settings increase GC program pages relative to baseline:

- T8/H20: 19.37%
- T10/H20: 15.14%
- T12/H20: 15.14%

This is consistent with a hot region that is too constrained for the workload.

With 24 or 28 hot lines, most configurations reduce GC copy cost.

## Host-visible Metrics

The host-level figures are normalized to the paired baseline for each workload.

The best P500 runtime in the supplied sweep is `th12_hot28`:

- Runtime change: -1.82%
- Average WRITE latency change: -1.23%

However, the sweep does not show a robust end-to-end speedup across P100/P500/P1000. The internal GC benefit is therefore stronger and more consistent than the host-visible benefit.

## Interpretation

The data supports the original placement hypothesis at the FTL level: separating hot and cold data can lower valid-page copying if enough hot capacity is reserved.

It does not establish that this optimization is sufficient to improve application runtime. End-to-end behavior can still be dominated by synchronization, queueing, filesystem behavior, or other upper-layer costs.
