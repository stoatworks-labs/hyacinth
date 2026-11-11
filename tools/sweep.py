"""Render every parameter at both ends of its range and fail if any made no
difference.

**This is the only thing in the repo that catches a dead control.** A GLSL
uniform whose name does not match the C++ is silently ignored --
`glGetUniformLocation` returns -1 and `glUniform` on -1 is a documented no-op
-- so a slider can be stone dead while everything compiles, links, loads and
renders.

Each control is swept where it can act. Every render is 150 frames (2.5 s at
60 fps) of the harness's street: string lights, lamps and town windows for
highlights, a checker ground receding to a horizon, an orange ball rolling
across it, the focus point on the ball. Radius acts only with Depth From
Radial, Depth Edges only through the guided filter (Haze here), Focus Depth
only under Manual focus, Track Size and Grab only while tracking, the orbs'
shape only with some Orbs, the flare's parts only with some Flare from a
Light. The aperture's own controls act on discs, which the default Blur
Size leaves too small at 320x180: they are swept with Blur Size at its top.
Focus X acts only on a depth that changes across the frame. Onion Rings are
drawn only where a ring is two texels wide, which no disc at 320x180 is:
that one sweep renders at 1280x720. An option is set by
its element's name. A control whose ends differ by less than `--floor` (mean
8-bit difference per channel) is reported as barely alive: vectrix's lesson.

Usage::

    tools/sweep.py [--build BUILD_DIR] [--verbose] [--jobs N] [--bare]

`--bare` drops every per-control context (keeping only the base), the
honesty check: what goes dead there is the list of controls that only act
with another one set, which AGENTS.md records.
"""

import argparse
import concurrent.futures
import pathlib
import re
import subprocess
import sys
import tempfile
import zlib

REPO = pathlib.Path(__file__).resolve().parent.parent

BASE = []

RADIAL = ["Depth From=Radial"]
MANUAL = ["Focus=Manual"]
TRACK = ["Focus=Track"]
ORBS = ["Orbs=0.6"]
FLARE = ["Flare=0.6", "Flare From=Light"]
BIG = [["--size", "1280x720"], "Mode=Bokeh Only", "Blur Size=1"]
# At 320x180 the default Blur Size leaves the street's lights 2 px circles of
# confusion, under the 3 px where a highlight becomes a sprite: the
# aperture's controls act on discs, so they are swept on wide ones.
WIDE = ["Blur Size=1"]
# The default plane recedes straight up the frame: its depth does not change
# across it, so Focus X has nothing to move until the plane is turned.
TURNED = ["Plane Angle=0.8"]
HAZE = ["Depth From=Haze"]

# name -> (low settings, high settings, context settings)
SWEEP = {
    "Mode": (["Mode=Depth of Field"], ["Mode=Bokeh Only"], []),
    "Blur Size": (["Blur Size=0.2"], ["Blur Size=0.6"], []),
    "Depth From": (["Depth From=Plane"], ["Depth From=Radial"], []),
    "Plane Angle": (["Plane Angle=0.7"], ["Plane Angle=0.8"], []),
    "Horizon": (["Horizon=0.2"], ["Horizon=0.5"], []),
    "Radius": (["Radius=0.4"], ["Radius=0.8"], RADIAL),
    # Not Luma: luma is an exact linear function of the colour, which the
    # colour guided filter returns whatever its window.
    "Depth Edges": (["Depth Edges=0.2"], ["Depth Edges=0.8"], HAZE),
    # Under Point focus the focus inverts with the depth: only which side of
    # focus a thing is on changes. Manual focus holds still -- off 0.5, about
    # which inverting is a mirror.
    "Depth Invert": (["Depth Invert=0"], ["Depth Invert=1"], MANUAL + ["Focus Depth=0.3"]),
    "Focus": (["Focus=Manual"], ["Focus=Point"], []),
    "Focus Depth": (["Focus Depth=0.3"], ["Focus Depth=0.7"], MANUAL),
    "Focus X": (["Focus X=0.3"], ["Focus X=0.7"], TURNED),
    "Focus Y": (["Focus Y=0.2"], ["Focus Y=0.6"], []),
    # Grabbed afresh at the focus point once the ball has rolled off it.
    "Grab": ([], [["--at", "100", "Grab=1"]], TRACK),
    "Track Size": (["Track Size=0.3"], ["Track Size=0.8"], TRACK + ["Show=Tracker"]),
    # The focus pulled ten frames before the end, slowly or quickly.
    "Focus Speed": (["Focus Speed=0.1"], ["Focus Speed=0.6"], MANUAL + [["--at", "140", "Focus Depth=0.9"]]),
    "Shape": (["Shape=Iris"], ["Shape=Heart"], WIDE),
    "Blades": (["Blades=0.2"], ["Blades=0.8"], WIDE + ["Blade Curve=0"]),
    "Blade Curve": (["Blade Curve=0"], ["Blade Curve=1"], WIDE),
    # Half of a heptagon's step (0.07 of a turn), on the large discs.
    "Iris Rotate": (["Iris Rotate=0"], ["Iris Rotate=0.07"], BIG + ["Blade Curve=0"]),
    "Cat Eye": (["Cat Eye=0"], ["Cat Eye=0.8"], WIDE),
    "Swirl": (["Swirl=0"], ["Swirl=1"], WIDE),
    "Squeeze": (["Squeeze=0"], ["Squeeze=0.8"], WIDE),
    "Bubble": (["Bubble=0.2"], ["Bubble=0.9"], WIDE),
    "Onion Rings": (["Onion Rings=0"], ["Onion Rings=1"], BIG),
    "Fringe": (["Fringe=0"], ["Fringe=0.8"], WIDE),
    "Highlights": (["Highlights=0.2"], ["Highlights=0.8"], []),
    "Threshold": (["Threshold=0.5"], ["Threshold=0.9"], []),
    "Orbs": (["Orbs=0"], ["Orbs=0.6"], []),
    "Orb Count": (["Orb Count=0.2"], ["Orb Count=0.8"], ORBS),
    "Orb Size": (["Orb Size=0.2"], ["Orb Size=0.8"], ORBS),
    "Orb Drift": (["Orb Drift=0"], ["Orb Drift=1"], ORBS),
    "Flare": (["Flare=0"], ["Flare=0.6"], []),
    "Flare From": (["Flare From=Highlights"], ["Flare From=Light"], ["Flare=0.6"]),
    "Light X": (["Light X=0.3"], ["Light X=0.7"], FLARE),
    "Light Y": (["Light Y=0.3"], ["Light Y=0.7"], FLARE),
    "Ghosts": (["Ghosts=0"], ["Ghosts=1"], FLARE),
    "Halo": (["Halo=0"], ["Halo=1"], FLARE),
    "Starburst": (["Starburst=0"], ["Starburst=1"], FLARE + ["Blade Curve=0"]),
    "Streak": (["Streak=0"], ["Streak=1"], FLARE),
    "Dirt": (["Dirt=0"], ["Dirt=1"], FLARE),
    "Show": (["Show=Picture"], ["Show=Depth"], []),
    "Mix": (["Mix=0"], ["Mix=1"], []),
}

