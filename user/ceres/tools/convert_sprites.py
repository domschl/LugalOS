#!/usr/bin/env python3
"""
user/ceres/tools/convert_sprites.py — Converts sprite images (PNG/JPG) into 4-bit packed Flash .rodata arrays for LugalOS.

- Automatically isolates subject using edge flood-fill (removing pure black/dark background).
- Scales to target dimensions preserving aspect ratio with LANCZOS resampling.
- Quantizes foreground colors to 15 RGB565 colors with index 0 reserved for transparency (0x0000).
- Emits compact 4-bit packed Flash .rodata tables and runtime PSRAM unpacker.
- Reduces Flash footprint from 6.9 KB to <2.0 KB (>70% Flash savings!).
"""

import sys
import os
from collections import deque
from PIL import Image

def rgb_to_rgb565(r, g, b):
    val = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3)
    if val == 0:
        val = 0x0821  # Faint dark gray to avoid colliding with 0x0000 transparent key
    return val

def color_distance(c1, c2):
    # Euclidean distance in RGB
    dr = c1[0] - c2[0]
    dg = c1[1] - c2[1]
    db = c1[2] - c2[2]
    return dr * dr + dg * dg + db * db

def isolate_and_crop(im, threshold=24):
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
            for nx, ny in [(x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)]:
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

def process_sprite(image_path, target_w, target_h, preview_path=None, anchor="bottom"):
    im = Image.open(image_path)
    cutout = isolate_and_crop(im)
    cw, ch = cutout.size

    # Scale maintaining aspect ratio
    scale = min(target_w / cw, target_h / ch)
    new_w = max(1, int(cw * scale))
    new_h = max(1, int(ch * scale))
    scaled = cutout.resize((new_w, new_h), Image.Resampling.LANCZOS)
    s_pixels = scaled.load()

    # Anchor alignment
    offset_x = (target_w - new_w) // 2
    if anchor == "bottom":
        offset_y = target_h - new_h
    elif anchor == "center":
        offset_y = (target_h - new_h) // 2
    else:
        offset_y = 0

    canvas = Image.new("RGBA", (target_w, target_h), (0, 0, 0, 0))
    canv_px = canvas.load()

    for y in range(new_h):
        for x in range(new_w):
            r, g, b, a = s_pixels[x, y]
            if a > 70:
                dx = offset_x + x
                dy = offset_y + y
                if 0 <= dx < target_w and 0 <= dy < target_h:
                    canv_px[dx, dy] = (r, g, b, 255)

    if preview_path:
        canvas.save(preview_path)
        print(f"Saved preview: {preview_path} ({target_w}x{target_h})")

    # Quantize to 15 colors + 1 transparent
    alpha = canvas.getchannel("A")
    fg = Image.new("RGB", (target_w, target_h), (0, 0, 0))
    fg.paste(canvas, mask=alpha)
    q = fg.quantize(colors=15, method=Image.Quantize.MEDIANCUT)
    q_rgb = q.convert("RGB")
    q_px = q_rgb.load()
    a_px = alpha.load()

    # Extract unique RGB colors from foreground
    palette_rgb = []
    for y in range(target_h):
        for x in range(target_w):
            if a_px[x, y] > 70:
                c = q_px[x, y]
                if c not in palette_rgb:
                    palette_rgb.append(c)
                if len(palette_rgb) >= 15:
                    break
        if len(palette_rgb) >= 15:
            break

    # Build 16-entry RGB565 palette: index 0 is 0x0000 (key color)
    palette_565 = [0x0000]
    for c in palette_rgb[:15]:
        palette_565.append(rgb_to_rgb565(c[0], c[1], c[2]))
    while len(palette_565) < 16:
        palette_565.append(0x0000)

    # Encode pixels to 4-bit indices (0 = transparent)
    indices = []
    for y in range(target_h):
        for x in range(target_w):
            if a_px[x, y] <= 70:
                indices.append(0)
            else:
                c = q_px[x, y]
                # Find closest match in palette_rgb
                best_idx = 1
                best_dist = 999999999
                for p_idx, pc in enumerate(palette_rgb[:15]):
                    d = color_distance(c, pc)
                    if d < best_dist:
                        best_dist = d
                        best_idx = p_idx + 1
                indices.append(best_idx)

    # Pack into nibbles (high nibble = even pixel, low nibble = odd pixel)
    packed_bytes = []
    for i in range(0, len(indices), 2):
        n0 = indices[i] & 0x0F
        n1 = indices[i + 1] & 0x0F if (i + 1 < len(indices)) else 0
        packed_bytes.append((n0 << 4) | n1)

    return palette_565, packed_bytes

