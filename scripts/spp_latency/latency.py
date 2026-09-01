#!/usr/bin/env python3
"""Join com_ts.sh output with uart_ts.sh output on the line body and print,
per COM line, how long after the same line hit the UART it reached the PC
(design §13.9 condition 7). Lines that were already in the ring when the
port was opened show seconds; lines streamed live show tens of ms.

    usage: latency.py <uart_ts.log> <com_ts.log>
"""
import re
import sys

uart = {}
for l in open(sys.argv[1], errors="replace"):
    p = l.rstrip("\n").split(" ", 2)
    if len(p) < 3:
        continue
    t, tag, body = p
    uart.setdefault(body, []).append((float(t), tag))

for l in open(sys.argv[2], errors="replace"):
    p = l.rstrip("\n").split(" ", 2)
    if len(p) < 3:
        continue
    t = float(p[0])
    body = re.sub(r"^#\d+ ", "", p[2])
    cands = [(ts, tag) for ts, tag in uart.get(body, []) if ts <= t + 0.05]
    if not cands:
        print("  n/a     -  %s" % body[:70])
        continue
    ts, tag = max(cands)
    print("%+8.3f s %s  %s" % (t - ts, tag, body[:70]))
