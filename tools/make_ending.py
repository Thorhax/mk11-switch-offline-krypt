#!/usr/bin/env python3
"""Converts a video into a Klassic Tower ending for Cyrax, Sektor or Kronika.

    python3 tools/make_ending.py <video> <Cyrax|Sektor|Kronika> [--out DIR]

Writes <DIR>/<KEY>_Ending.mp4 (CYR, SEK or KRO; default DIR ./endings_out) in the format of the game's own
endings: H.264 High (level 3.1) 1280x720 at 29.97 fps, then six AAC stereo 48 kHz tracks (one per game language,
all the video's own audio) stored before the video track. Copy it to
sdmc:/atmosphere/contents/0100F2200C984000/romfs/Movies/ on the SD card; the mod lists it in the game's table of
contents at boot and plays it after the tower's final fight. Needs ffmpeg.
"""
import argparse
import os
import subprocess
import sys

KEYS = {"Cyrax": "CYR", "Sektor": "SEK", "Kronika": "KRO"}
LANGUAGES = ("eng", "fra", "deu", "ita", "por", "spa")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("video")
    ap.add_argument("name", choices=KEYS)
    ap.add_argument("--out", default="endings_out")
    a = ap.parse_args()

    os.makedirs(a.out, exist_ok=True)
    out = os.path.join(a.out, f"{KEYS[a.name]}_Ending.mp4")
    cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", a.video]
    for _ in LANGUAGES:
        cmd += ["-map", "0:a:0"]
    cmd += ["-map", "0:v:0",
            "-c:a", "aac", "-b:a", "160k", "-ar", "48000", "-ac", "2",
            "-c:v", "libx264", "-profile:v", "high", "-level:v", "3.1", "-pix_fmt", "yuv420p",
            "-vf", "scale=1280:720:flags=lanczos,fps=30000/1001",
            "-b:v", "2500k", "-maxrate", "4000k", "-bufsize", "6000k", "-bf", "2", "-refs", "1"]
    for i, lang in enumerate(LANGUAGES):
        cmd += [f"-metadata:s:{i}", f"language={lang}"]
    cmd += [f"-metadata:s:{len(LANGUAGES)}", "language=und", out]
    subprocess.run(cmd, check=True)
    print(f"{out} ({os.path.getsize(out)} bytes)")


if __name__ == "__main__":
    sys.exit(main())
