#!/usr/bin/env python3
# Copyright (C) 2026 Mai Ooizumi
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Check that the full C++ import descriptor matches the pinned S2SDK."""

import argparse
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def verify(provider_path, source):
    version = (source / "version.txt").read_text(encoding="utf-8").strip()
    template = (source / "plugify-plugin-s2sdk.pplugin.in").read_text(encoding="utf-8")
    native = json.loads(template.replace("${S2SDK_VERSION}", version).replace("${S2SDK_PACKAGE}", "s2sdk"))
    provider = json.loads(provider_path.read_bytes())
    if provider != native:
        raise ValueError("C++ import descriptor differs from the pinned S2SDK descriptor")
    return {
        "sdk_version": version,
        "methods": len(native["methods"]),
        "prototypes": len(native["prototypes"]),
        "abi": "matched",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-source", required=True, type=Path)
    parser.add_argument("--provider", type=Path, default=ROOT / "bindings/s2sdk.pplugin")
    args = parser.parse_args()
    print(json.dumps(verify(args.provider, args.sdk_source)))


if __name__ == "__main__":
    main()
