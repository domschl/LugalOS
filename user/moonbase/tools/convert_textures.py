#!/usr/bin/env python3
"""
tools/convert_textures.py — Converts seamless terrain textures into 16-bit RGB565 / packed arrays for LugalOS.
"""

import sys
import os
from PIL import Image

def rgb_to_rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3)

def convert_texture_raw(image_path, target_w, target_h):
    im = Image.open(image_path).convert("RGB")
    scaled = im.resize((target_w, target_h), Image.Resampling.LANCZOS)
    px = scaled.load()

    data = [0] * (target_w * target_h)
    for y in range(target_h):
        for x in range(target_w):
            r, g, b = px[x, y]
            data[y * target_w + x] = rgb_to_rgb565(r, g, b)
    return data

def convert_texture_4bit(image_path, target_w, target_h):
    im = Image.open(image_path).convert("RGB")
    scaled = im.resize((target_w, target_h), Image.Resampling.LANCZOS)
    
    # Quantize to 16 colors
    im16 = scaled.quantize(colors=16, method=Image.Quantize.MEDIANCUT)
    pal_raw = im16.getpalette()[:48]
    
    palette_565 = []
    for i in range(16):
        r, g, b = pal_raw[i*3 : (i+1)*3]
        palette_565.append(rgb_to_rgb565(r, g, b))
        
    px = im16.load()
    nibbles = []
    for y in range(target_h):
        for x in range(0, target_w, 2):
            c1 = px[x, y] & 0x0F
            c2 = px[x+1, y] & 0x0F
            nibbles.append((c1 << 4) | c2)
            
    return palette_565, nibbles

def main():
    regolith_src = "assets/tex_regolith_seamless_128.png"
    if not os.path.exists(regolith_src):
        regolith_src = "assets/tex_regolith.jpg"
        
    print(f"Processing 4-bit packed regolith from {regolith_src} (128x128)...")
    pal, nibbles = convert_texture_4bit(regolith_src, 128, 128)
    
    cliff_src = "assets/tex_cliff.jpg"
    print(f"Processing cliff from {cliff_src} (128x64)...")
    cliff_data = convert_texture_raw(cliff_src, 128, 64)
    
    out_h = "include/moon_textures.h"
    out_c = "src/moon_textures.c"
    
    with open(out_h, "w") as fh:
        fh.write("#ifndef MOON_TEXTURES_H\n#define MOON_TEXTURES_H\n\n")
        fh.write("#include <stdint.h>\n#include \"moon_blitter.h\"\n\n")
        fh.write("#define TEX_REGOLITH_W 128\n")
        fh.write("#define TEX_REGOLITH_H 128\n")
        fh.write("extern uint16_t g_tex_regolith[16384];\n\n")
        fh.write("#define TEX_CLIFF_W 128\n")
        fh.write("#define TEX_CLIFF_H 64\n")
        fh.write("extern const uint16_t g_tex_cliff[8192];\n\n")
        fh.write("void moon_textures_init(void);\n\n")
        fh.write("#endif /* MOON_TEXTURES_H */\n")
        
    with open(out_c, "w") as fc:
        fc.write('#include "moon_textures.h"\n')
        fc.write('#include <stdbool.h>\n\n')
        fc.write('#if defined(CONFIG_BOARD_ESP32P4)\n')
        fc.write('#define BULK_BSS __attribute__((section(".bss.bulk")))\n')
        fc.write('#else\n')
        fc.write('#define BULK_BSS\n')
        fc.write('#endif\n\n')
        fc.write('/* 32 KB regolith texture unpacked into PSRAM / BSS */\n')
        fc.write('uint16_t g_tex_regolith[16384] BULK_BSS;\n\n')
        
        # Write 16-color palette
        fc.write('static const uint16_t g_tex_regolith_pal[16] = {\n')
        fc.write('    ' + ', '.join(f'0x{c:04X}' for c in pal) + '\n')
        fc.write('};\n\n')
        
        # Write packed nibbles (8192 bytes in Flash)
        fc.write('/* 8 KB packed 4-bit lunar regolith texture data in Flash */\n')
        fc.write('static const uint8_t g_tex_regolith_4bit[8192] = {\n')
        for i in range(0, len(nibbles), 16):
            chunk = nibbles[i:i+16]
            fc.write('    ' + ', '.join(f'0x{b:02X}' for b in chunk) + ',\n')
        fc.write('};\n\n')
        
        # Write unpacker
        fc.write('void moon_textures_init(void) {\n')
        fc.write('    static bool s_inited = false;\n')
        fc.write('    if (s_inited) return;\n')
        fc.write('    s_inited = true;\n\n')
        fc.write('    for (int i = 0; i < 8192; i++) {\n')
        fc.write('        uint8_t b = g_tex_regolith_4bit[i];\n')
        fc.write('        g_tex_regolith[i * 2]     = g_tex_regolith_pal[(b >> 4) & 0x0F];\n')
        fc.write('        g_tex_regolith[i * 2 + 1] = g_tex_regolith_pal[b & 0x0F];\n')
        fc.write('    }\n')
        fc.write('}\n\n')
        
        # Write cliff texture (16 KB in Flash)
        fc.write('/* Texture: cliff (128x64) */\n')
        fc.write('const uint16_t g_tex_cliff[8192] = {\n')
        for y in range(64):
            row = cliff_data[y * 128 : (y + 1) * 128]
            hex_row = ', '.join(f'0x{val:04X}' for val in row)
            fc.write(f'    {hex_row},\n')
        fc.write('};\n\n')
        
    print(f"Generated {out_h} and {out_c} successfully.")

if __name__ == "__main__":
    main()
