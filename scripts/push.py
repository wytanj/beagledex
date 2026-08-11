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
import socket
import sys
import time
from datetime import datetime
from pathlib import Path

import serial

ROOT = Path(__file__).resolve().parent.parent


def load_env():
    """Read .env so `push wifi` needs no arguments and no password on the command
    line — shell history is a bad place for one."""
    out = {}
    p = ROOT / ".env"
    if p.is_file():
        for line in p.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                out[k.strip()] = v.strip()
    return out


def lan_ip():
    """The address the device must use — 'localhost' means the watch itself.
    A UDP connect() sends no packets, so this works out the route locally."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    finally:
        s.close()

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
elif verb == "wifi":
    env = load_env()
    if rest:
        cmd = ">wifi " + rest
    else:
        ssid = env.get("HOME_WIFI_SSID")
        pw = env.get("HOME_WIFI_PASSWORD")
        if not ssid or not pw:
            raise SystemExit("set HOME_WIFI_SSID and HOME_WIFI_PASSWORD in .env, or pass them")
        cmd = f">wifi {ssid} {pw}"
        print(f"(ssid {ssid}, password {len(pw)} chars from .env)")
elif verb == "console":
    url = rest or f"http://{lan_ip()}:3000"
    cmd = ">console " + url
elif verb == "token":
    tok = rest or load_env().get("DEVICE_TOKEN", "")
    if not tok:
        raise SystemExit("set DEVICE_TOKEN in .env, or pass it")
    cmd = ">token " + tok
elif verb == "net":
    cmd = ">net"
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

# Never echo a credential. These commands are typed at a terminal whose history is
# kept, and the reply below gets copied into logs.
if verb == "wifi":
    print("→ >wifi <ssid> <password>", flush=True)
elif verb == "token":
    print("→ >token <token>", flush=True)
else:
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
