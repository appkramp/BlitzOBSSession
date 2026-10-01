#!/usr/bin/env python3
"""Draws the four vehicle-class icons as SVG.

They are redrawn from the application's PNGs (Assets.xcassets/class-*.imageset,
38x48 at most), which blur when a streamer scales the overlay up. The geometry
lives here rather than in the SVG files so a shape can be adjusted and every
file regenerated. Only polygons and plain paths are emitted: Qt's SVG renderer
supports SVG Tiny 1.2, which has no masks.

    python3 tools/make_class_icons.py            # writes data/icons/*.svg
    python3 tools/make_class_icons.py --check A  # compares with the PNGs in A
"""
import math
import os
import sys

OUT = os.path.join(os.path.dirname(__file__), "..", "data", "icons")


def clip(poly, a, b, c):
    """Keeps the part of a convex polygon where a*x + b*y <= c."""
    out = []
    for i, p in enumerate(poly):
        q = poly[(i + 1) % len(poly)]
        dp = a * p[0] + b * p[1] - c
        dq = a * q[0] + b * q[1] - c
        if dp <= 0:
            out.append(p)
        if (dp < 0) != (dq < 0) and dp != dq:
            t = dp / (dp - dq)
            out.append((p[0] + t * (q[0] - p[0]), p[1] + t * (q[1] - p[1])))
    return out


def diamond(cx, cy, hw, hh, cuts, gap):
    """A rhombus split into strips by cuts parallel to its upper-left edge.

    `cuts` are positions across the rhombus, 0 at the upper-left edge and 1
    at the lower-right one; `gap` is the width of each cut.
    """
    top, right, bottom, left = (cx, cy - hh), (cx + hw, cy), (cx, cy + hh), (cx - hw, cy)
    poly = [top, right, bottom, left]
    # Normal of the upper-left edge, pointing into the shape.
    ex, ey = top[0] - left[0], top[1] - left[1]
    n = math.hypot(ex, ey)
    a, b = ey / n, -ex / n
    if a * (right[0] - left[0]) + b * (right[1] - left[1]) < 0:
        a, b = -a, -b
    c0 = a * left[0] + b * left[1]
    c1 = a * right[0] + b * right[1]
    bounds = [c0] + [c0 + (c1 - c0) * t for t in cuts] + [c1]
    pieces = []
    for i in range(len(bounds) - 1):
        lo = bounds[i] + (gap / 2 if i > 0 else 0)
        hi = bounds[i + 1] - (gap / 2 if i + 1 < len(bounds) - 1 else 0)
        piece = poly
        if i + 1 < len(bounds) - 1:
            piece = clip(piece, a, b, hi)
        if i > 0:
            piece = clip(piece, -a, -b, -lo)
        pieces.append(piece)
    return pieces


def triangle(x0, x1, top, apex_x, apex_y, r):
    """A downward triangle with rounded corners, as a path."""
    pts = [(x0, top), (x1, top), (apex_x, apex_y)]
    d = []
    for i, p in enumerate(pts):
        prev, nxt = pts[i - 1], pts[(i + 1) % 3]
        def toward(q, dist):
            dx, dy = q[0] - p[0], q[1] - p[1]
            n = math.hypot(dx, dy)
            return (p[0] + dx / n * dist, p[1] + dy / n * dist)
        s, e = toward(prev, r), toward(nxt, r)
        d.append(("M" if i == 0 else "L") + f"{s[0]:.2f},{s[1]:.2f}")
        d.append(f"Q{p[0]:.2f},{p[1]:.2f} {e[0]:.2f},{e[1]:.2f}")
    return "".join(d) + "Z"


def shrunk(k, cx, cy, x0, x1, top, apex_x, apex_y, r):
    """Triangle parameters scaled by k around (cx, cy)."""
    sx = lambda x: cx + (x - cx) * k
    sy = lambda y: cy + (y - cy) * k
    return sx(x0), sx(x1), sy(top), sx(apex_x), sy(apex_y), r * k


SHAPES = {
    #            canvas     geometry
    "lightTank": ((36, 48), lambda: diamond(17.5, 23.3, 14.35, 21.3, [], 0)),
    "mediumTank": ((36, 48), lambda: diamond(17.45, 23.7, 14.55, 20.7, [0.49], 3.3)),
    "heavyTank": ((38, 48), lambda: diamond(18.7, 23.5, 16.0, 21.05, [0.31, 0.68], 2.6)),
    # Scaled to 82% of the app's shape: being wide at the top, the triangle
    # looked heavier than the diamonds at the same height.
    "AT-SPG": ((44, 48), lambda: triangle(*shrunk(0.82, 21.5, 24.0, 2.1, 40.9, 3.7, 21.25, 44.2, 3.2))),
}


def svg(name):
    (w, h), geo = SHAPES[name]
    shape = geo()
    if isinstance(shape, str):
        body = f'<path d="{shape}"/>'
    else:
        body = "".join(
            '<polygon points="' + " ".join(f"{x:.2f},{y:.2f}" for x, y in p) + '"/>'
            for p in shape)
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" '
            f'width="{w}" height="{h}"><g fill="#FFFFFF">{body}</g></svg>\n')


def raster(name, scale=8, out=1):
    """Rasterises the geometry with PIL at `out` times its size, for --check."""
    from PIL import Image, ImageDraw
    (w, h), geo = SHAPES[name]
    shape = geo()
    scale *= out
    img = Image.new("L", (w * scale, h * scale), 0)
    dr = ImageDraw.Draw(img)
    if isinstance(shape, str):
        # Approximate the rounded path by sampling its quadratic segments.
        import re
        nums = [float(v) for v in re.findall(r"-?\d+\.?\d*", shape)]
        pts, i = [], 0
        seq = nums
        # M/L s, Q p e, repeated three times: 6 numbers per corner.
        for k in range(3):
            sx, sy, px, py, ex, ey = seq[k * 6:(k + 1) * 6]
            for t in [j / 16 for j in range(17)]:
                x = (1 - t) ** 2 * sx + 2 * (1 - t) * t * px + t * t * ex
                y = (1 - t) ** 2 * sy + 2 * (1 - t) * t * py + t * t * ey
                pts.append((x * scale, y * scale))
        dr.polygon(pts, fill=255)
    else:
        for p in shape:
            dr.polygon([(x * scale, y * scale) for x, y in p], fill=255)
    return img.resize((w * out, h * out), Image.LANCZOS)


def check(assets):
    from PIL import Image
    for name in SHAPES:
        ref = Image.open(os.path.join(assets, f"class-{name}.imageset", f"class-{name}@3x.png")).convert("RGBA").split()[3]
        got = raster(name)
        a, b = list(ref.getdata()), list(got.getdata())
        inter = sum(min(x, y) for x, y in zip(a, b))
        union = sum(max(x, y) for x, y in zip(a, b))
        print(f"{name:11s} IoU {inter / union:.3f}")


if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "--check":
        check(sys.argv[2])
    else:
        os.makedirs(OUT, exist_ok=True)
        for name in SHAPES:
            with open(os.path.join(OUT, f"class-{name}.svg"), "w") as f:
                f.write(svg(name))
        print("written to", os.path.normpath(OUT))
