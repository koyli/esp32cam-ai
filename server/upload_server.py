#!/usr/bin/env python3
"""Receives file uploads from the ESP32 and saves them to UPLOAD_DIR."""

import os
import re
import datetime
from flask import Flask, request

app = Flask(__name__)

UPLOAD_DIR = os.environ.get("UPLOAD_DIR", "/home/ubuntu/recordings")
_SAFE_NAME = re.compile(r'^[\w\.\-]+\.(avi|txt)$')


@app.route("/upload", methods=["POST"])
def upload():
    raw = request.headers.get("X-Filename", "")
    filename = os.path.basename(raw)
    if not _SAFE_NAME.match(filename):
        filename = datetime.datetime.utcnow().strftime("%Y%m%d_%H%M%S") + ".bin"

    path = os.path.join(UPLOAD_DIR, filename)
    with open(path, "wb") as f:
        f.write(request.get_data())

    size = os.path.getsize(path)
    print(f"saved {filename} ({size / 1024:.0f} KB)", flush=True)
    return "OK", 201


if __name__ == "__main__":
    os.makedirs(UPLOAD_DIR, exist_ok=True)
    app.run(host="0.0.0.0", port=8080)