def generate_c_files(sprites, out_h_path, out_c_path):
    with open(out_h_path, "w") as fh:
        fh.write("/* user/ceres/include/ceres_sprites.h — Pre-rendered Sci-Fi Sprites */\n")
        fh.write("#ifndef CERES_SPRITES_H\n#define CERES_SPRITES_H\n\n")
        fh.write("#include <stdint.h>\n")
        fh.write("#include \"ceres_blitter.h\"\n\n")
        fh.write("#define CERES_KEY_COLOR 0x0000\n\n")

        for name, w, h, _, _ in sprites:
            fh.write(f"#define SPRITE_{name.upper()}_W {w}\n")
            fh.write(f"#define SPRITE_{name.upper()}_H {h}\n")
            fh.write(f"extern const uint16_t g_palette_{name}[16];\n")
            fh.write(f"extern const uint8_t  g_packed_{name}[{(w * h + 1) // 2}];\n\n")

        # Sprite Buffers Struct
        fh.write("/* PSRAM Decompression Buffers (owned by ceres_game_t) */\n")
        fh.write("typedef struct {\n")
        for name, w, h, _, _ in sprites:
            fh.write(f"    uint16_t {name}[{w * h}];\n")
        fh.write("} ceres_sprite_buffers_t;\n\n")

        # Sprite Surfaces Struct
        fh.write("/* Live RGB565 Surfaces (pointing into ceres_sprite_buffers_t) */\n")
        fh.write("typedef struct {\n")
        for name, _, _, _, _ in sprites:
            fh.write(f"    ceres_surface_t {name};\n")
        fh.write("} ceres_surfaces_t;\n\n")

        fh.write("/* Global surfaces accessed during gameplay */\n")
        for name, _, _, _, _ in sprites:
            fh.write(f"extern ceres_surface_t g_surf_{name};\n")
        fh.write("\n")

        fh.write("/* Fast PSRAM Unpacker called once at initialization */\n")
        fh.write("void ceres_sprites_unpack(ceres_sprite_buffers_t *bufs, ceres_surfaces_t *surfs);\n\n")
        fh.write("#endif /* CERES_SPRITES_H */\n")

    with open(out_c_path, "w") as fc:
        fc.write("/* user/ceres/src/ceres_sprites.c — Pre-rendered Sci-Fi Sprites (4-Bit Flash Packed) */\n")
        fc.write("#include \"ceres_sprites.h\"\n\n")

        # Active global surface pointers (single task: gameplay)
        for name, _, _, _, _ in sprites:
            fc.write(f"/* single task: ceres_game */\n")
            fc.write(f"ceres_surface_t g_surf_{name};\n")
        fc.write("\n")

        for name, w, h, pal, packed in sprites:
            fc.write(f"/* Sprite: {name} ({w}x{h}) */\n")
            pal_str = ", ".join(f"0x{v:04X}" for v in pal)
            fc.write(f"const uint16_t g_palette_{name}[16] = {{ {pal_str} }};\n")
            fc.write(f"const uint8_t g_packed_{name}[{len(packed)}] = {{\n")
            for i in range(0, len(packed), 16):
                chunk = packed[i : i + 16]
                hex_chunk = ", ".join(f"0x{v:02X}" for v in chunk)
                fc.write(f"    {hex_chunk},\n")
            fc.write("};\n\n")

        # Unpack function
        fc.write("void ceres_sprites_unpack(ceres_sprite_buffers_t *bufs, ceres_surfaces_t *surfs) {\n")
        fc.write("    if (!bufs) return;\n\n")

        for name, w, h, _, packed in sprites:
            fc.write(f"    /* Unpack {name} ({w}x{h}) into PSRAM buffer */\n")
            fc.write(f"    for (int i = 0; i < {w * h}; i++) {{\n")
            fc.write(f"        uint8_t byte = g_packed_{name}[i >> 1];\n")
            fc.write(f"        uint8_t idx = (i & 1) ? (byte & 0x0F) : (byte >> 4);\n")
            fc.write(f"        bufs->{name}[i] = g_palette_{name}[idx];\n")
            fc.write(f"    }}\n")
            fc.write(f"    g_surf_{name} = ceres_surface_make(bufs->{name}, {w}, {h}, {w});\n")
            fc.write(f"    if (surfs) surfs->{name} = g_surf_{name};\n\n")

        fc.write("}\n")

    print(f"Generated {out_h_path} and {out_c_path}")

def main():
    base_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    assets_dir = os.path.join(base_dir, "assets")

    # (filename, sprite_name, target_w, target_h, anchor)
    specs = [
        ("fuel_silo.png", "fuel_silo", 32, 24, "bottom"),
        ("sam_silo.png", "sam_silo", 24, 16, "bottom"),
        ("sam_rocket.png", "sam_rocket", 12, 24, "center"),
        ("radar_dish.png", "radar_dish", 24, 24, "bottom"),
        ("cryo_bunker.png", "cryo_bunker", 32, 24, "bottom"),
        ("survivor.png", "survivor", 16, 18, "bottom"),
        ("player_ship.png", "player_ship", 28, 14, "center"),
    ]

    sprites = []
    for fname, name, w, h, anchor in specs:
        path_png = os.path.join(assets_dir, fname)
        path_jpg = os.path.join(assets_dir, fname.replace(".png", ".jpg"))
        path = path_png if os.path.exists(path_png) else path_jpg

        if not os.path.exists(path):
            print(f"Skipping {fname} (not found in {assets_dir})")
            continue

        preview = os.path.join(assets_dir, f"{name}_clean.png")
        pal, packed = process_sprite(path, w, h, preview, anchor)
        sprites.append((name, w, h, pal, packed))

    if sprites:
        out_h = os.path.join(base_dir, "include", "ceres_sprites.h")
        out_c = os.path.join(base_dir, "src", "ceres_sprites.c")
        generate_c_files(sprites, out_h, out_c)
    else:
        print("No sprite assets found to process.")

if __name__ == "__main__":
    main()
