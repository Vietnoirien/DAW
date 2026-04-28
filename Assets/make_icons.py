#!/usr/bin/env python3
"""
Generate all LiBeDAW app icon assets from logo_app.png.
Produces:
  - PNG set (16, 32, 48, 64, 128, 256, 512, 1024)
  - icon.ico  (Windows multi-size)
  - icon.icns (macOS)
"""

import struct
import zlib
import os
from pathlib import Path
from PIL import Image

ASSETS = Path(__file__).parent
SRC = ASSETS / "logo_app.png"
ICONS_DIR = ASSETS / "icons"
ICONS_DIR.mkdir(exist_ok=True)

# ── PNG sizes ────────────────────────────────────────────────────────────────
PNG_SIZES = [16, 32, 48, 64, 128, 256, 512, 1024]

src = Image.open(SRC).convert("RGBA")

png_files = {}
for size in PNG_SIZES:
    out = ICONS_DIR / f"icon_{size}x{size}.png"
    resized = src.resize((size, size), Image.LANCZOS)
    resized.save(out, "PNG", optimize=True)
    png_files[size] = out
    print(f"  ✓ {out.name}")

# ── .ico (Windows) ───────────────────────────────────────────────────────────
ico_sizes = [16, 32, 48, 64, 128, 256]
ico_images = [src.resize((s, s), Image.LANCZOS).convert("RGBA") for s in ico_sizes]
ico_path = ICONS_DIR / "icon.ico"
ico_images[0].save(
    ico_path,
    format="ICO",
    sizes=[(s, s) for s in ico_sizes],
    append_images=ico_images[1:],
)
print(f"  ✓ {ico_path.name}")

# ── .icns (macOS) ────────────────────────────────────────────────────────────
# ICNS type codes: https://en.wikipedia.org/wiki/Apple_Icon_Image_format
ICNS_TYPES = {
    16:   b"icp4",
    32:   b"icp5",
    64:   b"icp6",
    128:  b"ic07",
    256:  b"ic08",
    512:  b"ic09",
    1024: b"ic10",
}

import io

def make_icns(png_files: dict, out_path: Path):
    chunks = []
    for size, type_code in ICNS_TYPES.items():
        if size not in png_files:
            continue
        png_data = png_files[size].read_bytes()
        chunk_len = 8 + len(png_data)
        chunks.append(type_code + struct.pack(">I", chunk_len) + png_data)

    body = b"".join(chunks)
    total = 8 + len(body)
    with open(out_path, "wb") as f:
        f.write(b"icns")
        f.write(struct.pack(">I", total))
        f.write(body)
    print(f"  ✓ {out_path.name}")

icns_path = ICONS_DIR / "icon.icns"
make_icns(png_files, icns_path)

# ── Summary ──────────────────────────────────────────────────────────────────
print()
print("All icon assets generated in:", ICONS_DIR)
print(f"  PNGs : {len(PNG_SIZES)} sizes (16–1024px)")
print(f"  ICO  : {ico_path}")
print(f"  ICNS : {icns_path}")
