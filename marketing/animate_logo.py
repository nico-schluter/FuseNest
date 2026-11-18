#!/usr/bin/env python3
"""
Draw-on animation for the FuseNest logo.

Every element of fusenest.svg is a <circle> or a <line>, so each one has an
exactly computable path length.  That lets us animate stroke-dashoffset from
L -> 0 ("draw the stroke along its path") without ever measuring in a browser,
and it means the same timing model can drive two renderers:

  * an animated SVG (CSS keyframes) -- for web/preview, loops forever
  * a sequence of static SVG frames -- rasterised and encoded to video

Both come from the same schedule, so what you tune in the preview is exactly
what renders.

Usage
-----
    ./animate_logo.py timeline                 # print the schedule, tune, repeat
    ./animate_logo.py preview                  # fusenest_anim.svg + preview.html
    ./animate_logo.py still --t 1.8            # one PNG at t=1.8s
    ./animate_logo.py frames --out /tmp/frames
    ./animate_logo.py video --out fusenest_logo.mov          # ProRes 4444, alpha
    ./animate_logo.py video --out fusenest_logo.mp4 --bg "#ffffff"
    ./animate_logo.py video --out fusenest_logo.webm         # VP9, alpha

Rasterising (still/frames/video) needs an SVG rasteriser; the script uses
`rsvg-convert` if it is on PATH, otherwise CairoSVG:

    brew install cairo
    python3 -m venv venv && ./venv/bin/pip install cairosvg
    ./venv/bin/python animate_logo.py video --out out.mov

Video encoding needs ffmpeg (`brew install ffmpeg`).

Tuning lives in CONFIG below -- that is the only part you should need to edit.
"""

from __future__ import annotations

import argparse
import math
import os
import random
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent

# --------------------------------------------------------------------------
# CONFIG -- everything you are likely to want to tune
# --------------------------------------------------------------------------

CONFIG = {
    "source": HERE / "fusenest.svg",
    "fps": 60,
    # Seconds the finished logo is held before the loop restarts (preview SVG)
    # and before the video ends.
    "hold": 0.70,
    # Seconds of blank before anything starts (useful leader for video edits).
    "lead_in": 0.0,
    # Deterministic jitter.
    "seed": 7,
    # "round" keeps part-drawn tiny arcs from looking like pinpricks, but it
    # extends every line end by half a stroke width vs. the source artwork.
    # Use "butt" for pixel-exact fidelity to fusenest.svg in the final frame.
    "linecap": "round",
    # Each element belongs to exactly one group, decided by its source class:
    #   cls-2 -> "ring"   the big clipped outer circle   (#ee8822, 10.4px)
    #   cls-3 -> "dark"   the logo mark: 5 circles + 13 lines (#ee8822, 4px)
    #   cls-1 -> "light"  the 61 background circles      (#f9ce9b, 4px)
    #
    # Per group:
    #   start        when the FIRST element of the group begins drawing (s)
    #   window       first-element-start -> last-element-END (s). Ignored when
    #                the group holds a single element.
    #   duration     how long one element takes to draw itself (s). Overlap is
    #                the ratio of `duration` to the stagger step, which is
    #                (window - duration) / (n - 1): raise `duration` or shrink
    #                `window` and more elements are in flight at once.
    #   ease         see EASINGS
    #   order        see ORDERINGS -- the order elements start drawing in
    #   jitter       randomises each element's start by +/- jitter*duration.
    #                Measured against `duration`, not the stagger step, so it
    #                still bites when the stagger is dense and overlapping.
    #                It can push the group a little past `window`.
    #   start_angle  degrees; where a circle starts drawing. 0 = 12 o'clock,
    #                90 = 3 o'clock, measured clockwise.
    #   angle_jitter degrees of random spread around `start_angle`, applied
    #                per circle. 0 keeps the whole group starting at
    #                `start_angle`; 360 scatters start points all the way
    #                round. Lines ignore it.
    #   clockwise    direction circles are drawn in
    #   flip_chance  0..1, per element chance of drawing the other way round.
    #                For a line that means starting from its other end.
    #   fade         fraction of `duration` spent fading opacity 0 -> 1.
    #                Keeps very small circles from popping in; 0 disables.
    "groups": {
        "ring": {
            "start": 0.00,
            "window": 0.0,
            "duration": 0.90,
            "ease": "ease_out",
            "order": "source",
            "jitter": 0.0,
            "start_angle": 0.0,
            "angle_jitter": 0.0,     # the ring is the hero sweep: keep it at 12 o'clock
            "clockwise": True,
            "flip_chance": 0.0,
            "fade": 0.0,
        },
        "dark": {
            "start": 0.22,
            "window": 0.95,
            "duration": 0.55,
            "ease": "ease_out",
            "order": "size_desc",
            "jitter": 0.22,
            "start_angle": 0.0,
            "angle_jitter": 360.0,
            "clockwise": True,
            "flip_chance": 0.5,
            "fade": 0.15,
        },
        "light": {
            "start": 0.62,
            "window": 1.30,
            "duration": 0.60,
            "ease": "ease_out_soft",
            "order": "size_desc",
            "jitter": 0.40,
            "start_angle": 0.0,
            "angle_jitter": 360.0,
            "clockwise": True,
            "flip_chance": 0.5,
            "fade": 0.35,
        },
    },
    # Draw order on screen (back to front). Source order puts the light
    # circles behind the dark mark; keep it unless you want that flipped.
    "z_order": ["light", "ring", "dark"],
}

