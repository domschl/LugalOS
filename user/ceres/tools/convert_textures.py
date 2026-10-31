#!/usr/bin/env python3
"""
user/ceres/tools/convert_textures.py — Converts seamless terrain textures into compact 4-bit packed Flash arrays.
"""

import sys
import os
from PIL import Image

def rgb_to_rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3)

def convert_texture_4bit(image_path, target_w, target_h):
    im = Image.open(image_path).convert("RGB")
    scaled = im.resize((target_w, target_h), Image.Resampling.LANCZOS)

    # Quantize to 16 colors for compact 4-bit palette
    im16 = scaled.quantize(colors=16, method=Image.Quantize.MEDIANCUT)
    pal_raw = im16.getpalette()[:48]

    palette_565 = []
    for i in range(16):
        r, g, b = pal_raw[i * 3:(i + 1) * 3]
        palette_565.append(rgb_to_rgb565(r, g, b))

    px = im16.load()
    nibbles = []
    for y in range(target_h):
        for x in range(0, target_w, 2):
            c1 = px[x, y] & 0x0F
            c2 = px[x + 1, y] & 0x0F
            nibbles.append((c1 << 4) | c2)

    return palette_565, nibbles

def main():
    base_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    assets_dir = os.path.join(base_dir, "assets")

    rock_src = os.path.join(assets_dir, "tex_cavern_rock.jpg")
    if not os.path.exists(rock_src):
        rock_src = os.path.join(assets_dir, "tex_cavern_rock.png")

    if not os.path.exists(rock_src):
        print(f"Texture source not found in {assets_dir}")
        return

    target_w, target_h = 32, 32
    print(f"Processing 4-bit packed rock texture from {rock_src} ({target_w}x{target_h})...")
    pal, nibbles = convert_texture_4bit(rock_src, target_w, target_h)

    out_h = os.path.join(base_dir, "include", "ceres_textures.h")
    out_c = os.path.join(base_dir, "src", "ceres_textures.c")

    with open(out_h, "w") as fh:
        fh.write("/* user/ceres/include/ceres_textures.h — Seamless Geological Textures */\n")
        fh.write("#ifndef CERES_TEXTURES_H\n#define CERES_TEXTURES_H\n\n")
        fh.write("#include <stdint.h>\n\n")
        fh.write(f"#define TEX_ROCK_W {target_w}\n")
        fh.write(f"#define TEX_ROCK_H {target_h}\n")
        fh.write(f"#define TEX_ROCK_PIXELS ({target_w} * {target_h})\n\n")
        fh.write("/* Zero-static texture unpacker: decompresses 4-bit Flash into caller-owned buffer */\n")
        fh.write("void ceres_textures_unpack(uint16_t *dst);\n\n")
        fh.write("#endif /* CERES_TEXTURES_H */\n")

    with open(out_c, "w") as fc:
        fc.write("/* user/ceres/src/ceres_textures.c — Seamless Geological Textures */\n")
        fc.write("#include \"ceres_textures.h\"\n\n")

        # 16-color palette (32 bytes in Flash .rodata)
        fc.write("static const uint16_t g_tex_rock_pal[16] = {\n")
        fc.write("    " + ", ".join(f"0x{c:04X}" for c in pal) + "\n")
        fc.write("};\n\n")

        # Packed 4-bit nibbles (1024 bytes in Flash .rodata)
        fc.write(f"/* Compact 4-bit packed rock strata texture in Flash ({len(nibbles)} bytes) */\n")
        fc.write(f"static const uint8_t g_tex_rock_4bit[{len(nibbles)}] = {{\n")
        for i in range(0, len(nibbles), 16):
            chunk = nibbles[i:i + 16]
            fc.write("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",\n")
        fc.write("};\n\n")

        # Unpacker routine (strictly zero static variables)
        fc.write("void ceres_textures_unpack(uint16_t *dst) {\n")
        fc.write("    if (!dst) return;\n")
        fc.write(f"    for (int i = 0; i < {len(nibbles)}; i++) {{\n")
        fc.write("        uint8_t b = g_tex_rock_4bit[i];\n")
        fc.write("        dst[i * 2]     = g_tex_rock_pal[(b >> 4) & 0x0F];\n")
        fc.write("        dst[i * 2 + 1] = g_tex_rock_pal[b & 0x0F];\n")
        fc.write("    }\n")
        fc.write("}\n")

    print(f"Generated {out_h} and {out_c}")

if __name__ == "__main__":
    main()
