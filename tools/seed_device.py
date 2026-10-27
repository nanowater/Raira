#!/usr/bin/env python3
"""Prepare a throw-away HOME for `lvglsim -S` in which a source is already paired, so a fake source can authenticate without
the pairing dance:

  python3 -I tools/seed_device.py /tmp/dev-home            # source "fake" with secret "s3cr3t"
  HOME=/tmp/dev-home build/bin/lvglsim -S 18399

The simulator keeps its settings in $HOME/.config/raira/settings.json (mode 0600).
"""
import argparse
import json
import os
import sys


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("home")
    ap.add_argument("--source-id", default="fake")
    ap.add_argument("--secret", default="s3cr3t")
    ap.add_argument("--device-id", default="testdevice")
    ap.add_argument("--device-name", default="Test device")
    a = ap.parse_args()
    d = os.path.join(a.home, ".config", "raira")
    os.makedirs(d, exist_ok=True)
    path = os.path.join(d, "settings.json")
    sources = [{"id": a.source_id, "name": "Fake PC", "secret": a.secret}]
    with open(os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "w") as f:
        json.dump({"hub_device_id": a.device_id, "hub_device_name": a.device_name, "hub_sources": json.dumps(sources)}, f)
    print(path)


if __name__ == "__main__":
    sys.exit(main())