# --------------------------------------------------------------------------
# Easing
# --------------------------------------------------------------------------

# Named cubic-beziers. Values are CSS cubic-bezier(x1,y1,x2,y2) control points,
# evaluated in Python with the same solver a browser uses, so frames and the
# CSS preview agree.
EASINGS = {
    "linear": (0.0, 0.0, 1.0, 1.0),
    "ease_out": (0.22, 1.0, 0.36, 1.0),        # quint-ish, snappy
    "ease_out_soft": (0.33, 1.0, 0.68, 1.0),   # quad-ish, gentle
    "ease_out_back": (0.34, 1.3, 0.64, 1.0),
    "ease_in_out": (0.65, 0.0, 0.35, 1.0),
    "ease_in": (0.55, 0.0, 1.0, 0.45),
}


def _bezier_axis(t: float, a1: float, a2: float) -> float:
    return ((1 - t) ** 3 * 0.0
            + 3 * (1 - t) ** 2 * t * a1
            + 3 * (1 - t) * t ** 2 * a2
            + t ** 3)


def ease(name: str, x: float) -> float:
    """Evaluate a named cubic-bezier easing at progress x in [0, 1]."""
    if x <= 0:
        return 0.0
    if x >= 1:
        return 1.0
    x1, y1, x2, y2 = EASINGS[name]
    if (x1, y1, x2, y2) == (0.0, 0.0, 1.0, 1.0):
        return x
    # Solve x(t) = x for t by bisection (robust; speed is a non-issue here).
    lo, hi = 0.0, 1.0
    for _ in range(60):
        mid = (lo + hi) / 2
        if _bezier_axis(mid, x1, x2) < x:
            lo = mid
        else:
            hi = mid
    t = (lo + hi) / 2
    return _bezier_axis(t, y1, y2)


def css_ease(name: str) -> str:
    x1, y1, x2, y2 = EASINGS[name]
    return f"cubic-bezier({x1},{y1},{x2},{y2})"


# --------------------------------------------------------------------------
# Shapes
# --------------------------------------------------------------------------

SVG_NS = "http://www.w3.org/2000/svg"
ET.register_namespace("", SVG_NS)


