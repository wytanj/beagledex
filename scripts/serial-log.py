"""Bridge the board's serial output into the console's store. No network.

  python scripts/serial-log.py                  # COM3, until ctrl-c
  python scripts/serial-log.py COM3 120         # stop after 120 s
  python scripts/serial-log.py COM3 0 hold      # run forever, don't reset on connect

Why this exists: the firmware only posts to the console when secrets.h is
present, and phase 3 networking isn't built yet. Until it is, the only record
of what the device actually did lives in a terminal that scrolls away. This
writes the same log records the HTTP sink writes, straight to .data/db, so
/devices/<id> picks them up with the 4-second tail it already has.

Nothing needs to be online for that: no WiFi on the device, and no web server
on this host either. The store is plain files, so the bridge writes files. Run
`npm run dev` only when you actually want to look at them.

COUPLED TO server/utils/store.ts. Key is log:<deviceId>:<seqKey>, which the fs
driver maps to .data/db/log/<deviceId>/<seqKey>, and seqKey is the zero-padded
millisecond epoch so lexical key order stays chronological. If the schema there
moves, this moves with it.
"""
import json
import random
import re
import string
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

import serial

# The firmware's own banners contain em dashes, and Windows hands Python a cp1252
# stdout under Git Bash. Without this, a logging session dies partway through on
# a character it was only trying to print.
sys.stdout.reconfigure(encoding="utf-8", errors="replace")

ROOT = Path(__file__).resolve().parent.parent
DB = ROOT / ".data" / "db"

port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0        # 0 = no limit
hold = len(sys.argv) > 3 and sys.argv[3].lower() in ("hold", "noreset", "no-reset")

# [info/codec] ES8311 up @ 16000 Hz  →  level=info tag=codec msg=ES8311 up @ …
TAGGED = re.compile(r"^\[(debug|info|warn|error)/([^\]]{1,32})\]\s*(.*)$")
# The boot banner's second line is the only place the firmware states its id.
DEVICE_LINE = re.compile(r"^device\s+([0-9a-f]{12})\s*$", re.I)
# "translator-p01 0.1.5 — phase 00 + 01" / "panel-smoke — CO5300 …"
BANNER = re.compile(r"^([a-z][a-z0-9-]{2,})\s+(\d+\.\d+\.\d+)\b")


def seq_key(at_ms: int) -> str:
    """Mirror of seqKey() in store.ts — zero-padded so keys sort chronologically."""
    rand = "".join(random.choice(string.ascii_lowercase + string.digits) for _ in range(6))
    return f"{at_ms:015d}-{rand}"


def only_known_device():
    """One board on the bench is the normal case, so don't make the user say which."""
    d = DB / "device"
    ids = [p.name for p in d.iterdir() if p.is_file()] if d.is_dir() else []
    return ids[0] if len(ids) == 1 else None


def write_log(device_id: str, level: str, tag: str, msg: str, at_ms: int) -> None:
    at = datetime.fromtimestamp(at_ms / 1000, timezone.utc).isoformat(timespec="milliseconds")
    at = at.replace("+00:00", "Z")
    row = {
        "deviceId": device_id,
        "level": level,
        "tag": tag,
        "msg": msg[:2000],          # same cap the HTTP sink applies
        "at": at,
        "id": seq_key(at_ms),
    }
    out = DB / "log" / device_id
    out.mkdir(parents=True, exist_ok=True)
    (out / row["id"]).write_text(json.dumps(row), encoding="utf-8")


def touch_device(device_id: str, fw_version=None) -> None:
    """Keep lastSeen honest so the fleet view means something between flashes.

    Only ever updates a device the console already knows about — registering one
    is flash.mjs's job, and inventing records here would undermine the point of
    the flash history being written by the thing that moved the bytes.
    """
    p = DB / "device" / device_id
    if not p.is_file():
        return
    try:
        rec = json.loads(p.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return
    rec["lastSeen"] = datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")
    if fw_version:
        rec["fwVersion"] = fw_version
    p.write_text(json.dumps(rec), encoding="utf-8")


device_id = only_known_device()
pending = []          # lines seen before we know who said them
written = 0
last_touch = 0.0      # rate-limit the device rewrite; see the loop

s = serial.Serial(port, 115200, timeout=0.2)

if not hold:
    # RTS drives EN on the USB-Serial/JTAG bridge. Pulse it to restart the app,
    # so a session captures the boot banner rather than joining mid-run.
    s.setDTR(False)
    s.setRTS(True)
    time.sleep(0.15)
    s.setRTS(False)
    time.sleep(0.05)
    s.reset_input_buffer()

print(f"--- {port} @115200 → .data/db/log "
      f"({'no limit' if secs <= 0 else f'{secs:g}s'}, "
      f"{'no reset' if hold else 'reset on connect'}) — ctrl-c to stop ---",
      flush=True)
if device_id:
    print(f"--- device {device_id} (from the store; serial overrides) ---", flush=True)

t0 = time.time()
buf = b""
try:
    while secs <= 0 or time.time() < t0 + secs:
        chunk = s.read(4096)
        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            text = raw.decode("utf-8", "replace").rstrip("\r").strip()
            if not text:
                continue

            at_ms = int(time.time() * 1000)
            print(f"{time.time() - t0:6.1f}s  {text}", flush=True)

            level, tag, msg = "info", "serial", text
            m = TAGGED.match(text)
            if m:
                level, tag, msg = m.group(1), m.group(2), m.group(3)

            fw = None
            if d := DEVICE_LINE.match(text):
                device_id = d.group(1).lower()
                tag = "boot"
            elif b := BANNER.match(text):
                fw, tag = b.group(2), "boot"

            if device_id is None:
                # Buffer rather than guess. The banner names the device on its
                # second line, so this holds one or two lines at most.
                pending.append((level, tag, msg, at_ms))
                continue

            for plevel, ptag, pmsg, pat in pending:
                write_log(device_id, plevel, ptag, pmsg, pat)
                written += 1
            pending.clear()

            write_log(device_id, level, tag, msg, at_ms)
            written += 1

            # A liveness stamp every few seconds is plenty, and the fs driver
            # rewrites the entire device record per call — so don't do it per
            # line. A version change always gets through immediately.
            if fw or time.time() - last_touch > 5.0:
                touch_device(device_id, fw)
                last_touch = time.time()
except KeyboardInterrupt:
    pass
finally:
    s.close()
    where = f".data/db/log/{device_id}" if device_id else "nowhere (device never identified)"
    if pending:
        print(f"--- {len(pending)} line(s) dropped: no device id ever seen. "
              f"Pass one, or flash a build whose banner prints it. ---", flush=True)
    print(f"--- closed: {written} record(s) → {where} ---", flush=True)
