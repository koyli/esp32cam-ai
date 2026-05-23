#!/usr/bin/env python3
"""Delete oldest files in UPLOAD_DIR until total size is under MAX_GB.
Run from cron, e.g.:  0 * * * * python3 /home/ubuntu/server/cleanup.py
"""

import os

UPLOAD_DIR = "/home/ubuntu/recordings"
MAX_BYTES  = 8 * 1024 ** 3  # 8 GB — adjust to taste


def main():
    entries = []
    total = 0
    for name in os.listdir(UPLOAD_DIR):
        path = os.path.join(UPLOAD_DIR, name)
        if not os.path.isfile(path):
            continue
        size = os.path.getsize(path)
        entries.append((os.path.getmtime(path), size, path))
        total += size

    if total <= MAX_BYTES:
        return

    entries.sort()  # oldest first
    for _mtime, size, path in entries:
        if total <= MAX_BYTES:
            break
        os.remove(path)
        print(f"deleted {os.path.basename(path)} ({size / 1024 / 1024:.1f} MB)")
        total -= size


if __name__ == "__main__":
    main()