@dataclass
class Shape:
    index: int           # document order
    group: str           # ring | dark | light
    cls: str             # original class name
    kind: str            # circle | line
    geom: tuple          # (cx, cy, r) or (x1, y1, x2, y2)
    clip: str | None     # clip-path attribute, preserved verbatim
    length: float = 0.0
    # Filled in by schedule()
    start: float = 0.0
    duration: float = 0.0
    ease: str = "linear"
    fade: float = 0.0
    start_angle: float = 0.0
    clockwise: bool = True

    @property
    def end(self) -> float:
        return self.start + self.duration

    @property
    def center(self) -> tuple[float, float]:
        if self.kind == "circle":
            return self.geom[0], self.geom[1]
        x1, y1, x2, y2 = self.geom
        return (x1 + x2) / 2, (y1 + y2) / 2

    def path_d(self) -> str:
        """The element as a path, so the draw can start wherever we like."""
        if self.kind == "line":
            x1, y1, x2, y2 = self.geom
            if not self.clockwise:  # draw from the other end
                x1, y1, x2, y2 = x2, y2, x1, y1
            return f"M{x1:.3f},{y1:.3f} L{x2:.3f},{y2:.3f}"
        cx, cy, r = self.geom
        start_angle = self.start_angle
        sweep = 1 if self.clockwise else 0
        span = 180.0 if self.clockwise else -180.0

        def pt(deg: float) -> str:
            a = math.radians(deg)
            # 0 deg = 12 o'clock, increasing clockwise.
            return f"{cx + r * math.sin(a):.3f},{cy - r * math.cos(a):.3f}"

        # Two half-arcs: a single arc command cannot describe a full circle.
        return (f"M{pt(start_angle)} "
                f"A{r:.3f},{r:.3f} 0 0,{sweep} {pt(start_angle + span)} "
                f"A{r:.3f},{r:.3f} 0 0,{sweep} {pt(start_angle + 2 * span)}")

    def offset_at(self, t: float) -> tuple[float, float]:
        """(stroke-dashoffset, opacity) at absolute time t."""
        if t <= self.start:
            return self.length, 0.0 if self.fade else 1.0
        if t >= self.end:
            return 0.0, 1.0
        x = (t - self.start) / self.duration
        drawn = ease(self.ease, x)
        if self.fade > 0:
            opacity = min(1.0, x / self.fade)
        else:
            opacity = 1.0
        return self.length * (1 - drawn), opacity


CLASS_TO_GROUP = {"cls-1": "light", "cls-2": "ring", "cls-3": "dark"}


def parse_source(path: Path) -> tuple[str, dict, list[Shape], str]:
    """Return (viewBox, {class: style-decls}, shapes, defs-xml) from the source."""
    tree = ET.parse(path)
    root = tree.getroot()
    view_box = root.get("viewBox", "0 0 486 486")

    styles: dict[str, str] = {}
    for style_el in root.iter(f"{{{SVG_NS}}}style"):
        for rule in (style_el.text or "").split("}"):
            if "{" not in rule:
                continue
            sel, decls = rule.split("{", 1)
            sel = sel.strip()
            if sel.startswith("."):
                styles[sel[1:]] = decls.strip().rstrip(";")

    defs_xml = ""
    for defs in root.iter(f"{{{SVG_NS}}}defs"):
        for child in defs:
            if child.tag == f"{{{SVG_NS}}}style":
                continue
            defs_xml += ET.tostring(child, encoding="unicode")

    shapes: list[Shape] = []
    for el in root.iter():
        tag = el.tag.split("}")[-1]
        if tag not in ("circle", "line"):
            continue
        cls = (el.get("class") or "").strip()
        group = CLASS_TO_GROUP.get(cls)
        if group is None:
            print(f"warning: skipping <{tag}> with unknown class {cls!r}",
                  file=sys.stderr)
            continue
        if tag == "circle":
            geom = (float(el.get("cx")), float(el.get("cy")), float(el.get("r")))
            length = 2 * math.pi * geom[2]
        else:
            geom = (float(el.get("x1")), float(el.get("y1")),
                    float(el.get("x2")), float(el.get("y2")))
            length = math.hypot(geom[2] - geom[0], geom[3] - geom[1])
        shapes.append(Shape(index=len(shapes), group=group, cls=cls, kind=tag,
                            geom=geom, clip=el.get("clip-path"), length=length))
    return view_box, styles, shapes, defs_xml


# --------------------------------------------------------------------------
# Ordering / schedule
# --------------------------------------------------------------------------

CANVAS_CENTER = (243.0, 243.0)


