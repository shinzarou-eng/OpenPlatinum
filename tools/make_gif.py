#!/usr/bin/env python3
"""Assemble les frames --gif d'OpenPlatinum en GIF anime.
Usage: python tools/make_gif.py OUT.gif W H fps [glob_frames]
Les PPM generes par le jeu (builddir/../_shots/gif_*.ppm) sont convertis,
redimensionnes a WxH et sauves en GIF boucle a `fps` images/s."""
import sys, glob
from PIL import Image

out, w, h, fps = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
pat = sys.argv[5] if len(sys.argv) > 5 else "_shots/gif_*.ppm"
stride = int(sys.argv[6]) if len(sys.argv) > 6 else 3   # capture 60fps -> stride 3 = 20fps reel
files = sorted(glob.glob(pat))[::stride]
if not files:
    sys.exit("aucune frame: " + pat)

frames = []
for f in files:
    im = Image.open(f).convert("RGB").resize((w, h), Image.LANCZOS)
    frames.append(im)

frames[0].save(out, save_all=True, append_images=frames[1:],
               duration=int(1000 / fps), loop=0, optimize=True)
print(f"{out}: {len(frames)} frames, {w}x{h}, {fps} fps")
