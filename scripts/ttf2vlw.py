# /// script
# requires-python = ">=3.12"
# dependencies = ["Pillow"]
# ///
"""Convert a TTF/OTF font to a TFT_eSPI VLW PROGMEM C header file.

Usage:
    uv run scripts/ttf2vlw.py <font_path> <size> <output.h> [--chars "0123456789"]
"""

import argparse
import struct
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


def render_glyph(font: ImageFont.FreeTypeFont, char: str, ascent: int):
    """Render a single glyph using baseline-anchored rendering for correct metrics."""
    advance = int(font.getlength(char))

    # Render on a large canvas with known baseline position
    canvas_h = ascent * 3
    # Nerd Font icon glyphs can be much wider than their nominal advance. Render
    # on a generous canvas first, then store the corrected advance below.
    canvas_w = max(advance + 40, font.size * 2)
    baseline_y = canvas_h // 2

    img = Image.new("L", (canvas_w, canvas_h), 0)
    draw = ImageDraw.Draw(img)
    # "ls" anchor = left-baseline: the (x, y) is the left edge of the baseline
    draw.text((10, baseline_y), char, font=font, fill=255, anchor="ls")

    # Find actual pixel bounds
    bbox = img.getbbox()
    if bbox is None:
        # Space or empty character
        return {
            "unicode": ord(char),
            "width": 0,
            "height": 0,
            "advance": advance,
            "dY": 0,
            "dX": 0,
            "bitmap": b"",
        }

    x1, y1, x2, y2 = bbox
    width = x2 - x1
    height = y2 - y1
    dx = x1 - 10

    # Some Nerd Font glyphs are visually much wider than their nominal advance.
    # TFT_eSPI uses the VLW advance for layout/clearing, so widen the stored
    # advance to cover the actual right edge of the rendered bitmap.
    advance = max(advance, dx + width)

    # Crop to glyph bounds
    glyph_img = img.crop((x1, y1, x2, y2))

    return {
        "unicode": ord(char),
        "width": width,
        "height": height,
        "advance": advance,
        "dY": baseline_y - y1,  # distance from baseline to top of glyph (positive = above)
        "dX": dx,              # left bearing (relative to render origin at x=10)
        "bitmap": bytes(glyph_img.getdata()),
    }


def build_vlw(font_path: str, size: int, chars: str) -> bytes:
    """Build VLW binary data from a font file."""
    font = ImageFont.truetype(font_path, size)
    ascent, descent = font.getmetrics()

    glyphs = []
    for ch in sorted(set(chars)):
        g = render_glyph(font, ch, ascent)
        if g is not None:
            glyphs.append(g)

    # Build VLW binary
    data = bytearray()

    # Header: 6 x uint32 big-endian
    data += struct.pack(">I", len(glyphs))  # glyph count
    data += struct.pack(">I", 11)           # version (0x0B)
    data += struct.pack(">I", size)         # font size
    data += struct.pack(">I", 0)            # deprecated
    data += struct.pack(">I", ascent)       # ascent
    data += struct.pack(">I", descent)      # descent

    # Glyph metrics: 7 x uint32 per glyph
    for g in glyphs:
        data += struct.pack(">I", g["unicode"])
        data += struct.pack(">I", g["height"])
        data += struct.pack(">I", g["width"])
        data += struct.pack(">I", g["advance"])
        data += struct.pack(">i", g["dY"])   # signed
        data += struct.pack(">i", g["dX"])   # signed
        data += struct.pack(">I", 0)         # padding

    # Bitmap data
    for g in glyphs:
        data += g["bitmap"]

    return bytes(data)


def vlw_to_header(vlw_data: bytes, var_name: str) -> str:
    """Convert VLW binary to a C PROGMEM header."""
    lines = [f"const uint8_t {var_name}[] PROGMEM = {{"]
    for i in range(0, len(vlw_data), 16):
        chunk = vlw_data[i : i + 16]
        hex_vals = ", ".join(f"0x{b:02X}" for b in chunk)
        lines.append(f"  {hex_vals},")
    lines.append("};")
    return "\n".join(lines)