def _sort_key(name: str):
    cx0, cy0 = CANVAS_CENTER

    def radial(s: Shape) -> float:
        x, y = s.center
        return math.hypot(x - cx0, y - cy0)

    def angular(s: Shape) -> float:
        x, y = s.center
        # Clockwise from 12 o'clock.
        return (math.degrees(math.atan2(x - cx0, cy0 - y))) % 360

    keys = {
        "source": lambda s: s.index,
        "size_desc": lambda s: -s.length,
        "size_asc": lambda s: s.length,
        "radial_out": radial,
        "radial_in": lambda s: -radial(s),
        "topdown": lambda s: s.center[1],
        "bottomup": lambda s: -s.center[1],
        "leftright": lambda s: s.center[0],
        "angular": angular,
        "random": lambda s: 0.0,  # handled by the shuffle below
    }
    if name not in keys:
        raise SystemExit(f"unknown order {name!r}; pick one of {sorted(keys)}")
    return keys[name]


ORDERINGS = ["source", "size_desc", "size_asc", "radial_out", "radial_in",
             "topdown", "bottomup", "leftright", "angular", "random"]


def schedule(shapes: list[Shape], cfg: dict) -> float:
    """Assign start/duration/ease to every shape. Returns total timeline length."""
    rng = random.Random(cfg["seed"])
    # Separate stream so changing angles never reshuffles the timing stagger.
    arng = random.Random(cfg["seed"] * 7919 + 1)
    lead = cfg["lead_in"]

    for group, gcfg in cfg["groups"].items():
        members = [s for s in shapes if s.group == group]
        if not members:
            continue
        key = _sort_key(gcfg["order"])
        if gcfg["order"] == "random":
            rng.shuffle(members)
        else:
            members.sort(key=key)

        dur = gcfg["duration"]
        n = len(members)
        # Spread the starts so the last element FINISHES at start + window.
        spread = max(0.0, gcfg["window"] - dur)
        step = spread / (n - 1) if n > 1 else 0.0
        # Jitter scales with the draw duration, not the stagger step: once the
        # stagger is dense the step is only milliseconds wide and jitter tied
        # to it would be invisible.
        jitter = gcfg["jitter"] * dur

        for i, shape in enumerate(members):
            offset = i * step + (rng.uniform(-jitter, jitter) if jitter else 0.0)
            shape.start = lead + gcfg["start"] + max(0.0, offset)
            shape.duration = dur
            shape.ease = gcfg["ease"]
            shape.fade = gcfg["fade"]
            spread_deg = gcfg.get("angle_jitter", 0.0)
            shape.start_angle = gcfg["start_angle"] + (
                arng.uniform(-spread_deg / 2, spread_deg / 2) if spread_deg else 0.0)
            shape.clockwise = gcfg["clockwise"]
            if arng.random() < gcfg.get("flip_chance", 0.0):
                shape.clockwise = not shape.clockwise

    draw_end = max((s.end for s in shapes), default=0.0)
    return draw_end + cfg["hold"]


# --------------------------------------------------------------------------
# Rendering
# --------------------------------------------------------------------------

def _style_attrs(styles: dict, cls: str) -> str:
    """Original class declarations, inlined, minus fill:none handling."""
    decls = styles.get(cls, "fill:none;stroke:#000;stroke-width:4")
    return decls


def render_frame_svg(view_box: str, styles: dict, defs_xml: str,
                     shapes: list[Shape], cfg: dict, t: float,
                     bg: str | None = None) -> str:
    """A static SVG of the animation at absolute time t."""
    parts = [f'<svg xmlns="{SVG_NS}" viewBox="{view_box}">']
    if defs_xml:
        parts.append(f"<defs>{defs_xml}</defs>")
    if bg:
        x, y, w, h = view_box.split()
        parts.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="{bg}"/>')

    for group in cfg["z_order"]:
        gcfg = cfg["groups"][group]
        for shape in [s for s in shapes if s.group == group]:
            offset, opacity = shape.offset_at(t)
            if opacity <= 0.0 and offset >= shape.length:
                continue  # nothing of it is visible yet
            d = shape.path_d()
            clip = f' clip-path="{shape.clip}"' if shape.clip else ""
            op = "" if opacity >= 1.0 else f' opacity="{opacity:.3f}"'
            parts.append(
                f'<path d="{d}" style="{_style_attrs(styles, shape.cls)}" '
                f'stroke-linecap="{cfg["linecap"]}" '
                f'stroke-dasharray="{shape.length:.3f}" '
                f'stroke-dashoffset="{offset:.3f}"{op}{clip}/>')
    parts.append("</svg>")
    return "".join(parts)


