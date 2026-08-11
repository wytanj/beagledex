"""Build, write and preview watch faces.

  python scripts/face.py test                    # synthetic pattern into slot 0
  python scripts/face.py write beagle.jpg        # convert + flash into slot 0
  python scripts/face.py write beagle.jpg --slot 1 --time-y 360
  python scripts/face.py build beagle.jpg -o face0.bin
  python scripts/face.py preview face0.bin       # → face0.png, what the panel will show

WHY THE CONVERSION HAPPENS HERE AND NOT ON THE DEVICE

The device is a UI layer; the host does the heavy lifting. So there is no JPEG
decoder in the firmware, no PSRAM working buffer and no decode CPU — it receives
a raw RGB565 array and blits it. This script is the entire image pipeline, and
the dashboard will call the same code path so its preview is exact rather than
approximate.

WHY IT GOES IN THE storage PARTITION

Two properties fall out of writing to 0x710000 rather than baking pixels into the
firmware:

  · the face survives every app reflash, because we only ever write 0x110000..
  · uploading is an esptool partition write — 322 KB at the measured 695 kbit/s
    is about 4 seconds, hash-verified, over tooling this project already trusts.
    The text command protocol would take ~30 s and need chunking.

LAYOUT, per slot of 0x52000 bytes from 0x710000

  +0x0000  descriptor, 32 bytes used of 4 KB reserved (see FIELDS below)
  +0x1000  pixel data, width*height*2 bytes, RGB565

The descriptor is deliberately roomy: it is the face *as data*, so where the time
sits and what colour it is are uploadable settings rather than a firmware build.
That is the whole reason faces are not micro apps.
"""
import argparse
import struct
import subprocess
import sys
import zlib
from pathlib import Path

from PIL import Image

# Windows hands Python a cp1252 stdout under Git Bash, which cannot encode the
# arrows and dashes below — and a script that dies on its own progress output is
# a poor tool. Force UTF-8 and never fail on a glyph.
sys.stdout.reconfigure(encoding="utf-8", errors="replace")

ROOT = Path(__file__).resolve().parent.parent

PANEL_W, PANEL_H = 368, 448
STORAGE_BASE = 0x710000
SLOT_STRIDE = 0x52000
PIX_OFFSET = 0x1000
MAGIC = 0x31434657          # 'WFC1' read back little-endian
FMT_RGB565_LE = 0

# FIELDS — keep in lockstep with faceLoad() in firmware/watch/watch.ino
#   0x00 u32 magic      0x0C u32 pixelBytes   0x18 u8  timeSize
#   0x04 u16 width      0x10 u32 crc32        0x19 u8  reserved
#   0x06 u16 height     0x14 u16 timeX        0x1A u16 timeColour (RGB565)
#   0x08 u8  format     0x16 u16 timeY        0x1C u16 photoHoldMs
#   0x09 u8  flags                            0x1E u16 reserved
FLAG_SHOW_DATE = 0x01


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def fit_cover(img, w, h):
    """Scale to cover, then centre-crop — the right choice for a photo face.
    'contain' would letterbox, and black bars on a watch look like a bug."""
    if img.width / img.height > w / h:
        nh, nw = h, round(img.width * h / img.height)
    else:
        nw, nh = w, round(img.height * w / img.width)
    img = img.resize((nw, nh), Image.LANCZOS)
    left, top = (nw - w) // 2, (nh - h) // 2
    return img.crop((left, top, left + w, top + h))


def pack_pixels(img):
    # tobytes() rather than getdata(): getdata() is deprecated in Pillow 12 and
    # goes away in 14, and this is quicker anyway.
    raw = img.convert("RGB").tobytes()
    out = bytearray(len(raw) // 3 * 2)
    for i in range(0, len(raw), 3):
        v = rgb565(raw[i], raw[i + 1], raw[i + 2])
        j = i // 3 * 2
        out[j] = v & 0xFF
        out[j + 1] = v >> 8
    return bytes(out)


def build_blob(pixels, w, h, time_x, time_y, time_size, time_colour, show_date, hold_ms):
    flags = FLAG_SHOW_DATE if show_date else 0
    hdr = struct.pack(
        "<IHHBBHIIHHBBHHH",
        MAGIC, w, h, FMT_RGB565_LE, flags, 0,
        len(pixels), zlib.crc32(pixels) & 0xFFFFFFFF,
        time_x, time_y, time_size, 0, time_colour, min(hold_ms, 0xFFFF), 0,
    )
    assert len(hdr) == 32, f"descriptor is {len(hdr)} bytes, firmware expects 32"
    return hdr + b"\x00" * (PIX_OFFSET - len(hdr)) + pixels


def test_pattern(w, h):
    """Deliberately asymmetric: a vertical gradient with a red marker in the TOP
    LEFT and colour bars along the bottom. If the panel shows the marker anywhere
    else, the orientation or byte order is wrong — a symmetric pattern would hide
    exactly the bugs a test pattern exists to catch."""
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = (int(30 + 60 * x / w), int(40 + 150 * y / h), int(90 + 90 * y / h))
    for y in range(40):
        for x in range(40):
            px[x, y] = (255, 0, 0)
    bars = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255)]
    bw = w // 4
    for i, c in enumerate(bars):
        for y in range(h - 40, h):
            for x in range(i * bw, min((i + 1) * bw, w)):
                px[x, y] = c
    return img


