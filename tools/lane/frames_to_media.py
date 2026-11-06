#!/usr/bin/env python3
"""Convert lane_console SVG frames to PNG, pick screenshots, and make an MP4.

Rendering uses rsvg-convert (librsvg). cairosvg was tried first and drew the
wrong text in one table cell (an AUTO shot rendered as SEMI) although the SVG
was correct, so it is not used.

Usage: frames_to_media.py frames/ --out docs/media --fps 2 [--shots 0.35,0.98]
  --shots: fractions of the session at which to keep a PNG screenshot
"""
from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("frames", type=pathlib.Path)
    p.add_argument("--out", type=pathlib.Path, required=True)
    p.add_argument("--fps", type=float, default=2.0)
    p.add_argument("--shots", default="0.4,0.99")
    p.add_argument("--name", default="lane_console")
    a = p.parse_args()

    svgs = sorted(a.frames.glob("frame_*.svg"))
    if not svgs:
        raise SystemExit("no frames")
    png_dir = a.frames / "png"
    png_dir.mkdir(exist_ok=True)
    for svg in svgs:
        subprocess.run(["rsvg-convert", "-w", "1400", "-b", "#292929", "-o",
                        str(png_dir / (svg.stem + ".png")), str(svg)], check=True)

    a.out.mkdir(parents=True, exist_ok=True)
    pngs = sorted(png_dir.glob("frame_*.png"))
    for i, frac in enumerate(float(x) for x in a.shots.split(",")):
        src = pngs[min(len(pngs) - 1, int(frac * (len(pngs) - 1)))]
        shutil.copy(src, a.out / f"{a.name}_{i + 1}.png")

    video = a.out / f"{a.name}.mp4"
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", str(a.fps),
                    "-i", str(png_dir / "frame_%04d.png"),
                    "-vf", "scale=trunc(iw/2)*2:trunc(ih/2)*2", "-c:v", "libx264",
                    "-pix_fmt", "yuv420p", str(video)], check=True)
    print(f"{len(pngs)} frames -> {video}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