def render_animated_svg(view_box: str, styles: dict, defs_xml: str,
                        shapes: list[Shape], cfg: dict, total: float) -> str:
    """A self-contained, infinitely looping CSS-animated SVG."""
    # NB: do not add vector-effect:non-scaling-stroke here. It makes the
    # browser apply the dash pattern in device pixels while stroke-dasharray
    # is computed in user units, so at any scale != 1 the circles never close.
    css = [
        f"path{{stroke-linecap:{cfg['linecap']}}}",
    ]
    for cls, decls in styles.items():
        css.append(f".{cls}{{{decls}}}")

    body = []
    for group in cfg["z_order"]:
        gcfg = cfg["groups"][group]
        for shape in [s for s in shapes if s.group == group]:
            i = shape.index
            p0 = 100.0 * shape.start / total
            p1 = 100.0 * shape.end / total
            L = shape.length
            # Draw: dashoffset L -> 0 across [p0, p1], eased.
            css.append(
                f"@keyframes d{i}{{"
                f"0%,{p0:.4f}%{{stroke-dashoffset:{L:.3f};"
                f"animation-timing-function:{css_ease(shape.ease)}}}"
                f"{p1:.4f}%,100%{{stroke-dashoffset:0}}}}")
            anim = f"d{i} {total:.3f}s linear infinite"
            if shape.fade > 0:
                pf = 100.0 * (shape.start + shape.fade * shape.duration) / total
                css.append(
                    f"@keyframes o{i}{{"
                    f"0%,{p0:.4f}%{{opacity:0}}"
                    f"{pf:.4f}%,100%{{opacity:1}}}}")
                anim += f",o{i} {total:.3f}s linear infinite"
            d = shape.path_d()
            clip = f' clip-path="{shape.clip}"' if shape.clip else ""
            body.append(
                f'<path id="s{i}" class="{shape.cls}" d="{d}"{clip} '
                f'style="stroke-dasharray:{L:.3f};stroke-dashoffset:{L:.3f};'
                f'animation:{anim}"/>')

    return (f'<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{view_box}">\n'
            f'<defs>{defs_xml}</defs>\n'
            f"<style>\n{chr(10).join(css)}\n</style>\n"
            + "\n".join(body) + "\n</svg>\n")


PREVIEW_HTML = """<!doctype html>
<meta charset="utf-8">
<title>FuseNest logo animation</title>
<style>
  :root {{ color-scheme: dark light; }}
  body {{ margin:0; font:14px/1.5 ui-sans-serif,system-ui,sans-serif;
         background:#15120f; color:#f9ce9b; }}
  header {{ padding:16px 24px; display:flex; gap:16px; align-items:center; }}
  button {{ font:inherit; padding:6px 12px; border-radius:6px; cursor:pointer;
           border:1px solid #ee882255; background:#ee88221a; color:inherit; }}
  .grid {{ display:grid; grid-template-columns:repeat(auto-fit,minmax(240px,1fr));
          gap:0; }}
  .cell {{ aspect-ratio:1; display:grid; place-items:center; padding:6%; }}
  .cell img {{ width:100%; height:100%; }}
  .dark {{ background:#15120f; }} .mid {{ background:#3b2a18; }}
  .light {{ background:#faf6f1; }} .white {{ background:#fff; }}
  footer {{ padding:12px 24px; opacity:.6; }}
</style>
<header>
  <strong>FuseNest draw-on</strong>
  <span>total {total:.2f}s &middot; {count} elements</span>
  <button onclick="replay()">replay</button>
</header>
<div class="grid">
  <div class="cell dark"><img src="{svg}" alt=""></div>
  <div class="cell mid"><img src="{svg}" alt=""></div>
  <div class="cell light"><img src="{svg}" alt=""></div>
  <div class="cell white"><img src="{svg}" alt=""></div>
</div>
<footer>Tune CONFIG in animate_logo.py, re-run <code>./animate_logo.py preview</code>, reload.</footer>
<script>
function replay() {{
  for (const img of document.images) {{
    const src = img.src.split('?')[0];
    img.src = src + '?' + Date.now();
  }}
}}
</script>
"""


