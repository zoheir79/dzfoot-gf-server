#!/usr/bin/env python3
"""
Overlay 'FAF dzfoot' text onto existing Ligue 1 DZ kit BMP textures.
Generates *_dzfoot.bmp variants in the same directory.
"""
import os
import sys
from PIL import Image, ImageDraw, ImageFont

KIT_DIR = os.path.join(
    os.path.dirname(__file__),
    "GameplayFootball", "data", "databases", "default", "images_teams", "ligue1dz"
)

# Fallback: try a few TTF paths common on Ubuntu / Windows
FONT_PATHS = [
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
    "C:/Windows/Fonts/arialbd.ttf",
    "C:/Windows/Fonts/DejaVuSans-Bold.ttf",
]


def get_font(size):
    for path in FONT_PATHS:
        if os.path.exists(path):
            try:
                return ImageFont.truetype(path, size)
            except Exception:
                pass
    return ImageFont.load_default()


def overlay_text(src_path, dst_path, text="FAF dzfoot"):
    img = Image.open(src_path)
    # Convert to RGBA to allow alpha compositing
    if img.mode != "RGBA":
        img = img.convert("RGBA")

    draw = ImageDraw.Draw(img)

    # Adaptive font size based on image width (approx 5% of width)
    font_size = max(12, img.width // 18)
    font = get_font(font_size)

    # Measure text
    bbox = draw.textbbox((0, 0), text, font=font)
    text_w = bbox[2] - bbox[0]
    text_h = bbox[3] - bbox[1]

    # Place near bottom center (common shirt front lower area)
    x = (img.width - text_w) // 2
    y = img.height - text_h - (img.height // 12)

    # Draw black outline/shadow for readability on any kit color
    for dx in (-2, 0, 2):
        for dy in (-2, 0, 2):
            if dx != 0 or dy != 0:
                draw.text((x + dx, y + dy), text, font=font, fill=(0, 0, 0, 180))

    # Main white text
    draw.text((x, y), text, font=font, fill=(255, 255, 255, 220))

    # Save back as BMP (GF uses 32-bit BMP; PIL will write compatible BMP)
    img.save(dst_path, format="BMP")
    print(f"  -> {dst_path}")


def main():
    if not os.path.isdir(KIT_DIR):
        print(f"ERROR: Kit directory not found: {KIT_DIR}")
        sys.exit(1)

    files = sorted(f for f in os.listdir(KIT_DIR) if f.endswith("_kit_01.bmp") or f.endswith("_kit_02.bmp"))
    print(f"Found {len(files)} kit textures in {KIT_DIR}")

    for fname in files:
        src = os.path.join(KIT_DIR, fname)
        # e.g. crbelouizdad_kit_01.bmp -> crbelouizdad_kit_01_dzfoot.bmp
        base, ext = os.path.splitext(fname)
        dst_name = f"{base}_dzfoot{ext}"
        dst = os.path.join(KIT_DIR, dst_name)
        print(f"Processing {fname} ...")
        overlay_text(src, dst)

    print("\nDone. 'FAF dzfoot' variants created alongside originals.")
    print("To use them, update kit_url in teamdata.cpp to point to *_dzfoot.bmp")


if __name__ == "__main__":
    main()
