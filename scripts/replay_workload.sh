#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 <replay-input.csv>" >&2
  exit 2
fi

WORKLOAD_FILE=$(realpath "$1")
: "${REPLAY_CMD:?Set REPLAY_CMD to the command that replays one workload file}"

# Optional: modified ftl.c uses this path when the environment variable is set.
export FEMU_FTL_STATS_FILE="${FEMU_FTL_STATS_FILE:-/tmp/femu_ftl_stats.csv}"

if [[ ! -f "$WORKLOAD_FILE" ]]; then
  echo "Workload file not found: $WORKLOAD_FILE" >&2
  exit 1
fi

# REPLAY_CMD is intentionally supplied by the user because the original replay
# executable/script was not preserved in the experiment archive.
echo "Replaying: $WORKLOAD_FILE"
echo "FTL stats: $FEMU_FTL_STATS_FILE"
bash -lc "$REPLAY_CMD \"$WORKLOAD_FILE\""
