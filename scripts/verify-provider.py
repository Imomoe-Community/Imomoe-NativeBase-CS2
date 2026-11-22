#!/usr/bin/env python3
# Copyright (C) 2026 Mai Ooizumi
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Check every consumed method/prototype signature against the pinned native SDK."""

import argparse
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def signature(method):
    fields = {"type", "ref", "prototype", "enum"}
    return (
        [{key: value for key, value in item.items() if key in fields} for item in method.get("paramTypes", [])],
        {key: value for key, value in method["retType"].items() if key in fields},
    )


def verify(provider_path, source):
    provider = json.loads(provider_path.read_text(encoding="utf-8"))
    native = json.loads((source / "plugify-plugin-s2sdk.pplugin.in").read_text(encoding="utf-8"))
    counts = {}
    for group in ("methods", "prototypes"):
        actual = {method["name"]: method for method in native[group]}
        for method in provider[group]:
            name = method["name"]
            if name not in actual:
                raise ValueError(f"Missing native {group}: {name}")
            if signature(method) != signature(actual[name]):
                raise ValueError(f"Native ABI mismatch: {name}")
        counts[group] = len(provider[group])
    version = (source / "version.txt").read_text(encoding="utf-8").strip()
    if provider["version"] != version:
        raise ValueError(f"Provider version {provider['version']} does not match SDK {version}")
    return {"sdk_version": version, **counts, "abi": "matched"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-source", required=True, type=Path)
    parser.add_argument("--provider", type=Path, default=ROOT / "bindings/s2sdk.pplugin")
    args = parser.parse_args()
    print(json.dumps(verify(args.provider, args.sdk_source)))


if __name__ == "__main__":
    main()
