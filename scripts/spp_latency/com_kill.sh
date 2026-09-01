#!/bin/bash
# Reclaim the Windows-side python that holds the COM port (a WSL-side kill or
# timeout never reaches it, docs/manual.md §7), then the WSL pipeline.
powershell.exe -NoProfile -Command "Get-CimInstance Win32_Process | ? { \$_.CommandLine -match 'python.*spp_tail' } | % { Stop-Process -Id \$_.ProcessId -Force; 'killed ' + \$_.ProcessId }"
pf="${SPP_COM_PIDFILE:-/tmp/spp_com_ts.pid}"
[ -f "$pf" ] && kill "$(cat "$pf")" 2>/dev/null; rm -f "$pf"; true