# The About block: a text line and browser buttons, with no pixel to sweep.
SKIP = {"About", "Project page", "Source on GitHub", "Support the work"}


def read_png(path):
    """Enough of PNG for hytest's own writer: 8-bit RGBA, filter 0 rows."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, width, height, idat = 8, 0, 0, b""
    while pos < len(data):
        length = int.from_bytes(data[pos:pos + 4], "big")
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width = int.from_bytes(body[0:4], "big")
            height = int.from_bytes(body[4:8], "big")
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    out = bytearray()
    for y in range(height):
        start = y * (stride + 1)
        out += raw[start + 1:start + 1 + stride]
    return bytes(out)


def difference(a, b):
    if len(a) != len(b):
        return 255.0
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


def render(hytest, out, settings):
    args = [str(hytest), "--out", str(out), "--size", "320x180", "--frames", "150"]
    for setting in settings:
        args += list(setting) if isinstance(setting, list) else ["--set", setting]
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"hytest failed: {' '.join(args)}\n{result.stderr.strip()}")
    return read_png(out)


def parameters(hytest):
    result = subprocess.run([str(hytest), "--list"], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"hytest --list failed: {result.stderr.strip()}")
    names = []
    for line in result.stdout.splitlines()[1:]:
        parts = re.split(r"\s{2,}", line.strip())
        if len(parts) >= 3 and parts[0].isdigit():
            names.append(parts[1].strip())
    return names


def sweep_one(hytest, scratch, name, bare):
    low, high, context = SWEEP[name]
    context = BASE + ([] if bare else context)
    tag = f"p{abs(hash(name))}"
    before = render(hytest, scratch / f"{tag}-a.png", context + low)
    after = render(hytest, scratch / f"{tag}-b.png", context + high)
    return name, difference(before, after)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=pathlib.Path, default=REPO / "build")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--bare", action="store_true")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--floor", type=float, default=0.05,
                        help="mean 8-bit difference below which a control is 'barely alive'")
    args = parser.parse_args()

    build = args.build if args.build.is_absolute() else REPO / args.build
    hytest = build / "hytest"
    if not hytest.exists():
        print(f"{hytest} not found", file=sys.stderr)
        return 1

    declared = parameters(hytest)
    unknown = [n for n in declared if n not in SWEEP and n not in SKIP]
    if unknown:
        # A new parameter with no sweep is a hole, not a pass.
        print(f"no sweep defined for: {', '.join(unknown)}", file=sys.stderr)
        return 1
    unused = [n for n in SWEEP if n not in declared]
    if unused:
        print(f"the sweep names parameters the plugin does not have: {', '.join(unused)}", file=sys.stderr)
        return 1

    dead, weak = [], []
    with tempfile.TemporaryDirectory() as scratch:
        scratch = pathlib.Path(scratch)
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            results = list(pool.map(lambda n: sweep_one(hytest, scratch, n, args.bare), [n for n in declared if n in SWEEP]))
    for name, delta in results:
        if delta == 0.0:
            dead.append(name)
            print(f"  DEAD {name:16s} both ends identical")
        elif delta < args.floor:
            weak.append(name)
            print(f"  WEAK {name:16s} mean delta {delta:.4f}")
        elif args.verbose:
            print(f"  ok   {name:16s} mean delta {delta:.3f}")

    print(f"{len(results)} parameters swept{' (bare: base context only)' if args.bare else ''}, "
          f"{len(dead)} dead, {len(weak)} barely alive")
    if dead or weak:
        if not args.bare:
            print("\nA parameter that changes nothing is usually a uniform name that does not match\n"
                  "the C++, or a setting nothing reads. Both are silent everywhere else.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
