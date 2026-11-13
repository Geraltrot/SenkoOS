#!/usr/bin/env python3
"""Writes manifest.json for the auto-updater (diy/firmware/Updater.cpp).

    make_manifest.py --tag v1.4.0-e22wroom-mc1.18.0-20261120 --bin firmware.bin \
        --board e22wroom --channel release --url-base https://github.com/OWNER/REPO/releases/download \
        --out manifest.json

The file is compact JSON without a trailing newline: the board verifies the Ed25519
signature over the exact bytes it downloads (manifest.sig is made by CI with openssl).
"""
import argparse
import hashlib
import json
import os

ap = argparse.ArgumentParser()
ap.add_argument("--tag", required=True)
ap.add_argument("--bin", required=True, help="application image (<tag>.bin)")
ap.add_argument("--board", required=True, help="technical board id, e.g. e22wroom")
ap.add_argument("--channel", required=True, choices=["release", "beta"])
ap.add_argument("--url-base", required=True, help="download base; the file URL is <base>/<tag>/<tag>.bin")
ap.add_argument("--out", default="manifest.json")
a = ap.parse_args()

if f"-{a.board}-" not in a.tag:
    raise SystemExit(f"tag {a.tag} is not for board {a.board}")
data = open(a.bin, "rb").read()
if len(data) < 16 or data[0] != 0xE9:
    raise SystemExit(f"{a.bin} is not an ESP32 application image")

manifest = {
    "version": a.tag,
    "board": a.board,
    "channel": a.channel,
    "url": f"{a.url_base.rstrip('/')}/{a.tag}/{a.tag}.bin",
    "size": len(data),
    "sha256": hashlib.sha256(data).hexdigest(),
}
with open(a.out, "w", encoding="ascii", newline="") as f:
    f.write(json.dumps(manifest, separators=(",", ":")))
print(f"manifest: {a.out} {manifest['version']} {manifest['size']} bytes sha256 {manifest['sha256']}")
