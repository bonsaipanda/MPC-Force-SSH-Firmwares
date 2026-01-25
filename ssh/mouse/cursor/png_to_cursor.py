from PIL import Image
import sys

MAX_W = 64
MAX_H = 64
ALPHA_THRESHOLD = 10  # 0–255, pixels below become transparent

def main(png_path):
    img = Image.open(png_path).convert("RGBA")
    w, h = img.size

    if w > MAX_W or h > MAX_H:
        raise ValueError(f"Image too large ({w}x{h}), max is {MAX_W}x{MAX_H}")

    pixels = img.load()

    print("/* Auto-generated cursor data */")
    print("#include <stdint.h>")
    print()
    print(f"static uint32_t cursor_data[{MAX_W * MAX_H}] = {{")

    for y in range(MAX_H):
        row = []
        for x in range(MAX_W):
            if x < w and y < h:
                r, g, b, a = pixels[x, y]
                if a < ALPHA_THRESHOLD:
                    argb = 0x00000000
                else:
                    argb = (a << 24) | (r << 16) | (g << 8) | b
            else:
                argb = 0x00000000

            row.append(f"0x{argb:08X}")

        print("    " + ", ".join(row) + ",")

    print("};")

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("Usage: png_to_cursor_c.py cursor.png")
        sys.exit(1)

    main(sys.argv[1])
