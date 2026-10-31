#!/usr/bin/env python3
"""Generates retro arcade pixel art sprites for Sector 4 Generator and Sector 6 Reactor Core."""
import os
from PIL import Image, ImageDraw

def create_generator():
    w, h = 24, 20
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    draw = ImageDraw.Draw(im)

    # Base: Armored pedestal (y: 15..19)
    # Gunmetal / slate armor with rivets
    draw.polygon([(2, 19), (21, 19), (19, 15), (4, 15)], fill=(50, 60, 75, 255))
    draw.line([(3, 19), (20, 19)], fill=(30, 35, 45, 255)) # Shadow
    draw.line([(4, 16), (19, 16)], fill=(90, 110, 130, 255)) # Highlight
    # Rivet dots
    draw.point([(5, 17), (11, 17), (18, 17)], fill=(160, 180, 200, 255))

    # Pylon body / Coil column (y: 6..14, x: 7..16)
    draw.rectangle([7, 6, 16, 14], fill=(40, 50, 65, 255))

    # Glowing electromagnetic coils (cyan energy rings)
    # Ring 1
    draw.line([(6, 13), (17, 13)], fill=(0, 200, 255, 255))
    draw.line([(8, 13), (15, 13)], fill=(180, 240, 255, 255)) # Glow core
    # Ring 2
    draw.line([(6, 10), (17, 10)], fill=(0, 220, 255, 255))
    draw.line([(8, 10), (15, 10)], fill=(220, 255, 255, 255))
    # Ring 3
    draw.line([(6, 7), (17, 7)], fill=(0, 200, 255, 255))
    draw.line([(8, 7), (15, 7)], fill=(180, 240, 255, 255))

    # Center power conduit (vertical pulse)
    draw.line([(11, 6), (12, 14)], fill=(240, 255, 255, 255))

    # Top emitter cap (y: 1..5)
    draw.polygon([(6, 5), (17, 5), (15, 2), (8, 2)], fill=(70, 85, 105, 255))
    draw.rectangle([10, 1, 13, 2], fill=(0, 255, 240, 255)) # Emitter tip
    draw.point([(11, 0), (12, 0)], fill=(255, 255, 255, 255)) # Spark tip

    return im

def create_reactor_core():
    w, h = 28, 28
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    draw = ImageDraw.Draw(im)

    # 4 corner mounting brackets / containment struts
    draw.polygon([(0, 0), (7, 0), (0, 7)], fill=(70, 65, 80, 255))
    draw.polygon([(27, 0), (20, 0), (27, 7)], fill=(70, 65, 80, 255))
    draw.polygon([(0, 27), (7, 27), (0, 20)], fill=(70, 65, 80, 255))
    draw.polygon([(27, 27), (20, 27), (27, 20)], fill=(70, 65, 80, 255))

    # Outer magnetic containment ring (radius ~12, centered at 13.5, 13.5)
    # Circle diameter 24 (bounds: 2, 2, 25, 25)
    draw.ellipse([2, 2, 25, 25], outline=(180, 40, 60, 255), width=2)
    # Metallic segment nodes
    draw.rectangle([12, 1, 15, 3], fill=(120, 110, 140, 255)) # Top
    draw.rectangle([12, 24, 15, 26], fill=(120, 110, 140, 255)) # Bottom
    draw.rectangle([1, 12, 3, 15], fill=(120, 110, 140, 255)) # Left
    draw.rectangle([24, 12, 26, 15], fill=(120, 110, 140, 255)) # Right

    # Inner high-energy ring (bounds: 5, 5, 22, 22)
    draw.ellipse([5, 5, 22, 22], outline=(255, 80, 20, 255), width=2)

    # Superheated plasma sphere (bounds: 8, 8, 19, 19)
    draw.ellipse([8, 8, 19, 19], fill=(255, 160, 0, 255))

    # Blazing core nucleus (bounds: 10, 10, 17, 17)
    draw.ellipse([10, 10, 17, 17], fill=(255, 230, 80, 255))

    # White-hot singularity center (bounds: 12, 12, 15, 15)
    draw.ellipse([12, 12, 15, 15], fill=(255, 255, 255, 255))

    # Energy flux arcs
    draw.point([(8, 10), (19, 17), (10, 18), (17, 9)], fill=(255, 255, 200, 255))

    return im

def main():
    assets_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "assets")
    os.makedirs(assets_dir, exist_ok=True)

    gen_img = create_generator()
    gen_path = os.path.join(assets_dir, "generator.png")
    gen_img.save(gen_path)
    print(f"Saved {gen_path} ({gen_img.size})")

    core_img = create_reactor_core()
    core_path = os.path.join(assets_dir, "core.png")
    core_img.save(core_path)
    print(f"Saved {core_path} ({core_img.size})")

if __name__ == "__main__":
    main()
