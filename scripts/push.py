"""Send host commands to the running firmware over serial.

  python scripts/push.py time                  # set the RTC from this machine's clock
  python scripts/push.py app Ask               # switch app
  python scripts/push.py tok "Konnichiwa. "    # feed tokens into the current app
  python scripts/push.py clear                 # clear the token buffer
  python scripts/push.py raw ">time 2026-08-11T09:41:00"

This is the configurator, in its CLI form. The verbs are deliberately the ones a
web UI would want, because that is where they are going: the console will drive
exactly these over the wire once phase 3 networking exists, and the firmware side
will not have to change. Until then the token sink is fed from here, which is
what lets the whole UI layer be built and tested with nothing connected.

DOES NOT RESET THE BOARD. DTR and RTS are deasserted before the port opens,
because on this board's native USB-Serial/JTAG bridge RTS drives EN — the very
trick monitor.py uses deliberately. Here it would reboot the app we are trying to
talk to, so it is suppressed.
"""
import sys
import time
from datetime import datetime

import serial

# Same reason as serial-log.py: cp1252 stdout under Git Bash cannot encode the
# arrows used below, and the device's replies contain em dashes.
sys.stdout.reconfigure(encoding="utf-8", errors="replace")

PORT = "COM3"

if len(sys.argv) < 2:
    print(__doc__)
    raise SystemExit(1)

verb = sys.argv[1].lower()
rest = " ".join(sys.argv[2:])

if verb == "time":
    # Local time, not UTC: a watch shows the time on the wearer's wrist. There is
    # no timezone field in the RTC, so whatever goes in is what gets displayed.
    now = datetime.now() if not rest else datetime.fromisoformat(rest)
    cmd = ">time " + now.strftime("%Y-%m-%dT%H:%M:%S")
elif verb == "app":
    cmd = ">app " + rest
elif verb == "tok":
    cmd = ">tok " + rest
elif verb == "clear":
    cmd = ">clear"
elif verb == "raw":
    cmd = rest
else:
    print(f"unknown verb {verb!r}")
    print(__doc__)
    raise SystemExit(1)

s = serial.Serial()
s.port = PORT
s.baudrate = 115200
s.timeout = 0.3
s.dtr = False          # set BEFORE open, or Windows asserts them and resets the board
s.rts = False
s.open()

print(f"→ {cmd}", flush=True)
s.write((cmd + "\n").encode("utf-8"))
s.flush()

# Read back briefly so the firmware's acknowledgement (or complaint) is visible.
deadline = time.time() + 1.5
buf = b""
while time.time() < deadline:
    chunk = s.read(512)
    if chunk:
        buf += chunk
for raw in buf.split(b"\n"):
    text = raw.decode("utf-8", "replace").strip()
    if text:
        print(f"← {text}", flush=True)
s.close()
