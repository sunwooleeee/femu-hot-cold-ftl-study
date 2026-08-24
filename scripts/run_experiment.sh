#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

: "${FEMU_FTL_C:?Set FEMU_FTL_C to the target FEMU ftl.c path}"
: "${FEMU_FTL_H:?Set FEMU_FTL_H to the target FEMU ftl.h path}"
: "${BUILD_CMD:?Set BUILD_CMD to the FEMU build command}"
: "${REPLAY_CMD:?Set REPLAY_CMD to the replay command used by replay_workload.sh}"
: "${WORKLOAD_FILE:?Set WORKLOAD_FILE to one replay input CSV}"

RESULT_DIR=${RESULT_DIR:-"$ROOT/results/rerun"}
mkdir -p "$RESULT_DIR"

cp "$ROOT/src/modified/ftl.h" "$FEMU_FTL_H"

for threshold in 8 10 12; do
  for hot_lines in 20 24 28; do
    cfg="th${threshold}_hot${hot_lines}"
    echo "=== $cfg ==="

    # Start from the checked-in modified source, then change only the two
    # compile-time experiment knobs.
    python3 - "$ROOT/src/modified/ftl.c" "$FEMU_FTL_C" "$threshold" "$hot_lines" <<'PY'
from pathlib import Path
import re, sys
src, dst, th, hot = sys.argv[1:]
s = Path(src).read_text()
s = re.sub(r'(?m)^#define THRESHOLD\s+\d+\s*$', f'#define THRESHOLD {th}', s)
s = re.sub(r'(?m)^#define HOT_LINE_CNT\s+\d+\s*$', f'#define HOT_LINE_CNT {hot}', s)
Path(dst).write_text(s)
PY

    bash -lc "$BUILD_CMD"

    cfg_dir="$RESULT_DIR/$cfg"
    mkdir -p "$cfg_dir"
    export FEMU_FTL_STATS_FILE="$cfg_dir/femu_ftl_stats.csv"
    export REPLAY_CMD
    "$ROOT/scripts/replay_workload.sh" "$WORKLOAD_FILE" \
      |& tee "$cfg_dir/replay.log"
  done
done
