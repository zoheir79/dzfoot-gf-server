#!/usr/bin/env python3
"""
Replace 'WATAR boatlines' sponsor text on existing Ligue 1 DZ kit BMP textures
with 'FAF dzfoot'. Generates {club}_dzfoot_kit_0X.bmp variants.
Preserves original 32-bit BMP format so SDL_LoadBMP_RW can read them.
"""
import os
import glob
from collections import Counter
from PIL import Image, ImageDraw, ImageFont

KIT_DIR = os.path.join(
    os.path.dirname(__file__),
    "GameplayFootball", "data", "databases", "default", "images_teams", "ligue1dz"
)

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


def dominant_color(img, box):
    """Return the most common non-black/non-white color in a region."""
    region = img.crop(box)
    pixels = list(region.getdata())
    # Filter out near-black and near-white pixels (borders / text)
    filtered = [
        (r, g, b, a) for r, g, b, a in pixels
        if not (r < 30 and g < 30 and b < 30) and not (r > 200 and g > 200 and b > 200)
    ]
    if not filtered:
        filtered = pixels
    # Downsample to reduce noise
    sampled = filtered[::max(1, len(filtered)//1000)]
    # Round to nearest 16 to cluster similar colors
    rounded = [(r//16*16, g//16*16, b//16*16, 255) for r, g, b, a in sampled]
    if not rounded:
        return (180, 0, 0, 255)
    return Counter(rounded).most_common(1)[0][0]


def erase_text_area(draw, img, box, fill_color):
    """Fill the given box with the shirt color."""
    draw.rectangle(box, fill=fill_color)


def process_kit(src_path, dst_path, text="FAF dzfoot"):
    img = Image.open(src_path)
    if img.mode != "RGBA":
        img = img.convert("RGBA")

    w, h = img.size
    draw = ImageDraw.Draw(img)

    # Define chest area where sponsor text typically lives (upper center)
    # Heuristic: x 25%-75%, y 8%-30% of texture
    chest_box = (int(w * 0.25), int(h * 0.08), int(w * 0.75), int(h * 0.30))

    # Compute dominant shirt color in that area
    shirt_color = dominant_color(img, chest_box)
    # Ensure full opacity
    shirt_color = (shirt_color[0], shirt_color[1], shirt_color[2], 255)

    # Erase a slightly larger area to cover logo + old text
    erase_box = (int(w * 0.22), int(h * 0.06), int(w * 0.78), int(h * 0.28))
    erase_text_area(draw, img, erase_box, shirt_color)

    # Write new text centered in the erased area
    font_size = max(12, int((erase_box[2] - erase_box[0]) / 10))
    font = get_font(font_size)

    bbox = draw.textbbox((0, 0), text, font=font)
    text_w = bbox[2] - bbox[0]
    text_h = bbox[3] - bbox[1]

    cx = (erase_box[0] + erase_box[2]) // 2
    cy = (erase_box[1] + erase_box[3]) // 2
    x = cx - text_w // 2
    y = cy - text_h // 2

    # Shadow/outline for readability on any kit color
    for dx in (-2, 0, 2):
        for dy in (-2, 0, 2):
            if dx != 0 or dy != 0:
                draw.text((x + dx, y + dy), text, font=font, fill=(0, 0, 0, 180))

    # Main white text
    draw.text((x, y), text, font=font, fill=(255, 255, 255, 230))

    # Save as BMP using original image's info to preserve header compatibility
    img.save(dst_path, format="BMP")
    print(f"  -> {dst_path}")


def main():
    if not os.path.isdir(KIT_DIR):
        print(f"ERROR: Kit directory not found: {KIT_DIR}")
        return

    # Clean up old dzfoot files (wrong naming or previous run)
    for old in glob.glob(os.path.join(KIT_DIR, "*_dzfoot_*.bmp")):
        print(f"Removing old {os.path.basename(old)}")
        os.remove(old)

    # Process each original kit
    for src in sorted(glob.glob(os.path.join(KIT_DIR, "*_kit_01.bmp"))):
        base = os.path.basename(src)
        club = base.split("_kit_")[0]
        dst = os.path.join(KIT_DIR, f"{club}_dzfoot_kit_01.bmp")
        print(f"Processing {base} ...")
        process_kit(src, dst)

    for src in sorted(glob.glob(os.path.join(KIT_DIR, "*_kit_02.bmp"))):
        base = os.path.basename(src)
        club = base.split("_kit_")[0]
        dst = os.path.join(KIT_DIR, f"{club}_dzfoot_kit_02.bmp")
        print(f"Processing {base} ...")
        process_kit(src, dst)

    print("\nDone. All 'FAF dzfoot' kit textures generated.")


if __name__ == "__main__":
    main()
