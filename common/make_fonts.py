#!/usr/bin/env python3
"""Make anti-aliased TFT_eSPI "smooth fonts" (Processing .vlw format) as C arrays.

  python make_fonts.py path/to/NotoSans-Bold.ttf

Writes fonts/*.h next to this script. Needs Pillow. Noto Sans is under the SIL Open Font License.
"""
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

ASCII = "".join(chr(c) for c in range(0x20, 0x7F))
FONTS = [  # (C name, pixel size, characters, tight: line height = the glyphs' own height)
    ("Noto16", 16, ASCII, False),
    ("Noto22", 22, ASCII, False),
    ("Noto30", 30, ASCII, False),
    ("NotoDigits56", 56, " 0123456789:%+-.", True),  # big numbers: no room wasted on accents and tails
]


def vlw(ttf, size, chars, tight):
    font = ImageFont.truetype(ttf, size)
    ascent, descent = font.getmetrics()
    metrics, bitmaps = [], []
    for ch in sorted(set(chars), key=ord):
        adv = round(font.getlength(ch))
        x0, y0, x1, y1 = font.getbbox(ch, anchor="ls")  # relative to the baseline; y0 < 0 is above it
        w, h = max(0, x1 - x0), max(0, y1 - y0)
        if ch == " " or w == 0 or h == 0:
            w = h = 0
            bmp = b""
        else:
            img = Image.new("L", (w, h), 0)
            ImageDraw.Draw(img).text((-x0, -y0), ch, font=font, fill=255, anchor="ls")
            bmp = img.tobytes()
        assert w < 256 and h < 256 and adv < 256
        # unicode, height, width, xAdvance, dY (top above baseline), dX (left offset), unused
        metrics.append(struct.pack(">7i", ord(ch), h, w, adv, -y0 if h else 0, x0 if w else 0, 0))
        bitmaps.append(bmp)
    if tight:  # the text box (and the area cleared behind it) hugs the glyphs
        tops = [struct.unpack(">7i", m) for m in metrics]
        ascent = max(t[4] for t in tops if t[1])
        descent = max(0, max(t[1] - t[4] for t in tops if t[1]))
    header = struct.pack(">6i", len(metrics), 11, size, 0, ascent, descent)
    return header + b"".join(metrics) + b"".join(bitmaps)


def main():
    ttf = sys.argv[1]
    out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fonts")
    os.makedirs(out_dir, exist_ok=True)
    for name, size, chars, tight in FONTS:
        data = vlw(ttf, size, chars, tight)
        with open(os.path.join(out_dir, name + ".h"), "w") as f:
            f.write(f"// Noto Sans Bold {size}px, smooth font for TFT_eSPI (SIL Open Font License). Made by make_fonts.py\n")
            f.write("#pragma once\n#include <pgmspace.h>\n")
            f.write(f"const uint8_t {name}[] PROGMEM = {{\n")
            for i in range(0, len(data), 24):
                f.write("  " + ",".join(f"0x{b:02X}" for b in data[i:i + 24]) + ",\n")
            f.write("};\n")
        print(f"{name}: {len(data)} bytes")


if __name__ == "__main__":
    main()
