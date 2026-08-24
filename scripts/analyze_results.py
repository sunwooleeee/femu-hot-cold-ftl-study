#!/usr/bin/env python3
"""Print the main findings from the checked-in experiment summary CSVs."""
from pathlib import Path
import pandas as pd

ROOT = Path(__file__).resolve().parents[1]
SUMMARY = ROOT / "results" / "summary"

host = pd.read_csv(SUMMARY / "normalized_host_metrics.csv")
gc = pd.read_csv(SUMMARY / "gc_waf_summary.csv")

best_runtime = host.loc[host["runtime_change_pct"].idxmin()]
best_gc = gc.loc[gc["gc_prog_pages"].idxmin()]
best_waf = gc.loc[gc["waf"].idxmin()]
baseline = gc[gc["config"] == "baseline"].iloc[0]

print("Host-visible best runtime configuration")
print(
    f"  {best_runtime['config']} / {best_runtime['workload']}: "
    f"{best_runtime['runtime_change_pct']:.2f}% vs paired baseline"
)
print()
print("Device-internal results")
print(
    f"  GC program pages: {int(baseline['gc_prog_pages']):,} -> "
    f"{int(best_gc['gc_prog_pages']):,} "
    f"({best_gc['gc_program_pages_change_pct']:.2f}%) at {best_gc['config']}"
)
print(
    f"  WAF: {baseline['waf']:.6f} -> {best_waf['waf']:.6f} "
    f"({best_waf['waf_change_pct']:.2f}%) at {best_waf['config']}"
)
