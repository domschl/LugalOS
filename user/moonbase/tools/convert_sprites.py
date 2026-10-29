#!/usr/bin/env python3
"""
tools/convert_sprites.py — Converts sprite images (JPG/PNG) into 16-bit RGB565 C arrays for LugalOS.

- Automatically isolates subject using edge flood-fill (removing black/magenta background).
- Scales to target dimensions (e.g. 64x64, 64x48).
- Saves a clean transparent PNG preview in assets/<name>_clean.png for inspection.
- Emits C array with 0x0000 as transparency key, elevating non-transparent pure black to 0x0821.
"""

import sys
import os
from collections import deque
from PIL import Image

def rgb_to_rgb565(r, g, b):
    val = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3)
    if val == 0:
        val = 0x0821 # Faint dark gray to avoid colliding with 0x0000 transparent key
    return val

def isolate_and_crop(im, threshold=22):
    w, h = im.size
    px = im.load()
    visited = set()
    queue = deque()

    # Seed all border pixels
    for x in range(w):
        queue.append((x, 0))
        queue.append((x, h - 1))
    for y in range(h):
        queue.append((0, y))
        queue.append((w - 1, y))

    bg = set()
    while queue:
        x, y = queue.popleft()
        if (x, y) in visited:
            continue
        visited.add((x, y))

        r, g, b = px[x, y][:3]
        is_magenta = (r > 215 and g < 40 and b > 215)
        is_dark = max(r, g, b) <= threshold

        if is_dark or is_magenta:
            bg.add((x, y))
            for nx, ny in [(x+1, y), (x-1, y), (x, y+1), (x, y-1)]:
                if 0 <= nx < w and 0 <= ny < h and (nx, ny) not in visited:
                    nr, ng, nb = px[nx, ny][:3]
                    if max(nr, ng, nb) <= threshold or (nr > 215 and ng < 40 and nb > 215):
                        queue.append((nx, ny))

    # Bounding box of non-background
    min_x, max_x = w, 0
    min_y, max_y = h, 0
    for y in range(h):
        for x in range(w):
            if (x, y) not in bg:
                if x < min_x: min_x = x
                if x > max_x: max_x = x
                if y < min_y: min_y = y
                if y > max_y: max_y = y

    if min_x > max_x or min_y > max_y:
        print("Warning: no foreground detected!")
        return Image.new("RGBA", (1, 1), (0, 0, 0, 0))

    # Create RGBA cutout
    cutout = Image.new("RGBA", (max_x - min_x + 1, max_y - min_y + 1), (0, 0, 0, 0))
    c_px = cutout.load()

    for y in range(min_y, max_y + 1):
        for x in range(min_x, max_x + 1):
            if (x, y) not in bg:
                r, g, b = px[x, y][:3]
                c_px[x - min_x, y - min_y] = (r, g, b, 255)

    return cutout

def process_sprite(image_path, target_w, target_h, preview_path=None):
    im = Image.open(image_path)
    cutout = isolate_and_crop(im)
    cw, ch = cutout.size

    # Scale maintaining aspect ratio
    scale = min(target_w / cw, target_h / ch)
    new_w = max(1, int(cw * scale))
    new_h = max(1, int(ch * scale))
    scaled = cutout.resize((new_w, new_h), Image.Resampling.LANCZOS)
    s_pixels = scaled.load()

    # Anchor bottom-center
    offset_x = (target_w - new_w) // 2
    offset_y = target_h - new_h

    canvas = Image.new("RGBA", (target_w, target_h), (0, 0, 0, 0))
    canv_px = canvas.load()
    rgb565_data = [0] * (target_w * target_h)

    for y in range(new_h):
        for x in range(new_w):
            r, g, b, a = s_pixels[x, y]
            if a > 70:
                dx = offset_x + x
                dy = offset_y + y
                if 0 <= dx < target_w and 0 <= dy < target_h:
                    canv_px[dx, dy] = (r, g, b, 255)
                    rgb565_data[dy * target_w + dx] = rgb_to_rgb565(r, g, b)

    if preview_path:
        canvas.save(preview_path)
        print(f"Saved preview: {preview_path} ({target_w}x{target_h})")

    return rgb565_data

def generate_c_files(sprites, out_h_path, out_c_path):
    with open(out_h_path, "w") as fh:
        fh.write("#ifndef MOON_SPRITES_H\n#define MOON_SPRITES_H\n\n")
        fh.write("#include <stdint.h>\n#include \"moon_blitter.h\"\n\n")
        fh.write("#define MOON_KEY_COLOR 0x0000\n\n")

        for name, w, h, _ in sprites:
            fh.write(f"#define SPRITE_{name.upper()}_W {w}\n")
            fh.write(f"#define SPRITE_{name.upper()}_H {h}\n")
            fh.write(f"extern const uint16_t g_sprite_{name}[{w * h}];\n")
            fh.write(f"extern const moon_surface_t g_surf_{name};\n\n")

        fh.write("#endif /* MOON_SPRITES_H */\n")

    with open(out_c_path, "w") as fc:
        fc.write("#include \"moon_sprites.h\"\n\n")

        for name, w, h, data in sprites:
            fc.write(f"/* Sprite: {name} ({w}x{h}) */\n")
            fc.write(f"const uint16_t g_sprite_{name}[{w * h}] = {{\n")
            for y in range(h):
                row = data[y * w : (y + 1) * w]
                hex_row = ", ".join(f"0x{val:04X}" for val in row)
                fc.write(f"    {hex_row},\n")
            fc.write("};\n\n")
            fc.write(f"const moon_surface_t g_surf_{name} = {{\n")
            fc.write(f"    .pixels = (uint16_t *)g_sprite_{name},\n")
            fc.write(f"    .width  = {w},\n")
            fc.write(f"    .height = {h},\n")
            fc.write(f"    .stride = {w}\n")
            fc.write("};\n\n")

    print(f"Generated {out_h_path} and {out_c_path}")

def main():
    specs = [
        ("assets/lander.jpg", "lander", 64, 64),
        ("assets/solar_array.jpg", "solar_array", 64, 48),
        ("assets/battery_bank.jpg", "battery_bank", 64, 48),
        ("assets/habitat_dome.jpg", "habitat_dome", 96, 80),
        ("assets/sinterer.jpg", "sinterer", 64, 48),
        ("assets/o2_extractor.jpg", "o2_extractor", 64, 48),
        ("assets/hydrolox_plant.jpg", "hydrolox_plant", 96, 80),
        ("assets/cryo_dewar.jpg", "cryo_dewar", 64, 48)
    ]

    sprites = []
    for path, name, w, h in specs:
        if not os.path.exists(path):
            print(f"Skipping {path} (not found)")
            continue
        preview = f"assets/{name}_clean.png"
        data = process_sprite(path, w, h, preview)
        sprites.append((name, w, h, data))

    if sprites:
        generate_c_files(sprites, "include/moon_sprites.h", "src/moon_sprites.c")

if __name__ == "__main__":
    main()