def esptool_write(path, addr, port):
    cmd = [sys.executable, "-m", "esptool", "--port", port, "--no-stub",
           "write-flash", hex(addr), str(path)]
    print("  " + " ".join(cmd), flush=True)
    return subprocess.run(cmd).returncode == 0


def main():
    ap = argparse.ArgumentParser(description="watch face builder")
    ap.add_argument("action", choices=["build", "write", "test", "preview"])
    ap.add_argument("image", nargs="?")
    ap.add_argument("-o", "--out")
    ap.add_argument("--slot", type=int, default=0)
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--time-x", type=int, default=40)
    ap.add_argument("--time-y", type=int, default=180)
    ap.add_argument("--time-size", type=int, default=7)
    ap.add_argument("--time-colour", default="ffffff")
    ap.add_argument("--no-date", action="store_true")
    ap.add_argument("--hold-ms", type=int, default=3000,
                    help="how long the photo shows on wake before the dark face")
    args = ap.parse_args()

    if args.action == "preview":
        blob = Path(args.image).read_bytes()
        magic, w, h, fmt = struct.unpack_from("<IHHB", blob, 0)
        if magic != MAGIC:
            sys.exit(f"not a face blob: magic {magic:#x}")
        pix = blob[PIX_OFFSET:PIX_OFFSET + w * h * 2]
        img = Image.new("RGB", (w, h))
        out = img.load()
        for i in range(w * h):
            v = int.from_bytes(pix[i * 2:i * 2 + 2], "little")
            out[i % w, i // w] = (((v >> 11) & 0x1F) << 3, ((v >> 5) & 0x3F) << 2, (v & 0x1F) << 3)
        dst = Path(args.image).with_suffix(".png")
        img.save(dst)
        print(f"{w}x{h} fmt {fmt} → {dst}")
        return

    # Accept an already-built blob as well as an image, so re-flashing the same
    # face does not mean re-converting it — and so passing the wrong one of the
    # two is not an error worth stopping for.
    prebuilt = None
    if args.image and Path(args.image).is_file():
        head = Path(args.image).read_bytes()[:4]
        if len(head) == 4 and struct.unpack("<I", head)[0] == MAGIC:
            prebuilt = Path(args.image).read_bytes()

    if prebuilt is not None:
        blob = prebuilt
        print(f"reusing built blob {args.image} ({len(blob)} bytes)")
    else:
        if args.action == "test":
            img = test_pattern(PANEL_W, PANEL_H)
        elif not args.image:
            sys.exit("need an image path")
        else:
            img = fit_cover(Image.open(args.image), PANEL_W, PANEL_H)

        colour = int(args.time_colour, 16)
        blob = build_blob(
            pack_pixels(img), PANEL_W, PANEL_H,
            args.time_x, args.time_y, args.time_size,
            rgb565((colour >> 16) & 0xFF, (colour >> 8) & 0xFF, colour & 0xFF),
            not args.no_date, args.hold_ms,
        )

    out = Path(args.out) if args.out else ROOT / "firmware" / f"face{args.slot}.bin"
    if prebuilt is None:
        out.write_bytes(blob)
    else:
        out = Path(args.image)
    addr = STORAGE_BASE + args.slot * SLOT_STRIDE
    print(f"slot {args.slot}: {len(blob)} bytes → {out}")
    print(f"  panel {PANEL_W}x{PANEL_H}, time at ({args.time_x},{args.time_y}) size {args.time_size}")
    print(f"  flash target {addr:#x} (storage + {args.slot * SLOT_STRIDE:#x})")

    if args.action == "write":
        if not esptool_write(out, addr, args.port):
            sys.exit("esptool write failed")
        print("written. reboot the device to pick it up.")


if __name__ == "__main__":
    main()
