#!/usr/bin/env python3
# Copyright (C) 2026 Mai Ooizumi
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Build the manifest-pinned S2SDK and stage its matching delivery inputs."""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from package import package


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "runtime/s2sdk.json"
NATIVE_VERSION = json.loads((ROOT / "imomoe_native_base.pplugin").read_bytes())["version"]
EXPECTED_SDK_TESTS = (
    "convar_lifecycle_tests",
    "s2sdk-event-manager-lifecycle-selfcheck",
    "s2sdk-multi-addon-identity-selfcheck",
)
SDK_TESTS = "|".join(EXPECTED_SDK_TESTS)
SDK_BUILD_TARGETS = (
    "plugify-plugin-s2sdk",
    "convar_lifecycle_tests",
    "s2sdk-event-manager-lifecycle-selfcheck",
    "s2sdk-multi-addon-identity-selfcheck",
)
EXPECTED_NATIVE_TESTS = (
    "funrounds_native_boundary_test",
    "airmove_test",
    "recoil_impulse_policy_test",
    "ghost_visibility_policy_test",
    "ghost_radar_policy_test",
    "bighead_policy_test",
    "detour_slot_test",
    "interaction_owner_test",
    "standard_grenade_policy_test",
)
NATIVE_TESTS = "|".join(EXPECTED_NATIVE_TESTS)
NATIVE_BUILD_TARGETS = ("imomoe-native-base", *EXPECTED_NATIVE_TESTS)
POLYHOOK_VERSION = "2.5.7"
STEAMRT_BASE_IMAGE = (
    "registry.gitlab.steamos.cloud/steamrt/sniper/sdk@"
    "sha256:bb30053d3cd19d35c76ae69b2790dbaa1430448006f8240cb6d7e723f2d1940e"
)
STEAMRT_BUILD_IMAGE = "funrounds-steamrt-gcc14:sniper-20261026-gcc14.2.0"
STEAMRT_DOCKERFILE = ROOT / "scripts/docker/steamrt-gcc14.Dockerfile"
GCC_PACKAGE = "gcc-14-monolithic"
GCC_PACKAGE_VERSION = "1:14.2.0-19~steamrt3.1+bsrt3.1"
GCC_VERSION = "14.2.0"
SOURCESDK_PATH = "external/sourcesdk"


def validate_manifest(manifest):
    expected_fields = {"version", "repository", "commit", "sourcesdk_commit"}
    if set(manifest) != expected_fields:
        raise ValueError(f"SDK manifest must contain exactly: {', '.join(sorted(expected_fields))}")
    version = manifest.get("version")
    commit = manifest.get("commit", "")
    sourcesdk_commit = manifest.get("sourcesdk_commit", "")
    repository = manifest.get("repository", "")
    if not version or not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        raise ValueError("SDK manifest must pin a stable version")
    if not re.fullmatch(r"[0-9a-f]{40}", commit) or not re.fullmatch(r"[0-9a-f]{40}", sourcesdk_commit):
        raise ValueError("SDK manifest must pin the source and SourceSDK commits")
    if repository != "https://github.com/Mai-Ooizumi/plugify-plugin-s2sdk.git":
        raise ValueError("SDK manifest must use the FunRounds S2SDK fork")
    return {
        "version": version,
        "repository": repository,
        "commit": commit,
        "sourcesdk_commit": sourcesdk_commit,
    }


def read_spec(path=MANIFEST):
    if not path.is_file():
        raise FileNotFoundError(f"S2SDK source manifest is missing: {path}")
    return validate_manifest(json.loads(path.read_text(encoding="utf-8")))


def checked(command, cwd=None, capture=False):
    print("+ " + subprocess.list2cmdline([str(item) for item in command]))
    try:
        return subprocess.run(
            [str(item) for item in command],
            cwd=cwd,
            check=True,
            text=True,
            stdout=subprocess.PIPE if capture else None,
            stderr=subprocess.STDOUT if capture else None,
        )
    except subprocess.CalledProcessError as error:
        if capture and error.stdout:
            print(error.stdout, file=sys.stderr, end="" if error.stdout.endswith("\n") else "\n")
        raise


def git_command(*arguments):
    command = ["git", "-c", "core.fsmonitor=false"]
    if sys.platform == "win32":
        # SourceSDK's recursive Protobuf paths can exceed MAX_PATH in worktrees.
        command += ["-c", "core.longpaths=true"]
    return [*command, *arguments]


def capture_version(command):
    return checked(command, capture=True).stdout.splitlines()[0]


def host_name():
    if sys.platform == "win32":
        return "windows-x64"
    if sys.platform.startswith("linux"):
        return "linux-x64"
    raise RuntimeError("Only Windows and Linux x64 builds are supported")


def container_reference(value):
    return value == STEAMRT_BASE_IMAGE


def prepare_steamrt_build_image(base_image):
    checked([
        "docker", "build", "--file", STEAMRT_DOCKERFILE,
        "--build-arg", f"STEAMRT_SDK_IMAGE={base_image}",
        "--tag", STEAMRT_BUILD_IMAGE, STEAMRT_DOCKERFILE.parent,
    ])
    image_id = checked(
        ["docker", "image", "inspect", "--format={{.Id}}", STEAMRT_BUILD_IMAGE],
        capture=True,
    ).stdout.strip()
    if not re.fullmatch(r"sha256:[0-9a-f]{64}", image_id):
        raise RuntimeError(f"Docker returned an invalid Steam Runtime image ID: {image_id}")

    def run_in_image(*command):
        return checked(
            ["docker", "run", "--rm", "--network", "none", STEAMRT_BUILD_IMAGE, *command],
            capture=True,
        ).stdout.strip()

    package_version = run_in_image("dpkg-query", "-W", "-f=${Version}", GCC_PACKAGE)
    c_compiler_version = run_in_image("gcc-14", "-dumpfullversion")
    compiler_version = run_in_image("g++-14", "-dumpfullversion")
    default_compiler = run_in_image(
        "bash", "-lc",
        "test \"$(gcc -dumpfullversion)\" = \"$1\" "
        "&& test \"$(g++ -dumpfullversion)\" = \"$1\" && printf '%s' \"$1\"",
        "bash", GCC_VERSION,
    )
    if (package_version != GCC_PACKAGE_VERSION or c_compiler_version != GCC_VERSION
            or compiler_version != GCC_VERSION or default_compiler != GCC_VERSION):
        raise RuntimeError(
            "Prepared Steam Runtime compiler does not match the pinned GCC environment: "
            f"package={package_version}, gcc-14={c_compiler_version}, "
            f"g++-14={compiler_version}, default={default_compiler}"
        )
    return {
        "base_image": base_image,
        "prepared_image": STEAMRT_BUILD_IMAGE,
        "image_id": image_id,
        "compiler_package": f"{GCC_PACKAGE}={package_version}",
        "c_compiler_version": c_compiler_version,
        "compiler_version": compiler_version,
    }


def container_argument(item):
    value = str(item)
    definition = ""
    if value.startswith("-D") and "=" in value:
        definition, value = value.split("=", 1)
        definition += "="
    path = Path(value)
    if path.is_absolute() and path.resolve().is_relative_to(ROOT):
        return definition + "/workspace/" + path.resolve().relative_to(ROOT).as_posix()
    return str(item)


def checked_build(command, image=None, capture=False):
    if not image:
        return checked(command, capture=capture)
    relative_args = [container_argument(item) for item in command]
    docker = [
        "docker", "run", "--rm", "--network", "none",
        "--user", f"{os.getuid()}:{os.getgid()}",
        "--env", "HOME=/tmp",
        "--volume", f"{ROOT}:/workspace",
        "--workdir", "/workspace",
        image,
        *relative_args,
    ]
    return checked(docker, capture=capture)


def checked_workspace(value):
    work_root = (ROOT / ".work").resolve()
    workspace = (ROOT / value).resolve() if not value.is_absolute() else value.resolve()
    if not workspace.is_relative_to(work_root) or workspace == work_root:
        raise ValueError(f"Build workspace must be a new directory under {work_root}")
    if workspace.exists():
        raise FileExistsError(f"Build workspace already exists; choose a new named path: {workspace}")
    workspace.parent.mkdir(parents=True, exist_ok=True)
    workspace.mkdir()
    return workspace


def submodule_url(source, path):
    paths = checked(
        git_command("-C", source, "config", "--file", ".gitmodules", "--get-regexp", r"^submodule\..*\.path$"),
        capture=True,
    ).stdout.splitlines()
    for line in paths:
        key, _, configured_path = line.partition(" ")
        if configured_path == path:
            name = key.removeprefix("submodule.").removesuffix(".path")
            return checked(
                git_command("-C", source, "config", "--file", ".gitmodules", "--get", f"submodule.{name}.url"),
                capture=True,
            ).stdout.strip()
    raise RuntimeError(f"SDK source does not configure a submodule at {path}")


def verify_sourcesdk_origin(source):
    expected = submodule_url(source, SOURCESDK_PATH)
    actual = checked(
        git_command("-C", source / SOURCESDK_PATH, "remote", "get-url", "origin"), capture=True
    ).stdout.strip()
    if actual != expected:
        raise RuntimeError(f"Unexpected SourceSDK source origin: {actual}; expected {expected}")


def validate_submodule_status(output, sourcesdk_commit):
    sourcesdk_status = None
    for line in output.splitlines():
        state = line[:1]
        fields = line[1:].split()
        if len(fields) < 2:
            raise RuntimeError(f"Unrecognized recursive submodule status: {line}")
        commit, path = fields[:2]
        if state != " ":
            raise RuntimeError("Recursive SDK submodules are not at their pinned gitlink commits")
        if path == SOURCESDK_PATH:
            sourcesdk_status = commit
    if sourcesdk_status != sourcesdk_commit:
        raise RuntimeError(
            f"Recursive SDK SourceSDK submodule does not match its manifest pin: "
            f"{sourcesdk_status}; expected {sourcesdk_commit}"
        )


def checked_resume_source(value):
    source = (ROOT / value).resolve() if not value.is_absolute() else value.resolve()
    if not source.is_dir():
        raise FileNotFoundError(f"Resume source does not exist: {source}")
    return source


def read_cmake_cache(build):
    values = {}
    for line in (build / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        if line.startswith(("#", "//")):
            continue
        key, separator, value = line.partition("=")
        if separator:
            name, _, _ = key.partition(":")
            values[name] = value
    compiler_files = list((build / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake"))
    if compiler_files:
        compiler_config = compiler_files[0].read_text(encoding="utf-8")
        for name in ("ID", "VERSION"):
            match = re.search(rf'^set\(CMAKE_CXX_COMPILER_{name} "([^"]*)"\)', compiler_config, re.MULTILINE)
            if match:
                values[f"CMAKE_CXX_COMPILER_{name}"] = match.group(1)
    return values


def visual_studio_generator(installation_version, capabilities):
    major = installation_version.split(".", 1)[0]
    matches = [
        item["name"]
        for item in capabilities.get("generators", [])
        if item.get("name", "").startswith(f"Visual Studio {major} ")
    ]
    if len(matches) != 1:
        raise RuntimeError(f"CMake has no unique Visual Studio {major} generator: {matches}")
    return matches[0]


def windows_cmake_generator():
    configured = os.environ.get("CMAKE_GENERATOR")
    if configured:
        return configured
    installer = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"))
    vswhere = installer / "Microsoft Visual Studio/Installer/vswhere.exe"
    if not vswhere.is_file():
        raise RuntimeError(f"Cannot locate Visual Studio using vswhere: {vswhere}")
    version = checked([vswhere, "-latest", "-property", "installationVersion"], capture=True).stdout.strip()
    capabilities = json.loads(checked(["cmake", "-E", "capabilities"], capture=True).stdout)
    return visual_studio_generator(version, capabilities)


def verify_source(spec, source):
    origin = checked(git_command("-C", source, "remote", "get-url", "origin"), capture=True).stdout.strip()
    if origin != spec["repository"]:
        raise RuntimeError(f"Unexpected SDK source origin: {origin}")
    head = checked(git_command("-C", source, "rev-parse", "HEAD"), capture=True).stdout.strip()
    if head != spec["commit"]:
        raise RuntimeError(f"Unexpected SDK source commit: {head}; expected {spec['commit']}")
    verify_sourcesdk_origin(source)
    submodules = checked(
        git_command("-C", source, "submodule", "status", "--recursive"), capture=True
    ).stdout
    validate_submodule_status(submodules, spec["sourcesdk_commit"])
    paths = [line[1:].split()[1] for line in submodules.splitlines()]
    for path in sorted(paths, key=lambda item: item.count("/"), reverse=True):
        status = checked(
            git_command(
                "-C", source / path, "status", "--porcelain", "--untracked-files=all", "--ignore-submodules=none"
            ),
            capture=True,
        ).stdout
        if status:
            raise RuntimeError(f"Recursive SDK submodule working tree is not clean at {path}:\n{status.rstrip()}")
    status = checked(
        git_command("-C", source, "status", "--porcelain", "--untracked-files=all", "--ignore-submodules=none"),
        capture=True,
    ).stdout
    if status:
        raise RuntimeError(f"SDK source working tree is not clean:\n{status.rstrip()}")
    checked([sys.executable, ROOT / "scripts/verify-provider.py", "--sdk-source", source])
    return submodules


def clone_source(spec, source):
    checked(git_command("init", str(source)))
    checked(git_command("-C", source, "remote", "add", "origin", spec["repository"]))
    checked(git_command("-C", source, "fetch", "--depth=1", "origin", spec["commit"]))
    checked(git_command("-C", source, "checkout", "--detach", spec["commit"]))
    checked(git_command("-C", source, "submodule", "update", "--init", "--recursive", "--depth", "1"))
    return verify_source(spec, source)


def cmake_platform_arguments():
    if sys.platform == "win32":
        common = [
            "-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded",
            "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>",
        ]
        # Pick the latest installed Visual Studio that this CMake advertises.
        generator = ["-G", windows_cmake_generator(), "-A", "x64"]
    else:
        common = [
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_C_COMPILER=/usr/bin/gcc-14",
            "-DCMAKE_CXX_COMPILER=/usr/bin/g++-14",
        ]
        generator = ["-G", "Ninja"]
    return generator, common


def build_targets(build, targets, image=None):
    build_command = ["cmake", "--build", build, "--target", *targets, "--config", "Release",
                     "--parallel", "1" if sys.platform == "win32" else str(min(4, os.cpu_count() or 1))]
    if sys.platform == "win32":
        build_command.extend(["--", "/nr:false"])
    checked_build(build_command, image=image)


def require_tests_passed(output, expected, suite):
    passed = set(re.findall(
        r"^\s*\d+/\d+\s+Test\s+#\d+:\s+(\S+)\s+.*\bPassed\b",
        output,
        re.MULTILINE,
    ))
    expected = set(expected)
    if passed != expected:
        missing = sorted(expected - passed)
        unexpected = sorted(passed - expected)
        raise RuntimeError(
            f"CTest must run and pass exactly the expected {suite} tests; "
            f"missing={missing}, unexpected={unexpected}"
        )


def run_ctest(build, pattern, expected, suite, image=None):
    command = ["ctest", "--test-dir", build, "--build-config", "Release", "-R", pattern,
               "--output-on-failure"]
    try:
        result = checked_build(command, image=image, capture=True)
    except subprocess.CalledProcessError as error:
        print(error.stdout or "", end="")
        raise
    print(result.stdout, end="")
    require_tests_passed(result.stdout, expected, suite)


def configure(source, build, image=None):
    generator, common = cmake_platform_arguments()
    options = [
        "-DSOURCESDK_GAME_TARGET=cs2",
        "-DSOURCESDK_ENABLE_TESTS=ON",
        "-DS2SDK_BUILD_EVENT_MANAGER_SELFCHECK=ON",
        "-DS2SDK_BUILD_MULTI_ADDON_SELFCHECK=ON",
        "-Dprotobuf_WITH_ZLIB=OFF",
    ]
    checked_build(["cmake", "-S", source, "-B", build, *generator, *common, *options], image=image)
    build_targets(build, SDK_BUILD_TARGETS, image)
    run_ctest(build, SDK_TESTS, EXPECTED_SDK_TESTS, "SDK", image)


def generate_sdk_cpp_imports(generator, output):
    checked([generator, "-manifest", ROOT / "bindings/s2sdk.pplugin", "-output", output,
             "-lang", "cpp", "-overwrite"])
    include = output / "external/plugify/include"
    if not (include / "s2sdk/s2sdk.hpp").is_file():
        raise FileNotFoundError(f"C++ S2SDK imports missing from {include}")
    return include


def cmake_definition(name, path):
    return f"-D{name}={Path(path)}"


def configure_native(source, generated_sdk_include, build, image=None, *, protobuf_library):
    generator, common = cmake_platform_arguments()
    options = [
        "-DFUNROUNDS_NATIVE_BUILD_TESTS=ON",
        "-DFUNROUNDS_NATIVE_LIFECYCLE_PROBE=OFF",
        cmake_definition("FUNROUNDS_PLUGIFY_INCLUDE_DIR", source / "external/plugify/include"),
        cmake_definition("FUNROUNDS_S2SDK_SOURCE_DIR", source),
        cmake_definition("FUNROUNDS_S2SDK_GENERATED_INCLUDE_DIR", generated_sdk_include),
        cmake_definition("FUNROUNDS_PROTOBUF_LIBRARY", protobuf_library),
    ]
    native_source = ROOT
    checked_build(["cmake", "-S", native_source, "-B", build, *generator, *common, *options], image=image)
    build_targets(build, NATIVE_BUILD_TARGETS, image)
    run_ctest(build, NATIVE_TESTS, EXPECTED_NATIVE_TESTS, "native boundary", image)


def validate_native_manifest(manifest, package_version):
    if manifest.get("name") != "imomoe_native_base" or manifest.get("version") != package_version:
        raise ValueError("Native plugin manifest must be funrounds_native at the workspace package version")
    if manifest.get("entry") != "bin/imomoe-native-base":
        raise ValueError("Native plugin manifest has an unexpected entry")
    dependencies = {item.get("name"): item.get("constraints") for item in manifest.get("dependencies", [])}
    expected = {"s2sdk": "3.4.3", "polyhook": f">={POLYHOOK_VERSION}"}
    if not all(dependencies.get(name) == constraint for name, constraint in expected.items()):
        raise ValueError(f"Native plugin dependencies must declare the paired runtime minimums: {expected}")


def stage_sdk_configs(build, output):
    names = ("s2sdk.gamedata.jsonc", "s2sdk.settings.jsonc", "s2sdk.translations.jsonc")
    configs = [(name, find_output(build, name, f"Release/{name}")) for name in names]
    for name, source in configs:
        shutil.copy2(source, output / name)


def stage_native(build, output, target):
    if target == "windows-x64":
        binary_name = "imomoe-native-base.dll"
        binary = find_output(build, "bin/Release/imomoe-native-base.dll", "Release/bin/imomoe-native-base.dll",
                             "Release/imomoe-native-base.dll", "bin/imomoe-native-base.dll")
        magic = b"MZ"
    else:
        binary_name = "libimomoe-native-base.so"
        binary = find_output(build, "bin/libimomoe-native-base.so")
        magic = b"\x7fELF"
    if not binary.read_bytes().startswith(magic):
        raise ValueError(f"Unexpected {target} native binary: {binary}")
    native_package = output / "imomoe_native_base"
    (native_package / "bin").mkdir(parents=True)
    (native_package / "gamedata").mkdir()
    shutil.copy2(binary, native_package / "bin" / binary_name)
    shutil.copy2(find_output(build, "imomoe_native_base.pplugin"), native_package / "imomoe_native_base.pplugin")
    shutil.copy2(ROOT / "LICENSE", native_package / "LICENSE")
    shutil.copy2(find_output(build, "gamedata/imomoe-native-base.jsonc"),
                 native_package / "gamedata/imomoe-native-base.jsonc")
    return native_package, binary_name


def find_output(build, *relative_paths):
    for relative in relative_paths:
        path = build / relative
        if path.is_file():
            return path
    raise FileNotFoundError("Build output missing: " + " or ".join(str(build / path) for path in relative_paths))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generator", required=True, type=Path, help="plugify-gen v1.2.15 executable")
    parser.add_argument("--work-dir", type=Path, help="new workspace under .work/; defaults to .work/nativebase-windows or .work/nativebase-linux")
    parser.add_argument("--dist", type=Path, help="output folder for the two platform install ZIPs")
    parser.add_argument("--container-image", help="exact pinned Valve Steam Runtime Sniper SDK base image for Linux builds")
    parser.add_argument("--resume-source", type=Path, help="reuse a clean pinned SDK checkout (inside this repository on Linux)")
    args = parser.parse_args()

    target = host_name()
    if target == "linux-x64" and not args.container_image:
        parser.error("Linux builds require --container-image pinned by OCI sha256 digest")
    if args.container_image and not container_reference(args.container_image):
        parser.error("--container-image must be an immutable OCI @sha256 digest reference")
    if target == "windows-x64" and args.container_image:
        parser.error("--container-image is only valid for Linux builds")
    generator = args.generator.resolve()
    if not generator.is_file():
        parser.error(f"Missing plugify-gen executable: {generator}")
    generator_version = capture_version([generator, "-version"])
    if not re.fullmatch(r"plugify-generator vv?1\.2\.15", generator_version):
        parser.error(f"Expected plugify-gen v1.2.15, got {generator_version}")
    workspace_name = "windows" if target == "windows-x64" else "linux"
    workspace = checked_workspace(args.work_dir or Path(f".work/nativebase-{workspace_name}"))
    source = workspace / "source"
    build = workspace / "build"
    output = workspace / "output"
    output.mkdir()
    native_build = workspace / "native-build"
    generated_sdk = workspace / "generated-s2sdk"

    spec = read_spec()
    provider = json.loads((ROOT / "bindings/s2sdk.pplugin").read_text(encoding="utf-8"))
    if provider["version"] != spec["version"]:
        raise ValueError(f"Provider version {provider['version']} differs from SDK manifest {spec['version']}")

    resume_source = checked_resume_source(args.resume_source) if args.resume_source else None
    if resume_source and target == "linux-x64" and not resume_source.is_relative_to(ROOT):
        parser.error("Linux SDK source must be inside this checkout for the container mount")
    if resume_source:
        source = resume_source
        submodules = verify_source(spec, source)
    else:
        submodules = clone_source(spec, source)
    build_environment = prepare_steamrt_build_image(args.container_image) if target == "linux-x64" else None

    generated_sdk_include = generate_sdk_cpp_imports(generator, generated_sdk)
    if target == "linux-x64":
        configure(source, build, image=STEAMRT_BUILD_IMAGE)
    else:
        configure(source, build)
    cmake_cache = read_cmake_cache(build)
    if target == "linux-x64" and (
        cmake_cache.get("CMAKE_CXX_COMPILER_ID") != "GNU"
        or cmake_cache.get("CMAKE_CXX_COMPILER_VERSION") != GCC_VERSION
    ):
        raise RuntimeError(
            "Linux SDK must use the pinned Steam Runtime GCC compiler: "
            f"{cmake_cache.get('CMAKE_CXX_COMPILER_ID')} {cmake_cache.get('CMAKE_CXX_COMPILER_VERSION')}"
        )

    if target == "linux-x64":
        configure_native(source, generated_sdk_include, native_build, image=STEAMRT_BUILD_IMAGE,
                         protobuf_library=build / "external/sourcesdk/thirdparty/protobuf/libprotobuf.a")
    else:
        configure_native(source, generated_sdk_include, native_build,
                         protobuf_library=build / "external/sourcesdk/thirdparty/protobuf/Release/libprotobuf.lib")

    if target == "windows-x64":
        sdk_name = "plugify-plugin-s2sdk.dll"
        sdk_binary = find_output(build, "Release/plugify-plugin-s2sdk.dll", "plugify-plugin-s2sdk.dll")
    else:
        sdk_name = "libplugify-plugin-s2sdk.so"
        sdk_binary = find_output(build, sdk_name)
    magic = b"MZ" if target == "windows-x64" else b"\x7fELF"
    if not sdk_binary.read_bytes().startswith(magic):
        raise ValueError(f"Unexpected {target} SDK binary: {sdk_binary}")

    shutil.copy2(sdk_binary, output / sdk_name)
    stage_sdk_configs(build, output)
    shutil.copy2(find_output(build, "plugify-plugin-s2sdk.pplugin", "Release/plugify-plugin-s2sdk.pplugin"),
                 output / "plugify-plugin-s2sdk.pplugin")
    native_package, native_binary_name = stage_native(native_build, output, target)
    native_manifest = json.loads((native_package / "imomoe_native_base.pplugin").read_text(encoding="utf-8"))
    package_version = NATIVE_VERSION
    validate_native_manifest(native_manifest, package_version)
    cmake_generator = cmake_cache.get("CMAKE_GENERATOR", "unknown")
    environment = args.container_image or os.environ.get("FUNROUNDS_SDK_BUILD_ENVIRONMENT", f"native-{target}")
    build_record = {
        "platform": target,
        "sdk_version": spec["version"],
        "repository": spec["repository"],
        "commit": spec["commit"],
        "sourcesdk_commit": spec["sourcesdk_commit"],
        "recursive_submodules": submodules.splitlines(),
        "provider": "plugify-plugin-s2sdk.pplugin",
        "cpp_imports": "bindings/s2sdk.pplugin",
        "provider_version": provider["version"],
        "package_version": package_version,
        "native": {
            "manifest": "imomoe_native_base/imomoe_native_base.pplugin",
            "version": native_manifest["version"],
            "entry": native_manifest["entry"],
            "dependencies": {item["name"]: item["constraints"] for item in native_manifest["dependencies"]},
            "lifecycle_probe": False,
            "boundary_tests": list(EXPECTED_NATIVE_TESTS),
            "artifacts": [
                f"imomoe_native_base/bin/{native_binary_name}",
                "imomoe_native_base/imomoe_native_base.pplugin",
                "imomoe_native_base/LICENSE",
                "imomoe_native_base/gamedata/imomoe-native-base.jsonc",
            ],
        },
        "generator": generator_version,
        "build_environment": build_environment or environment,
        "source_reused": resume_source is not None,
        "cmake": checked_build(
            ["cmake", "--version"], image=STEAMRT_BUILD_IMAGE if target == "linux-x64" else None,
            capture=True,
        ).stdout.splitlines()[0],
        "cmake_generator": cmake_generator,
        "git": capture_version(git_command("--version")),
        "compiler": cmake_cache.get("CMAKE_CXX_COMPILER_ID", cmake_cache.get("CMAKE_CXX_COMPILER", "selected by " + cmake_generator)),
        "compiler_version": cmake_cache.get("CMAKE_CXX_COMPILER_VERSION"),
        "sdk_tests": list(EXPECTED_SDK_TESTS),
        "artifacts": [
            sdk_name,
            "s2sdk.gamedata.jsonc",
            "s2sdk.settings.jsonc",
            "s2sdk.translations.jsonc",
            "plugify-plugin-s2sdk.pplugin",
            *[path.relative_to(output).as_posix() for path in sorted(native_package.rglob("*")) if path.is_file()],
        ],
    }
    (output / "sdk-build.json").write_text(json.dumps(build_record, indent=2) + "\n", encoding="utf-8")
    if args.dist:
        package(output, args.dist, source / "LICENSE")
    print(f"SDK delivery inputs: {output}")
    for path in sorted(output.iterdir()):
        print(f"  {path.name} ({path.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
