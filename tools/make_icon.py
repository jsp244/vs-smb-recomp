"""Draws the port's icon (original artwork: an arcade token marked VS on a red
badge) and writes it as a Windows .ico (16/32/48/256), a PNG preview and a C
array for SDL_SetWindowIcon. Pure Python, no image libraries.

usage: python tools/make_icon.py
"""
import math
import os
import struct
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_ICO = os.path.join(ROOT, "frontend", "vssmb.ico")
OUT_PNG = os.path.join(ROOT, "frontend", "icon_preview.png")
OUT_C = os.path.join(ROOT, "frontend", "icon_data.c")


def lerp(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def dist_seg(px, py, ax, ay, bx, by):
    """distance from P to segment AB"""
    vx, vy = bx - ax, by - ay
    wx, wy = px - ax, py - ay
    l2 = vx * vx + vy * vy
    t = 0.0 if l2 == 0 else max(0.0, min(1.0, (wx * vx + wy * vy) / l2))
    dx, dy = px - (ax + t * vx), py - (ay + t * vy)
    return math.hypot(dx, dy)


# the letters V and S as strokes (segments in normalised coordinates), blocky like arcade lettering
LETTERS = [
    ((0.255, 0.355), (0.345, 0.645)), ((0.345, 0.645), (0.435, 0.355)),          # V
    ((0.700, 0.360), (0.540, 0.360)), ((0.540, 0.360), (0.540, 0.500)),          # S: top, upper left,
    ((0.540, 0.500), (0.700, 0.500)), ((0.700, 0.500), (0.700, 0.640)),          # middle, lower right,
    ((0.700, 0.640), (0.540, 0.640)),                                            # bottom
]


def color_at(u, v):
    """RGBA (0..1) of the artwork at normalised coordinates: an arcade token marked VS on a red badge"""
    # rounded-square badge, dark red towards the bottom
    r = 0.22
    cx, cy = min(max(u, r), 1 - r), min(max(v, r), 1 - r)
    if math.hypot(u - cx, v - cy) > r:
        return (0, 0, 0, 0)
    col = lerp((0.74, 0.12, 0.12), (0.36, 0.04, 0.07), v)
    # diagonal stripes in the background, faint
    if int((u + v) * 14) % 2 == 0:
        col = lerp(col, (0.0, 0.0, 0.0), 0.10)
    d = math.hypot(u - 0.5, v - 0.5)
    if d < 0.40:
        # the token: a shadow ring, the rim, and the face with a light from the top left
        if d > 0.375:
            return (*lerp(col, (0.0, 0.0, 0.0), 0.55), 1.0)
        light = max(0.0, min(1.0, 0.5 - ((u - 0.5) * 0.7 + (v - 0.5) * 0.9)))
        if d > 0.315:
            col = lerp((0.62, 0.38, 0.05), (1.0, 0.80, 0.30), light)
            # milled edge
            if int(math.atan2(v - 0.5, u - 0.5) / math.pi * 36) % 2 == 0:
                col = lerp(col, (0.35, 0.20, 0.02), 0.25)
        else:
            col = lerp((0.86, 0.58, 0.10), (1.0, 0.90, 0.45), light)
            if d > 0.295:
                col = lerp(col, (0.45, 0.26, 0.02), 0.5)      # engraved line inside the rim
        ink = (0.40, 0.17, 0.02)
        for (ax, ay), (bx, by) in LETTERS:
            dd = dist_seg(u, v, ax, ay, bx, by)
            if dd < 0.042:
                return (*ink, 1.0)
            if dd < 0.050 and (u - ax) + (v - ay) > 0:         # a little relief below and right of the strokes
                col = lerp(col, (1.0, 0.95, 0.70), 0.6)
        return (*col, 1.0)
    return (*col, 1.0)


def render(size, ss=4):
    px = bytearray()
    for y in range(size):
        for x in range(size):
            acc = [0.0, 0.0, 0.0, 0.0]
            for sy in range(ss):
                for sx in range(ss):
                    u = (x + (sx + 0.5) / ss) / size
                    v = (y + (sy + 0.5) / ss) / size
                    c = color_at(u, v)
                    acc[0] += c[0] * c[3]; acc[1] += c[1] * c[3]; acc[2] += c[2] * c[3]; acc[3] += c[3]
            n = ss * ss
            a = acc[3] / n
            if a > 0:
                rgb = [acc[i] / acc[3] for i in range(3)]
            else:
                rgb = [0, 0, 0]
            px += bytes(int(max(0, min(1, c)) * 255 + 0.5) for c in rgb) + bytes([int(a * 255 + 0.5)])
    return bytes(px)


def png(size, rgba):
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + rgba[y * size * 4:(y + 1) * size * 4] for y in range(size))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def bmp_entry(size, rgba):
    """32-bit DIB with an empty AND mask, rows bottom-up, BGRA"""
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, size * size * 4, 0, 0, 0, 0)
    rows = []
    for y in range(size - 1, -1, -1):
        row = rgba[y * size * 4:(y + 1) * size * 4]
        rows.append(b"".join(bytes((row[i + 2], row[i + 1], row[i], row[i + 3])) for i in range(0, len(row), 4)))
    mask_row = b"\x00" * (((size + 31) // 32) * 4)
    return header + b"".join(rows) + mask_row * size


def main():
    sizes = (16, 32, 48, 256)
    images = {}
    for s in sizes:
        images[s] = render(s, ss=6 if s <= 48 else 2)
        print(f"rendered {s}x{s}")
    entries = []
    for s in sizes:
        data = png(s, images[s]) if s == 256 else bmp_entry(s, images[s])
        entries.append((s, data))
    offset = 6 + 16 * len(entries)
    ico = struct.pack("<HHH", 0, 1, len(entries))
    body = b""
    for s, data in entries:
        ico += struct.pack("<BBBBHHII", s if s < 256 else 0, s if s < 256 else 0, 0, 0, 1, 32, len(data), offset + len(body))
        body += data
    with open(OUT_ICO, "wb") as f:
        f.write(ico + body)
    with open(OUT_PNG, "wb") as f:
        f.write(png(256, images[256]))
    w = 64
    data = render(w, ss=4)
    with open(OUT_C, "w", newline="\n") as f:
        f.write("/* generated by tools/make_icon.py: the window icon, 64x64 RGBA (original artwork) */\n")
        f.write("#include <stdint.h>\n")
        f.write(f"const int icon_width = {w}, icon_height = {w};\n")
        f.write(f"const uint8_t icon_rgba[{w * w * 4}] = {{\n")
        for i in range(0, len(data), 32):
            f.write("    " + ",".join(str(b) for b in data[i:i + 32]) + ",\n")
        f.write("};\n")
    print("wrote", OUT_ICO, OUT_PNG, OUT_C)


if __name__ == "__main__":
    main()
