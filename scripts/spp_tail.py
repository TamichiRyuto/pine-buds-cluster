#!/usr/bin/env python3
# Windows-side SPP log receiver (docs/design-ibrt-transport.md §13.7).
#
# WSL2 has no Bluetooth, so this must run with WINDOWS Python, not the WSL
# python3 -- e.g. from PowerShell/cmd:
#     C:\Users\Ryuto\pinebuds-logs> python spp_tail.py COM7
# Copy this file to C:\Users\Ryuto\pinebuds-logs\ first (or run it in place
# from \\wsl$\... -- either works, only the *interpreter* must be Windows).
# Requires pyserial: `pip install pyserial` (Windows Python, not WSL).
#
# One-time human setup (design §13.7.1), do this before the first run:
#   0. (new pairing only) take the bud out of the case once to enter
#      pairing mode, or flash a firmware with BTIF_BAM_GENERAL_ACCESSIBLE
#      temporarily forced on (design §13.4.3)
#   1. Settings -> Bluetooth & devices -> Devices -> More Bluetooth settings
#   2. COM Ports tab -> Add -> Outgoing -> pick PineBuds Pro as the device,
#      "PineBudsLog" as the service (the SDP ServiceName, design §13.2.2)
#   3. note the assigned COMn
#   4. if it does not show up: "Remove device" then re-pair, then redo step 2
#      (whether Windows re-scans an existing bond's SDP record for a newly
#      added service is unconfirmed -- design §13.2.1/§13.11 item 1)
#
# Wire format (design §13.6): plain text lines, "\n" only (not "\r\n" --
# that is the UART side's framing, unrelated to this channel).
#     #<seq> <same body as the matching UART/COMPUTE_TRACE line>
# seq is a monotonically increasing counter assigned when the line was
# pushed onto the target's ring buffer; a gap means the ring dropped lines
# (buffer overflow) before they could be sent. A drop is additionally
# reported as its own single-chunk line:
#     #- [log] dropped=<n> contended=<m>
# which this script does not gap-check (it does not start with "#<digits>").
#
# From WSL, tail the output with:
#     tail -f /mnt/c/Users/Ryuto/pinebuds-logs/run-*.log
# No TCP bridge (v1 does not need one -- design §13.7.2: at most a few tens
# of lines per run).

import pathlib
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM7"
out = pathlib.Path(r"C:\Users\Ryuto\pinebuds-logs") / time.strftime(
    "run-%Y%m%d-%H%M%S.log"
)

# The outgoing COM port opens a real RFCOMM connection the instant it is
# opened. timeout is mandatory: without it, readline() can block forever if
# the bud never sends anything (design §13.7.2).
with serial.Serial(port, 115200, timeout=1) as s, out.open("w", buffering=1) as f:
    prev = None
    while True:  # a 1 s timeout read returns b""; keep listening, never exit
        raw = s.readline()
        line = raw.decode("ascii", "replace").rstrip("\r\n")
        if not line:
            continue
        if line.startswith("#") and not line.startswith("#-"):
            n = int(line[1:].split(" ", 1)[0])
            if prev is not None and n != prev + 1:
                f.write(f"!! GAP {prev} -> {n}\n")
            prev = n
        f.write(line + "\n")
        print(line)
