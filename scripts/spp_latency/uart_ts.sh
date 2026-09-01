#!/bin/bash
# Timestamp (WSL clock, bash EPOCHREALTIME) every line appended to a UART
# capture file. Pair with com_ts.sh, then join with latency.py
# (docs/manual.md §5b).
#   usage: uart_ts.sh <raw-file> <tag>  >> uart_ts.log
f=$1; tag=$2
tail -F -n0 "$f" 2>/dev/null | while IFS= read -r l; do
  l=${l//$'\r'/}
  [ -n "$l" ] && printf '%s %s %s\n' "$EPOCHREALTIME" "$tag" "$l"
done
