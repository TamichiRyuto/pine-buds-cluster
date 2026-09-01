#!/bin/bash
# Open the SPP COM port with the Windows-side receiver (scripts/spp_tail.py,
# copied to C:\Users\Ryuto\pinebuds-logs) in unbuffered mode and timestamp
# each line on arrival with the WSL clock -- the same clock uart_ts.sh uses,
# so the two logs are directly comparable. Windows python's stdout streams
# through the interop pipe line by line (measured: no extra buffering).
#   usage: com_ts.sh [COMn]  >> com_ts.log      (stop with: com_kill.sh)
port=${1:-COM6}
echo $$ > "${SPP_COM_PIDFILE:-/tmp/spp_com_ts.pid}"
powershell.exe -NoProfile -Command "Set-Location C:\Users\Ryuto\pinebuds-logs; python -u spp_tail.py $port" 2>&1 \
  | while IFS= read -r l; do l=${l//$'\r'/}; [ -n "$l" ] && printf '%s %s %s\n' "$EPOCHREALTIME" "$port" "$l"; done