# --------------------------------------------------------------------------
# Rasterising
# --------------------------------------------------------------------------

class Rasteriser:
    """rsvg-convert if available, else CairoSVG."""

    def __init__(self, size: int):
        self.size = size
        self.rsvg = shutil.which("rsvg-convert")
        self.cairosvg = None
        if not self.rsvg:
            os.environ.setdefault("DYLD_FALLBACK_LIBRARY_PATH", "/opt/homebrew/lib")
            try:
                import cairosvg  # noqa: PLC0415
                self.cairosvg = cairosvg
            except Exception as exc:  # pragma: no cover
                raise SystemExit(
                    "No SVG rasteriser. Install one of:\n"
                    "  brew install librsvg            # provides rsvg-convert\n"
                    "  brew install cairo && pip install cairosvg\n"
                    f"(cairosvg import failed: {exc})")

    def to_png(self, svg: str, out: Path) -> None:
        if self.rsvg:
            subprocess.run(
                [self.rsvg, "-w", str(self.size), "-h", str(self.size),
                 "-o", str(out)],
                input=svg.encode(), check=True)
        else:
            self.cairosvg.svg2png(bytestring=svg.encode(), write_to=str(out),
                                  output_width=self.size, output_height=self.size)


def write_frames(out_dir: Path, view_box, styles, defs_xml, shapes, cfg,
                 total: float, size: int, bg: str | None) -> int:
    out_dir.mkdir(parents=True, exist_ok=True)
    for f in out_dir.glob("frame_*.png"):
        f.unlink()
    raster = Rasteriser(size)
    fps = cfg["fps"]
    n = int(round(total * fps))
    for i in range(n):
        t = i / fps
        svg = render_frame_svg(view_box, styles, defs_xml, shapes, cfg, t, bg)
        raster.to_png(svg, out_dir / f"frame_{i:05d}.png")
        if i % 20 == 0 or i == n - 1:
            print(f"\r  frame {i + 1}/{n}", end="", flush=True)
    print()
    return n


def encode(frames_dir: Path, out: Path, fps: int, bg: str | None) -> None:
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise SystemExit("ffmpeg not found (brew install ffmpeg)")
    ext = out.suffix.lower()
    inputs = ["-framerate", str(fps), "-i", str(frames_dir / "frame_%05d.png")]
    if ext == ".mov":
        # ProRes 4444 keeps the alpha channel; drops straight into any NLE.
        codec = ["-c:v", "prores_ks", "-profile:v", "4444", "-pix_fmt", "yuva444p10le"]
    elif ext == ".webm":
        codec = ["-c:v", "libvpx-vp9", "-pix_fmt", "yuva420p", "-crf", "24", "-b:v", "0"]
    elif ext == ".mp4":
        if not bg:
            print("note: mp4 cannot carry alpha; compositing on white. "
                  "Use --bg to change, or output .mov/.webm for transparency.")
        codec = ["-c:v", "libx264", "-crf", "16", "-pix_fmt", "yuv420p"]
    elif ext == ".gif":
        codec = ["-filter_complex", "split[a][b];[a]palettegen[p];[b][p]paletteuse"]
    else:
        raise SystemExit(f"unsupported output {ext}; use .mov, .webm, .mp4 or .gif")
    cmd = [ffmpeg, "-y", *inputs, *codec, str(out)]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)


# --------------------------------------------------------------------------
# Commands
# --------------------------------------------------------------------------

def load(cfg: dict):
    view_box, styles, shapes, defs_xml = parse_source(Path(cfg["source"]))
    total = schedule(shapes, cfg)
    return view_box, styles, defs_xml, shapes, total


def cmd_timeline(args, cfg):
    _, _, _, shapes, total = load(cfg)
    print(f"{'#':>3} {'group':<6} {'kind':<7} {'len':>8} {'start':>7} {'end':>7}")
    for s in sorted(shapes, key=lambda s: (s.start, s.index)):
        print(f"{s.index:>3} {s.group:<6} {s.kind:<7} {s.length:>8.1f} "
              f"{s.start:>7.3f} {s.end:>7.3f}")
    print()
    for group in cfg["groups"]:
        members = [s for s in shapes if s.group == group]
        if not members:
            continue
        print(f"{group:<6} n={len(members):<3} "
              f"draws {min(s.start for s in members):.2f}s "
              f"-> {max(s.end for s in members):.2f}s")
    print(f"\ntotal {total:.2f}s (incl. {cfg['hold']:.2f}s hold) "
          f"= {int(round(total * cfg['fps']))} frames @ {cfg['fps']}fps")


