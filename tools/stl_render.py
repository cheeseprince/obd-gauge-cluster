#!/usr/bin/env python3
"""
stl_render.py — isometric preview renders of binary STL files.

Written because the usual suspects (blender, openscad, trimesh, f3d) are not
installed on this box and a case STL is much easier to review as a picture than
as a bounding box. Pure numpy + Pillow: parse the mesh, project it
isometrically, rasterize with a z-buffer, shade each facet from its own normal.

  ./stl_render.py case/*.stl -o renders/ [--size 900] [--workers N]

Output is one PNG per STL, supersampled 3x and downsampled for clean edges.
Renders are parallelized one-STL-per-worker (see --workers).
"""
import argparse
import os
import struct
import sys
from multiprocessing import Pool
from pathlib import Path

import numpy as np
from PIL import Image

# Isometric camera: yaw 45 deg about Z, then pitch 35.264 deg (atan(1/sqrt(2)))
# about X — the classic true-isometric orientation where the three axes are
# 120 deg apart on screen.
YAW = np.radians(45.0)
PITCH = np.radians(35.264)

LIGHT = np.array([-0.4, -0.55, 0.75])       # over-the-left-shoulder key light
LIGHT = LIGHT / np.linalg.norm(LIGHT)
AMBIENT = 0.28
BASE_COLOR = np.array([0.62, 0.66, 0.72])   # cool light grey, prints legibly
BG = 255                                     # white background
SUPERSAMPLE = 3


def load_binary_stl(path: Path) -> tuple[np.ndarray, np.ndarray]:
    """Return (tris, normals) as float32 arrays shaped (N,3,3) and (N,3)."""
    with open(path, "rb") as fh:
        header = fh.read(84)
        if len(header) < 84:
            raise ValueError(f"{path}: too short to be a binary STL")
        count = struct.unpack("<I", header[80:84])[0]
        body = fh.read(count * 50)
    if len(body) != count * 50:
        raise ValueError(f"{path}: truncated (expected {count} facets)")

    raw = np.frombuffer(body, dtype=np.uint8).reshape(count, 50)
    floats = raw[:, :48].copy().view(np.float32).reshape(count, 4, 3)
    normals = floats[:, 0, :]
    tris = floats[:, 1:4, :]
    return tris.astype(np.float32), normals.astype(np.float32)


def project(tris: np.ndarray, size: int) -> np.ndarray:
    """Rotate into the isometric view and scale to fit `size` with a margin."""
    cy, sy = np.cos(YAW), np.sin(YAW)
    rz = np.array([[cy, -sy, 0], [sy, cy, 0], [0, 0, 1]], dtype=np.float32)
    cp, sp = np.cos(PITCH), np.sin(PITCH)
    rx = np.array([[1, 0, 0], [0, cp, -sp], [0, sp, cp]], dtype=np.float32)

    pts = tris.reshape(-1, 3) @ rz.T @ rx.T
    mins, maxs = pts.min(axis=0), pts.max(axis=0)
    span = (maxs - mins)[:2].max()
    margin = size * 0.06
    scale = (size - 2 * margin) / span

    centre = (mins + maxs) / 2.0
    pts[:, 0] = (pts[:, 0] - centre[0]) * scale + size / 2.0
    pts[:, 1] = (centre[1] - pts[:, 1]) * scale + size / 2.0   # flip Y for image space
    pts[:, 2] = (pts[:, 2] - mins[2]) * scale                  # depth, larger = nearer
    return pts.reshape(-1, 3, 3)


def shade(normals: np.ndarray) -> np.ndarray:
    """Lambertian intensity per facet, from the STL's own face normals."""
    lengths = np.linalg.norm(normals, axis=1, keepdims=True)
    lengths[lengths == 0] = 1.0
    unit = normals / lengths
    # Normals are shaded in model space rotated the same way as the geometry.
    cy, sy = np.cos(YAW), np.sin(YAW)
    rz = np.array([[cy, -sy, 0], [sy, cy, 0], [0, 0, 1]], dtype=np.float32)
    cp, sp = np.cos(PITCH), np.sin(PITCH)
    rx = np.array([[1, 0, 0], [0, cp, -sp], [0, sp, cp]], dtype=np.float32)
    view_n = unit @ rz.T @ rx.T
    diffuse = np.clip(view_n @ LIGHT, 0.0, 1.0)
    return np.clip(AMBIENT + (1.0 - AMBIENT) * diffuse, 0.0, 1.0)