def preview(font_path: str, size: int, chars: str, output_png: str):
    """Generate a visual preview showing glyphs with baseline and metrics."""
    font = ImageFont.truetype(font_path, size)
    ascent, descent = font.getmetrics()

    glyphs = []
    for ch in sorted(set(chars)):
        g = render_glyph(font, ch, ascent)
        if g is not None:
            glyphs.append(g)

    print(f"Font: {font_path}")
    print(f"Size: {size}px  Ascent: {ascent}  Descent: {descent}")
    print(f"{'Char':>5} {'Unicode':>7} {'W':>3}x{'H':<3} {'Adv':>4} {'dY':>4} {'dX':>4}")
    print("-" * 42)

    cell_w = max(g["advance"] for g in glyphs if g["advance"]) + 4
    cell_h = ascent + descent + 10
    cols = min(len(glyphs), 16)
    rows = (len(glyphs) + cols - 1) // cols

    img = Image.new("RGB", (cols * cell_w, rows * cell_h), (30, 30, 40))
    draw = ImageDraw.Draw(img)

    for i, g in enumerate(glyphs):
        ch = chr(g["unicode"])
        print(f"{repr(ch):>5} U+{g['unicode']:04X}  {g['width']:>3}x{g['height']:<3} {g['advance']:>4} {g['dY']:>4} {g['dX']:>4}")

        col = i % cols
        row = i // cols
        cx = col * cell_w
        cy = row * cell_h
        baseline_y = cy + ascent + 4

        # Draw baseline (red), ascent line (blue), descent line (yellow)
        draw.line([(cx, baseline_y), (cx + cell_w - 1, baseline_y)], fill=(80, 40, 40))
        draw.line([(cx, baseline_y - ascent), (cx + cell_w - 1, baseline_y - ascent)], fill=(40, 40, 80))
        draw.line([(cx, baseline_y + descent), (cx + cell_w - 1, baseline_y + descent)], fill=(80, 80, 40))

        # Draw the character
        draw.text((cx + 2, baseline_y), ch, font=font, fill=(230, 230, 240), anchor="ls")

    img.save(output_png)
    print(f"\nPreview saved to {output_png}")
    print(f"Lines: red=baseline, blue=ascent, yellow=descent")


def sprites(font_path: str, size: int, chars: str, output_png: str):
    """Generate a sprite sheet showing the actual cropped alpha bitmaps from the VLW."""
    font = ImageFont.truetype(font_path, size)
    ascent, descent = font.getmetrics()

    glyphs = []
    for ch in sorted(set(chars)):
        g = render_glyph(font, ch, ascent)
        if g is not None and g["width"] > 0:
            glyphs.append(g)

    # Layout: each cell shows the raw alpha bitmap at 1:1 + label
    pad = 2
    label_h = 14
    max_h = max(g["height"] for g in glyphs)
    cell_h = max_h + label_h + pad * 3
    max_w = max(g["width"] for g in glyphs)
    cell_w = max(max_w + pad * 2, 30)

    cols = min(len(glyphs), 16)
    rows = (len(glyphs) + cols - 1) // cols

    img = Image.new("RGB", (cols * cell_w, rows * cell_h), (20, 20, 30))
    draw = ImageDraw.Draw(img)

    for i, g in enumerate(glyphs):
        col = i % cols
        row = i // cols
        cx = col * cell_w + pad
        cy = row * cell_h + pad

        # Label
        ch = chr(g["unicode"])
        label = ch if g["unicode"] < 0x100 else f"U+{g['unicode']:04X}"
        try:
            small_font = ImageFont.truetype("/System/Library/Fonts/Menlo.ttc", 10)
        except Exception:
            small_font = ImageFont.load_default()
        draw.text((cx, cy), label, fill=(120, 200, 120), font=small_font)

        # The actual alpha bitmap
        if g["width"] > 0 and g["height"] > 0:
            bmp = Image.new("L", (g["width"], g["height"]), 0)
            bmp.putdata(g["bitmap"])
            # Convert alpha to visible: white on dark
            rgb_bmp = Image.new("RGB", bmp.size, (20, 20, 30))
            for py in range(bmp.height):
                for px in range(bmp.width):
                    a = bmp.getpixel((px, py))
                    rgb_bmp.putpixel((px, py), (a, a, a))
            img.paste(rgb_bmp, (cx, cy + label_h + pad))

        # Cell border
        draw.rectangle(
            [col * cell_w, row * cell_h, (col + 1) * cell_w - 1, (row + 1) * cell_h - 1],
            outline=(50, 50, 60),
        )

    img.save(output_png)
    print(f"Sprite sheet saved to {output_png} ({cols}x{rows} grid, {len(glyphs)} glyphs)")


def main():
    parser = argparse.ArgumentParser(description="Convert TTF/OTF to TFT_eSPI VLW header")
    parser.add_argument("font_path", help="Path to TTF/OTF font file")
    parser.add_argument("size", type=int, help="Font size in pixels")
    parser.add_argument("output", help="Output .h file path")
    parser.add_argument("--chars", default=" 0123456789.AkWhVm:⚡",
                        help="Characters to include")
    parser.add_argument("--var-name", default=None,
                        help="C variable name (defaults to output filename stem)")
    parser.add_argument("--preview", default=None, metavar="FILE.png",
                        help="Generate a visual preview PNG showing glyphs with metrics")
    parser.add_argument("--sprites", default=None, metavar="FILE.png",
                        help="Generate a sprite sheet of the actual extracted alpha bitmaps")
    args = parser.parse_args()

    var_name = args.var_name or Path(args.output).stem
    print(f"Converting {args.font_path} size {args.size}px")
    print(f"Characters: {args.chars!r} ({len(set(args.chars))} unique)")

    if args.preview:
        preview(args.font_path, args.size, args.chars, args.preview)
    if args.sprites:
        sprites(args.font_path, args.size, args.chars, args.sprites)

    vlw_data = build_vlw(args.font_path, args.size, args.chars)
    header = vlw_to_header(vlw_data, var_name)

    Path(args.output).write_text(header)
    print(f"Written {len(vlw_data)} bytes VLW -> {args.output} ({len(header)} bytes C)")


if __name__ == "__main__":
    main()
