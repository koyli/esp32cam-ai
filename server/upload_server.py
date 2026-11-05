#!/usr/bin/env python3
"""Receives file uploads from the ESP32 and saves them to UPLOAD_DIR."""

import os
import re
import datetime
from flask import Flask, request

app = Flask(__name__)

UPLOAD_DIR = os.environ.get("UPLOAD_DIR", "/home/ubuntu/recordings")
# Trail-camera files go where trailcam-review looks for them.
TRAILCAM_DIR = os.environ.get("TRAILCAM_DIR", "/home/ubuntu/capture/trailcam")
# Motion clips too, as camera "motion:esp32cam". Their summaries stay in UPLOAD_DIR:
# trailcam-review ignores .txt files and would leave them behind when it moves a clip.
MOTION_DIR = os.environ.get("MOTION_DIR", "/home/ubuntu/capture/motion/esp32cam")
_SAFE_NAME = re.compile(r'^[\w\.\-]+\.(avi|txt)$')
_TRAILCAM_NAME = re.compile(r'^[A-Za-z0-9]+_\d{14}_\d+\.(jpg|mp4)$', re.I)


def _split_ext(filename):
    """Split off the extension, keeping the clip's ".avi.txt" summary suffix whole."""
    if filename.endswith(".avi.txt"):
        return filename[:-len(".avi.txt")], ".avi.txt"
    return os.path.splitext(filename)


def _open_unique(filename):
    """Open a new file for writing, adding _1, _2, ... rather than overwriting."""
    stem, ext = _split_ext(filename)
    candidate = filename
    for i in range(1, 1000):
        try:
            return candidate, open(os.path.join(UPLOAD_DIR, candidate), "xb")
        except FileExistsError:
            candidate = f"{stem}_{i}{ext}"
    raise RuntimeError(f"too many files named {filename}")


def _receive(directory, filename):
    """Write the request body to a hidden temporary name, which trailcam-review
    skips, so that a half-written file is never reviewed. Returns its path."""
    tmp = os.path.join(directory, f".{filename}.part")
    with open(tmp, "wb") as f:
        while chunk := request.stream.read(1 << 16):
            f.write(chunk)
    return tmp


def _save_clip(filename):
    """Move a received clip into MOTION_DIR, adding _1, _2, ... rather than overwriting."""
    tmp = _receive(MOTION_DIR, filename)
    stem, ext = _split_ext(filename)
    candidate = filename
    for i in range(1, 1000):
        try:
            os.link(tmp, os.path.join(MOTION_DIR, candidate))
            break
        except FileExistsError:
            candidate = f"{stem}_{i}{ext}"
    else:
        raise RuntimeError(f"too many files named {filename}")
    os.unlink(tmp)
    size = os.path.getsize(os.path.join(MOTION_DIR, candidate))
    print(f"saved motion/{candidate} ({size / 1024:.0f} KB)", flush=True)
    return "OK", 201


def _save_trailcam(raw):
    """Save a trail-camera file under its own name, replacing any earlier copy.

    The device re-sends a file whose upload it didn't see succeed, so a repeat is
    the same file.
    """
    filename = os.path.basename(raw)
    if not _TRAILCAM_NAME.match(filename):
        return "bad filename", 400
    stem, ext = os.path.splitext(filename)
    filename = stem + ext.lower()  # trailcam-review matches .jpg/.mp4 case-insensitively; keep it tidy
    os.replace(_receive(TRAILCAM_DIR, filename), os.path.join(TRAILCAM_DIR, filename))
    size = os.path.getsize(os.path.join(TRAILCAM_DIR, filename))
    print(f"saved trailcam/{filename} ({size / 1024:.0f} KB)", flush=True)
    return "OK", 201


@app.route("/upload", methods=["POST"])
def upload():
    if request.headers.get("X-Upload-Kind") == "trailcam":
        return _save_trailcam(request.headers.get("X-Filename", ""))

    raw = request.headers.get("X-Filename", "")
    filename = os.path.basename(raw)
    if not _SAFE_NAME.match(filename):
        filename = datetime.datetime.utcnow().strftime("%Y%m%d_%H%M%S") + ".bin"

    if filename.endswith(".avi"):
        return _save_clip(filename)

    filename, f = _open_unique(filename)
    with f:
        f.write(request.get_data())

    size = os.path.getsize(os.path.join(UPLOAD_DIR, filename))
    print(f"saved {filename} ({size / 1024:.0f} KB)", flush=True)
    return "OK", 201


if __name__ == "__main__":
    os.makedirs(UPLOAD_DIR, exist_ok=True)
    os.makedirs(TRAILCAM_DIR, exist_ok=True)
    os.makedirs(MOTION_DIR, exist_ok=True)
    app.run(host="0.0.0.0", port=8080)