def rasterize(tris: np.ndarray, intensity: np.ndarray, size: int) -> np.ndarray:
    """Z-buffered scanline fill. Returns an (size,size,3) uint8 image."""
    img = np.full((size, size, 3), BG, dtype=np.uint8)
    zbuf = np.full((size, size), -np.inf, dtype=np.float32)

    xs, ys, zs = tris[:, :, 0], tris[:, :, 1], tris[:, :, 2]
    x0 = np.clip(np.floor(xs.min(axis=1)).astype(int), 0, size - 1)
    x1 = np.clip(np.ceil(xs.max(axis=1)).astype(int), 0, size - 1)
    y0 = np.clip(np.floor(ys.min(axis=1)).astype(int), 0, size - 1)
    y1 = np.clip(np.ceil(ys.max(axis=1)).astype(int), 0, size - 1)

    # Draw far-to-near so the z-test does less work on average.
    order = np.argsort(zs.mean(axis=1))

    for i in order:
        if x1[i] < x0[i] or y1[i] < y0[i]:
            continue
        ax, ay, az = xs[i, 0], ys[i, 0], zs[i, 0]
        bx, by, bz = xs[i, 1], ys[i, 1], zs[i, 1]
        cx, cy_, cz = xs[i, 2], ys[i, 2], zs[i, 2]

        area = (bx - ax) * (cy_ - ay) - (by - ay) * (cx - ax)
        if abs(area) < 1e-9:
            continue

        gy, gx = np.mgrid[y0[i]:y1[i] + 1, x0[i]:x1[i] + 1]
        px, py = gx + 0.5, gy + 0.5
        w0 = ((bx - ax) * (py - ay) - (by - ay) * (px - ax)) / area
        w1 = ((cx - bx) * (py - by) - (cy_ - by) * (px - bx)) / area
        w2 = 1.0 - w0 - w1
        inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
        if not inside.any():
            continue

        # Barycentric weights map to the OPPOSITE vertex: w0 spans a->b->P
        # relative to c, so depth interpolates as w1*az + w2*bz + w0*cz.
        depth = w1 * az + w2 * bz + w0 * cz
        sub_z = zbuf[y0[i]:y1[i] + 1, x0[i]:x1[i] + 1]
        win = inside & (depth > sub_z)
        if not win.any():
            continue
        sub_z[win] = depth[win]
        colour = (BASE_COLOR * intensity[i] * 255.0).astype(np.uint8)
        img[y0[i]:y1[i] + 1, x0[i]:x1[i] + 1][win] = colour

    return img


def render_one(job: tuple[str, str, int]) -> str:
    src, outdir, size = job
    path = Path(src)
    tris, normals = load_binary_stl(path)
    big = size * SUPERSAMPLE
    projected = project(tris, big)
    img = rasterize(projected, shade(normals), big)
    out = Path(outdir) / f"{path.stem}.png"
    Image.fromarray(img).resize((size, size), Image.LANCZOS).save(out)
    return f"{path.name}: {len(tris)} facets -> {out}"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("stl", nargs="+", help="binary STL file(s)")
    ap.add_argument("-o", "--out", default=".", help="output directory")
    ap.add_argument("--size", type=int, default=900, help="output edge in pixels")
    ap.add_argument("--workers", type=int, default=min(os.cpu_count() or 1, 4))
    args = ap.parse_args(argv)

    Path(args.out).mkdir(parents=True, exist_ok=True)
    jobs = [(s, args.out, args.size) for s in args.stl]
    if len(jobs) == 1 or args.workers == 1:
        for j in jobs:
            print(render_one(j))
    else:
        with Pool(min(args.workers, len(jobs))) as pool:
            for line in pool.imap_unordered(render_one, jobs):
                print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