def cmd_preview(args, cfg):
    view_box, styles, defs_xml, shapes, total = load(cfg)
    svg_path = Path(args.out or HERE / "fusenest_anim.svg")
    svg_path.write_text(render_animated_svg(view_box, styles, defs_xml,
                                            shapes, cfg, total))
    html_path = svg_path.with_name("preview.html")
    html_path.write_text(PREVIEW_HTML.format(svg=svg_path.name, total=total,
                                             count=len(shapes)))
    print(f"wrote {svg_path}  ({total:.2f}s loop, {len(shapes)} elements)")
    print(f"wrote {html_path}")


def cmd_still(args, cfg):
    view_box, styles, defs_xml, shapes, total = load(cfg)
    svg = render_frame_svg(view_box, styles, defs_xml, shapes, cfg, args.t, args.bg)
    out = Path(args.out or HERE / f"still_{args.t:.2f}.png")
    if out.suffix.lower() == ".svg":
        out.write_text(svg)
    else:
        Rasteriser(args.size).to_png(svg, out)
    print(f"wrote {out} (t={args.t:.2f}s of {total:.2f}s)")


def cmd_frames(args, cfg):
    view_box, styles, defs_xml, shapes, total = load(cfg)
    out_dir = Path(args.out)
    n = write_frames(out_dir, view_box, styles, defs_xml, shapes, cfg,
                     total, args.size, args.bg)
    print(f"wrote {n} frames to {out_dir}")


def cmd_video(args, cfg):
    view_box, styles, defs_xml, shapes, total = load(cfg)
    out = Path(args.out)
    bg = args.bg
    if out.suffix.lower() in (".mp4", ".gif") and bg is None:
        bg = "#ffffff"
    with tempfile.TemporaryDirectory() as tmp:
        frames_dir = Path(tmp)
        print(f"rendering {total:.2f}s at {cfg['fps']}fps, {args.size}px...")
        write_frames(frames_dir, view_box, styles, defs_xml, shapes, cfg,
                     total, args.size, bg)
        print("encoding...")
        encode(frames_dir, out, cfg["fps"], bg)
    print(f"wrote {out} ({total:.2f}s, {args.size}x{args.size})")


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--fps", type=int, help="override CONFIG fps")
    p.add_argument("--seed", type=int, help="override CONFIG jitter seed")
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("timeline", help="print the computed schedule")
    s.set_defaults(func=cmd_timeline)

    s = sub.add_parser("preview", help="animated SVG + preview.html")
    s.add_argument("--out", help="output .svg (default marketing/fusenest_anim.svg)")
    s.set_defaults(func=cmd_preview)

    s = sub.add_parser("still", help="one frame as PNG (or SVG)")
    s.add_argument("--t", type=float, required=True, help="time in seconds")
    s.add_argument("--out")
    s.add_argument("--size", type=int, default=1080)
    s.add_argument("--bg", default=None)
    s.set_defaults(func=cmd_still)

    s = sub.add_parser("frames", help="PNG sequence")
    s.add_argument("--out", required=True, help="output directory")
    s.add_argument("--size", type=int, default=1080)
    s.add_argument("--bg", default=None)
    s.set_defaults(func=cmd_frames)

    s = sub.add_parser("video", help="encode a video (.mov/.webm/.mp4/.gif)")
    s.add_argument("--out", required=True)
    s.add_argument("--size", type=int, default=1080)
    s.add_argument("--bg", default=None,
                   help="background colour; default transparent for .mov/.webm")
    s.set_defaults(func=cmd_video)

    args = p.parse_args()
    cfg = dict(CONFIG)
    if args.fps:
        cfg["fps"] = args.fps
    if args.seed is not None:
        cfg["seed"] = args.seed
    args.func(args, cfg)


if __name__ == "__main__":
    main()
