#!/usr/bin/env python3
"""Render and verify a sparrow nesting solution.

    tools/plot_nest.py fusenest_instance.json [solution.json] [out.svg]

The point of this is not the picture, it is the checks.

jagua-rs applies a per-item centering pre-transform at import
(`centering_transformation`, i.e. translate by the negative area centroid), but
`int_to_ext_transformation` composes that back out on the way out: the exported
transformation is documented as "applicable to the original shapes". So it maps
OUR polygon directly, and no centroid arithmetic belongs here:

    v' = (x, -y) if reflected       # before rotation
    v' = rotate(v', rotation)       # degrees, counter-clockwise
    v' = v' + translation

That is also why the exported translations look wildly out of range -- the
composition folds -rotate(centroid) into the translation, so a part whose model
centroid sits far from the origin gets a large negative offset.

If the reconstruction is wrong, parts overlap or fall outside the strip, which
the checks below detect. Geometry, not vibes.

SHEETS. A sheet nest is one strip with immovable separator rectangles laid into
the gaps between sheets, so it arrives here as an ordinary solution plus a
`sheets` object describing the geometry. The separators are placements of an
item the INSTANCE file does not define -- the kernel adds it after parsing --
so they are rebuilt from that object rather than looked up.

That brings one check this file exists for: every part must lie wholly within
ONE sheet. It is the invariant the whole scheme rests on, it is checked here
from the geometry rather than from anything the solver says about itself, and
it is the only check that would catch a separator in the wrong place.
"""
import json
import math
import sys


def centroid(pts):
    """Area centroid, matching jagua-rs SPolygon::centroid.

    The signed-area form is winding-independent: reversing the ring flips both
    the cross-product sums and the area, so the quotient is unchanged. That is
    why we need not replicate jagua-rs's CCW normalisation.
    """
    a = cx = cy = 0.0
    n = len(pts)
    for i in range(n):
        xi, yi = pts[i]
        xj, yj = pts[(i + 1) % n]
        cross = xi * yj - xj * yi
        a += cross
        cx += (xi + xj) * cross
        cy += (yi + yj) * cross
    a *= 0.5
    if abs(a) < 1e-12:
        raise ValueError("degenerate polygon")
    return cx / (6.0 * a), cy / (6.0 * a)


def place(pts, tf):
    ang = math.radians(tf["rotation"])
    ca, sa = math.cos(ang), math.sin(ang)
    tx, ty = tf["translation"]
    reflected = tf.get("reflected", False)
    out = []
    for x, y in pts:
        if reflected:
            y = -y
        out.append((x * ca - y * sa + tx, x * sa + y * ca + ty))
    return out


def seg_dist(p, q, r, s):
    """Minimum distance between segments pq and rs; 0.0 if they cross."""
    def sub(a, b):
        return (a[0] - b[0], a[1] - b[1])

    def cross(a, b):
        return a[0] * b[1] - a[1] * b[0]

    d1, d2 = sub(q, p), sub(s, r)
    denom = cross(d1, d2)
    if abs(denom) > 1e-15:
        t = cross(sub(r, p), d2) / denom
        u = cross(sub(r, p), d1) / denom
        if 0.0 <= t <= 1.0 and 0.0 <= u <= 1.0:
            return 0.0

    def pt_seg(pt, a, b):
        ab = sub(b, a)
        L = ab[0] ** 2 + ab[1] ** 2
        if L < 1e-18:
            return math.dist(pt, a)
        t = max(0.0, min(1.0, ((pt[0] - a[0]) * ab[0] + (pt[1] - a[1]) * ab[1]) / L))
        return math.dist(pt, (a[0] + t * ab[0], a[1] + t * ab[1]))

    return min(pt_seg(p, r, s), pt_seg(q, r, s), pt_seg(r, p, q), pt_seg(s, p, q))


def inside(pt, poly):
    x, y = pt
    hit = False
    n = len(poly)
    for i in range(n):
        x1, y1 = poly[i]
        x2, y2 = poly[(i + 1) % n]
        if (y1 > y) != (y2 > y) and x < x1 + (y - y1) / (y2 - y1) * (x2 - x1):
            hit = not hit
    return hit


def bbox(ring):
    xs = [p[0] for p in ring]
    ys = [p[1] for p in ring]
    return min(xs), min(ys), max(xs), max(ys)


def bbox_gap(a, b):
    """Lower bound on the distance between two rings, from their boxes."""
    dx = max(0.0, max(a[0] - b[2], b[0] - a[2]))
    dy = max(0.0, max(a[1] - b[3], b[1] - a[3]))
    return math.hypot(dx, dy)


def clearance(a, b):
    """Min distance between two rings; 0.0 if they intersect or nest."""
    best = float("inf")
    for i in range(len(a)):
        p, q = a[i], a[(i + 1) % len(a)]
        for j in range(len(b)):
            r, s = b[j], b[(j + 1) % len(b)]
            d = seg_dist(p, q, r, s)
            if d == 0.0:
                return 0.0
            best = min(best, d)
    if inside(a[0], b) or inside(b[0], a):
        return 0.0
    return best


