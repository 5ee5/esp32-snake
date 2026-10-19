#!/bin/bash
# Print the board's serial log for a number of seconds, without an
# interactive terminal (unlike `pio device monitor`).
#
# usage: tools/log.sh [seconds] [port]
#
# Lines buffered on the board before connecting arrive in one burst
# right away. They are dropped, the same as monitor/filter_fresh.py.
set -euo pipefail

exec python3 - "${1:-30}" "${2:-/dev/ttyACM0}" <<'EOF'
import sys, time
import serial

seconds, port = float(sys.argv[1]), sys.argv[2]
s = serial.Serial(port, 115200, timeout=0.2)

start = time.monotonic()
while time.monotonic() - start < 0.5:
    s.read(4096)

end = start + seconds
while time.monotonic() < end:
    line = s.readline().decode(errors="replace").rstrip()
    if line:
        print(line, flush=True)
EOF
