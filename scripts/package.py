#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Publish two separate install ZIPs from one pinned NativeBase/SDK build."""

import argparse
import json
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED

ROOT = Path(__file__).resolve().parents[1]
ENVS = "game/csgo/addons/plugify/envs"


def package(output, dist, sdk_license):
    build = json.loads((output / "sdk-build.json").read_bytes())
    lock_path = ROOT / "runtime/s2sdk.json"
    lock = json.loads(lock_path.read_bytes())
    if (build["sdk_version"], build["commit"], build["sourcesdk_commit"]) != (
        lock["version"], lock["commit"], lock["sourcesdk_commit"],
    ):
        raise ValueError("Staged build does not match the pinned SDK and SourceSDK")
    platform = build["platform"]
    win = platform == "windows-x64"
    if not win and platform != "linux-x64":
        raise ValueError(f"Unsupported platform: {platform}")
    magic = b"MZ" if win else b"\x7fELF"
    prefix, suffix = ("", ".dll") if win else ("lib", ".so")
    native_root = output / "imomoe_native_base"
    native_manifest = native_root / "imomoe_native_base.pplugin"
    native = json.loads(native_manifest.read_bytes())
    if native != json.loads((ROOT / native_manifest.name).read_bytes()):
        raise ValueError("Staged NativeBase descriptor differs from source")
    sdk_manifest = output / "plugify-plugin-s2sdk.pplugin"
    sdk = json.loads(sdk_manifest.read_bytes())
    if (sdk["name"], sdk["version"], sdk["entry"]) != ("s2sdk", lock["version"], "bin/plugify-plugin-s2sdk"):
        raise ValueError("Expected the generated fork S2SDK runtime descriptor")
    if sdk != json.loads((ROOT / "bindings/s2sdk.pplugin").read_bytes()):
        raise ValueError("Staged S2SDK descriptor differs from the C++ import descriptor")
    if (native_root / "gamedata/imomoe-native-base.jsonc").read_bytes() != (ROOT / "gamedata/imomoe-native-base.jsonc").read_bytes():
        raise ValueError("Staged NativeBase gamedata differs from source")

    native_bin = f"bin/{prefix}imomoe-native-base{suffix}"
    sdk_bin = f"{prefix}plugify-plugin-s2sdk{suffix}"
    native_files = {
        name: native_root / name for name in
        (native_bin, native_manifest.name, "gamedata/imomoe-native-base.jsonc", "LICENSE")
    }
    sdk_files = {
        f"bin/{sdk_bin}": output / sdk_bin,
        **{name: output / name for name in (
            sdk_manifest.name, "s2sdk.gamedata.jsonc", "s2sdk.settings.jsonc", "s2sdk.translations.jsonc",
        )},
        "s2sdk-fork.json": lock_path,
        "LICENSE": sdk_license,
    }
    packages = (
        (dist / f"imomoe-native-base-{native['version']}-{platform}.zip", "imomoe_native_base", native_files),
        (dist / f"s2sdk-fork-{lock['version']}-{lock['commit'][:7]}-{platform}.zip", "plugify-plugin-s2sdk", sdk_files),
    )
    for archive, _, files in packages:
        if archive.exists():
            raise FileExistsError(archive)
        for name, path in files.items():
            if not path.is_file():
                raise FileNotFoundError(path)
            if name.startswith("bin/"):
                with path.open("rb") as stream:
                    if stream.read(len(magic)) != magic:
                        raise ValueError(f"Wrong binary platform: {path}")
    dist.mkdir(parents=True, exist_ok=True)
    for archive, owner, files in packages:
        members = {f"{ENVS}/{owner}/{name}": path for name, path in files.items()}
        with ZipFile(archive, "x", compression=ZIP_DEFLATED) as out:
            for name, path in members.items():
                out.write(path, name)
        with ZipFile(archive) as out:
            if set(out.namelist()) != members.keys() or out.testzip() is not None:
                raise ValueError(f"Invalid ZIP inventory or CRC: {archive}")
        print(archive)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-output", required=True, type=Path)
    parser.add_argument("--dist", required=True, type=Path)
    parser.add_argument("--sdk-license", required=True, type=Path, help="LICENSE from the pinned fork source")
    args = parser.parse_args()
    package(args.build_output, args.dist, args.sdk_license)
