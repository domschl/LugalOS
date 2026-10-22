#!/usr/bin/env python3
"""
tools/convert_textures.py — Converts seamless terrain textures (PNG/JPG) into 16-bit RGB565 C arrays for LugalOS.
"""

import sys
import os
from PIL import Image

def rgb_to_rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3)

def convert_texture(image_path, target_w, target_h):
    im = Image.open(image_path).convert("RGB")
    scaled = im.resize((target_w, target_h), Image.Resampling.LANCZOS)
    px = scaled.load()

    data = [0] * (target_w * target_h)
    for y in range(target_h):
        for x in range(target_w):
            r, g, b = px[x, y]
            data[y * target_w + x] = rgb_to_rgb565(r, g, b)
    return data

def generate_texture_files(textures, out_h_path, out_c_path):
    with open(out_h_path, "w") as fh:
        fh.write("#ifndef MOON_TEXTURES_H\n#define MOON_TEXTURES_H\n\n")
        fh.write("#include <stdint.h>\n#include \"moon_blitter.h\"\n\n")

        for name, w, h, _ in textures:
            fh.write(f"#define TEX_{name.upper()}_W {w}\n")
            fh.write(f"#define TEX_{name.upper()}_H {h}\n")
            fh.write(f"extern const uint16_t g_tex_{name}[{w * h}];\n\n")

        fh.write("#endif /* MOON_TEXTURES_H */\n")

    with open(out_c_path, "w") as fc:
        fc.write("#include \"moon_textures.h\"\n\n")

        for name, w, h, data in textures:
            fc.write(f"/* Texture: {name} ({w}x{h}) */\n")
            fc.write(f"const uint16_t g_tex_{name}[{w * h}] = {{\n")
            for y in range(h):
                row = data[y * w : (y + 1) * w]
                hex_row = ", ".join(f"0x{val:04X}" for val in row)
                fc.write(f"    {hex_row},\n")
            fc.write("};\n\n")

    print(f"Generated {out_h_path} and {out_c_path}")

def main():
    if len(sys.argv) < 2:
        print("Usage: convert_textures.py <image_path:name:w:h> ...")
        sys.exit(1)

    textures = []
    for arg in sys.argv[1:]:
        path, name, w_str, h_str = arg.split(":")
        w, h = int(w_str), int(h_str)
        print(f"Processing texture {name} from {path} -> {w}x{h}...")
        data = convert_texture(path, w, h)
        textures.append((name, w, h, data))

    out_h = "include/moon_textures.h"
    out_c = "src/moon_textures.c"
    generate_texture_files(textures, out_h, out_c)

if __name__ == "__main__":
    main()
