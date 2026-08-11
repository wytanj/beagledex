"""Serial monitor for the translator board.

  python scripts/monitor.py                 # reset, then stream 60s
  python scripts/monitor.py COM3 120        # stream 120s
  python scripts/monitor.py COM3 60 hold    # don't reset on connect

Lines are stamped with seconds since connect so you can line up what you
pressed with what the firmware said.
"""
import sys, time, serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0
hold = len(sys.argv) > 3 and sys.argv[3].lower() in ("hold", "noreset", "no-reset")

s = serial.Serial(port, 115200, timeout=0.2)

if not hold:
    # RTS drives EN on the USB-Serial/JTAG bridge. Pulse it to restart the app.
    s.setDTR(False)
    s.setRTS(True)
    time.sleep(0.15)
    s.setRTS(False)
    time.sleep(0.05)
    s.reset_input_buffer()

print(f"--- {port} @115200 for {secs:g}s "
      f"({'no reset' if hold else 'reset on connect'}) — ctrl-c to stop ---",
      flush=True)

t0 = time.time()
end = t0 + secs
buf = b""
try:
    while time.time() < end:
        chunk = s.read(4096)
        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.decode("utf-8", "replace").rstrip("\r")
            if text:
                print(f"{time.time() - t0:6.1f}s  {text}", flush=True)
except KeyboardInterrupt:
    pass
finally:
    if buf:
        print(f"{time.time() - t0:6.1f}s  {buf.decode('utf-8', 'replace')}", flush=True)
    s.close()
    print("--- closed ---", flush=True)