def separator_ring(sheets):
    """The separator's original ring, as the kernel defines it.

    Kept deliberately close to `SheetStrip::prepare`: the gap rectangle, inset
    vertically by the separation so that inflating it by half the separation
    gives back exactly the deflated container's height. If the two ever drift
    apart, this file silently checks the wrong rectangle -- which is why the
    numbers come from the solution's own `sheets` object and not from here.
    """
    gap = float(sheets["gap"])
    sep = float(sheets["separation"])
    h = float(sheets["height"]) - 2.0 * sep
    return [(0.0, 0.0), (gap, 0.0), (gap, h), (0.0, h)]


def sheet_of(ring, sheets, tol):
    """Which sheet a part's ring lies wholly within, or None."""
    pitch = float(sheets["pitch"])
    width = float(sheets["width"])
    x0 = min(p[0] for p in ring)
    x1 = max(p[0] for p in ring)
    k = int(x0 // pitch)
    if k < 0 or k >= int(sheets["allocated"]):
        return None
    return k if x1 <= pitch * k + width + tol else None


def main():
    inst_path = sys.argv[1]
    sol_path = sys.argv[2] if len(sys.argv) > 2 else inst_path + ".solution.json"
    out_path = sys.argv[3] if len(sys.argv) > 3 else "nest.svg"

    inst = json.load(open(inst_path))
    sol = json.load(open(sol_path))

    shapes = {it["id"]: it["shape"]["data"] for it in inst["items"]}
    height = float(inst["strip_height"])
    sep = float(inst.get("min_item_separation", 0.0))
    width = float(sol["strip_width"])

    sheets = sol.get("sheets")
    if sheets:
        shapes[sheets["separator_item_id"]] = separator_ring(sheets)

    def is_separator(item_id):
        return bool(sheets) and item_id == sheets["separator_item_id"]

    # Separators stay in `placed`, and so in the clearance check: a part flush
    # against one has to honour the separation like any other neighbour, and
    # that boundary is exactly where a mistake would hide. They are excluded
    # only where they would be counted as PARTS.
    placed = []
    for pi in sol["layout"]["placed_items"]:
        placed.append((pi["item_id"], place(shapes[pi["item_id"]], pi["transformation"])))
    parts = [(i, r) for i, r in placed if not is_separator(i)]

    # --- checks ----------------------------------------------------------
    problems = []

    xs = [p[0] for _, ring in placed for p in ring]
    ys = [p[1] for _, ring in placed for p in ring]
    # Separately, because the separators reach into the gaps and would make the
    # parts' own extent -- the thing being judged -- unreadable.
    pxs = [p[0] for _, ring in parts for p in ring]
    pys = [p[1] for _, ring in parts for p in ring]
    tol = 1e-3
    if min(xs) < -tol or max(xs) > width + tol or min(ys) < -tol or max(ys) > height + tol:
        problems.append(
            "outside strip: x [%.3f, %.3f] vs [0, %.3f], y [%.3f, %.3f] vs [0, %.3f]"
            % (min(xs), max(xs), width, min(ys), max(ys), height)
        )

    # The sheet invariant, checked from the geometry: no part may cross a
    # boundary, and none may sit past the last sheet.
    if sheets:
        per_sheet = [0] * int(sheets["allocated"])
        strays = []
        for item_id, ring in parts:
            k = sheet_of(ring, sheets, tol)
            if k is None:
                strays.append(item_id)
            else:
                per_sheet[k] += 1
        used = max((k + 1 for k, n in enumerate(per_sheet) if n), default=0)
        print("sheets:       %d of %d used, pitch %.3f, %.3f x %.3f each"
              % (used, sheets["allocated"], sheets["pitch"], sheets["width"],
                 sheets["height"]))
        print("per sheet:    %s" % per_sheet)
        if strays:
            # Not a problem with the nest: the sheets were too small for the set,
            # the solver said so, and those parts are excluded from the layout the
            # add-in applies. It is only a problem if the solver did NOT say so,
            # which the agreement checks below catch.
            print("overflow:     %d parts fit on no sheet (%s)"
                  % (len(strays), ", ".join(str(i) for i in strays[:8])
                     + (", ..." if len(strays) > 8 else "")))
        # The real check: the solver's account of its own layout must match the
        # geometry. A separator in the wrong place shows up here and nowhere else.
        if used != sheets["used"]:
            problems.append("the solver reported %s sheets used, the geometry says %d"
                            % (sheets["used"], used))
        if len(strays) != sheets["overflow"]:
            problems.append("the solver reported %s overflowing, the geometry says %d"
                            % (sheets["overflow"], len(strays)))

    # An axis-aligned box test first: exact segment work is O(n*m) per pair and
    # only the near neighbours can ever beat the running minimum.
    #
    # Boxes that OVERLAP are always checked exactly, whatever the running
    # minimum. Pruning them on `>= worst` looks sound and is not: the first
    # overlap found sets worst to 0.0, after which every remaining pair is
    # pruned and every further overlap goes unreported. This checker found one
    # of twelve parts sitting on top of separators that way.
    boxes = [bbox(ring) for _, ring in placed]
    worst = float("inf")
    worst_pair = None
    exact = 0
    for i in range(len(placed)):
        for j in range(i + 1, len(placed)):
            gap = bbox_gap(boxes[i], boxes[j])
            if gap > 0.0 and gap >= worst:
                continue
            exact += 1
            d = clearance(placed[i][1], placed[j][1])
            if d < worst:
                worst, worst_pair = d, (placed[i][0], placed[j][0])
            if d == 0.0:
                problems.append("items %d and %d overlap" % (placed[i][0], placed[j][0]))
    print("pairs checked exactly: %d of %d" % (exact, len(placed) * (len(placed) - 1) // 2))

    print("strip:        %.3f wide x %.3f high" % (width, height))
    print("placed:       %d parts%s"
          % (len(parts), " + %d separators" % (len(placed) - len(parts)) if sheets else ""))
    print("parts bbox:   x [%.3f, %.3f]  y [%.3f, %.3f]"
          % (min(pxs), max(pxs), min(pys), max(pys)))
    print("min clearance %.4f mm between items %s (requested %.3f)" % (worst, worst_pair, sep))
    area = sum(abs(0.5 * sum(r[i][0] * r[(i + 1) % len(r)][1] - r[(i + 1) % len(r)][0] * r[i][1]
                             for i in range(len(r))))
               for _, r in parts)
    # Against the sheets in use, not the whole strip: the empty sheets a caller
    # allocated and the solve did not need are not waste, they were never cut.
    if sheets:
        denom = max(used, 1) * float(sheets["width"]) * float(sheets["height"])
        print("outline area: %.2f mm2 -> %.2f%% of the %d sheets used"
              % (area, 100.0 * area / denom, used))
    else:
        print("outline area: %.2f mm2 -> %.2f%% of strip" % (area, 100.0 * area / (width * height)))

    if problems:
        print("\nPROBLEMS:")
        for p in problems:
            print("  -", p)
    else:
        print("\nOK: inside the strip, no overlaps%s"
              % (", every placed part on one sheet" if sheets else ""))

    # --- svg -------------------------------------------------------------
    # Pad to a SQUARE canvas: several macOS rasterisers (qlmanage among them)
    # force a square thumbnail and crop anything taller, silently losing the
    # bottom of a tall strip.
    pad = 4.0
    side = max(width, height) + 2 * pad
    padx = (side - width) / 2.0
    pady = (side - height) / 2.0
    scale = 10.0
    W = H = side * scale
    palette = ["#4e79a7", "#f28e2c", "#59a14f", "#e15759", "#9c755f",
               "#76b7b2", "#edc949", "#b07aa1"]
    svg = [
        '<svg xmlns="http://www.w3.org/2000/svg" width="%.0f" height="%.0f" '
        'viewBox="0 0 %.3f %.3f">' % (W, H, side, side),
        '<rect x="0" y="0" width="%.3f" height="%.3f" fill="#1e1e1e"/>' % (side, side),
        # scale(1,-1) puts +y upwards, matching the model rather than SVG.
        '<g transform="translate(%.3f,%.3f) scale(1,-1) translate(0,%.3f)">' % (padx, pady, -height),
        '<rect x="0" y="0" width="%.3f" height="%.3f" fill="none" stroke="#888" '
        'stroke-width="0.3" stroke-dasharray="1.5,1.5"/>' % (width, height),
    ]
    # Every ALLOCATED sheet, so the headroom shows.
    if sheets:
        for k in range(int(sheets["allocated"])):
            svg.append('<rect x="%.3f" y="0" width="%.3f" height="%.3f" fill="none" '
                       'stroke="#5a8cc8" stroke-width="0.4"/>'
                       % (float(sheets["pitch"]) * k, float(sheets["width"]),
                          float(sheets["height"])))
    for item_id, ring in placed:
        pts = " ".join("%.4f,%.4f" % (x, y) for x, y in ring)
        if is_separator(item_id):
            svg.append('<polygon points="%s" fill="#5a8cc8" fill-opacity="0.18" '
                       'stroke="none"/>' % pts)
            continue
        colour = palette[item_id % len(palette)]
        svg.append('<polygon points="%s" fill="%s" fill-opacity="0.55" stroke="%s" '
                   'stroke-width="0.25"/>' % (pts, colour, colour))
    svg.append("</g></svg>")

    with open(out_path, "w") as f:
        f.write("\n".join(svg))
    print("wrote", out_path)


if __name__ == "__main__":
    main()
