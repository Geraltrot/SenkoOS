#!/usr/bin/env python3
"""Writes the update manifest of one board for the auto-updater (diy/firmware/Updater.cpp).

    make_manifest.py --tag v1.4.0-mc1.18.0-20261120 --bin firmware.bin \
        --board heltec_v3 --channel release --url-base https://github.com/OWNER/REPO/releases/download \
        --out manifest-heltec_v3.json

One release carries the firmware of every board: <tag>-<board>.bin (OTA) and <tag>-<board>-merged.bin
(USB). The board downloads <url-base>/<tag>/<tag>-<board>.bin. Manifest names: SenkoPhone (e22wroom)
keeps manifest.json, which boards up to v1.3.x read; other boards use manifest-<board>.json.

The file is compact JSON without a trailing newline: the board verifies the Ed25519
signature over the exact bytes it downloads (the .sig next to it is made by CI with openssl).
"""
import argparse
import hashlib
import json
import re

# image chip id (esp_image_header_t bytes 12..13) per board, the same check as on the board
CHIP_ID = {"e22wroom": 0, "heltec_v3": 9, "heltec_v4": 9}

ap = argparse.ArgumentParser()
ap.add_argument("--tag", required=True)
ap.add_argument("--bin", required=True, help="application image of this board (<tag>-<board>.bin)")
ap.add_argument("--board", required=True, choices=sorted(CHIP_ID), help="technical board id (DIY_BOARD_ID)")
ap.add_argument("--channel", required=True, choices=["release", "beta"])
ap.add_argument("--url-base", required=True, help="download base; the file URL is <base>/<tag>/<tag>-<board>.bin")
ap.add_argument("--out", required=True)
a = ap.parse_args()

if not re.match(r"^v\d+\.\d+\.\d+-", a.tag):
    raise SystemExit(f"tag {a.tag} is not v<major>.<minor>.<patch>-...")
data = open(a.bin, "rb").read()
if len(data) < 16 or data[0] != 0xE9:
    raise SystemExit(f"{a.bin} is not an ESP32 application image")
chip = data[12] | (data[13] << 8)
if chip != CHIP_ID[a.board]:
    raise SystemExit(f"{a.bin} is for chip id {chip}, board {a.board} needs {CHIP_ID[a.board]}")

manifest = {
    "version": a.tag,
    "board": a.board,
    "channel": a.channel,
    "url": f"{a.url_base.rstrip('/')}/{a.tag}/{a.tag}-{a.board}.bin",
    "size": len(data),
    "sha256": hashlib.sha256(data).hexdigest(),
}
with open(a.out, "w", encoding="ascii", newline="") as f:
    f.write(json.dumps(manifest, separators=(",", ":")))
print(f"manifest: {a.out} {manifest['version']} {a.board} {manifest['size']} bytes sha256 {manifest['sha256']}")
