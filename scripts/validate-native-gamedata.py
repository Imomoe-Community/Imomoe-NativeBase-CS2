#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Check native gamedata and its typed C++ key references."""

import argparse
import json
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DATA = ROOT / "gamedata/imomoe-native-base.jsonc"
DEFAULT_SOURCE = ROOT / "src"
PLATFORMS = ("win64", "linuxsteamrt64")
CATEGORIES = {"Signature": "Signatures", "Offset": "Offsets", "VTable": "VTables"}
QUOTED = r'''"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*' '''.strip()
TOKEN = re.compile(QUOTED + r"|//[^\n]*|/\*[\s\S]*?\*/")
LOOKUP = re.compile(r"\b(Signature|Offset|VTable)Key\s*(?:([({])|[A-Za-z_]\w*\s*([({]))")
SDK_LOOKUP = re.compile(r"\b(?:(?:s2sdk::)?GetGameConfig|__s2sdk_GetGameConfig)[A-Z]\w*\s*\(")
LITERAL = re.compile(r'"([^"\\]*)"')
BYTE_PATTERN = re.compile(r" *[0-9A-Fa-f]{2}(?: +(?:[0-9A-Fa-f]{2}|\?\??))* *")


class ValidationError(ValueError):
    pass


def without_comments(source):
    """Remove comments without interpreting // in quoted strings as a comment."""
    return TOKEN.sub(
        lambda match: (re.sub(r"[^\n]", " ", match.group())
                       if match.group().startswith(("//", "/*")) else match.group()),
        source,
    )


def mask_strings(source):
    return TOKEN.sub(lambda match: re.sub(r"[^\n]", " ", match.group()), source)


def parse_jsonc(source):
    source = without_comments(source)
    source = re.sub(
        r'"(?:\\.|[^"\\])*"|,(?=\s*[}\]])',
        lambda match: "" if match.group() == "," else match.group(),
        source,
    )

    def unique_pairs(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValidationError(f"duplicate JSON key: {key}")
            result[key] = value
        return result

    try:
        return json.loads(source, object_pairs_hook=unique_pairs)
    except json.JSONDecodeError as exc:
        raise ValidationError(f"invalid JSONC: {exc}") from exc


def platform_source(source, platform):
    """Select platform branches; unknown feature guards retain both arms."""
    lines = []
    stack = []  # (parent possible, remaining branch possible)
    enabled = True
    directive = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$")

    def condition(kind, expression):
        expression = expression.strip()
        if kind in ("ifdef", "ifndef"):
            expression = f"{'!' if kind == 'ifndef' else ''}defined({expression})"
        match = re.fullmatch(r"(!?)\s*defined\s*\(\s*(_WIN32|__linux__)\s*\)", expression)
        if not match:
            return True, True
        value = (match.group(2) == "_WIN32") == (platform == "win64")
        if match.group(1):
            value = not value
        return value, not value

    for line in source.splitlines(keepends=True):
        match = directive.match(line)
        if not match:
            lines.append(line if enabled else "\n" if line.endswith("\n") else "")
            continue
        kind, expression = match.groups()
        if kind in ("if", "ifdef", "ifndef"):
            yes, no = condition(kind, expression)
            stack.append((enabled, no))
            enabled = enabled and yes
        elif kind == "else":
            if not stack:
                raise ValidationError("unmatched #else")
            parent, remaining = stack[-1]
            enabled = parent and remaining
            stack[-1] = (parent, False)
        elif kind == "elif":
            if not stack:
                raise ValidationError("unmatched #elif")
            parent, remaining = stack[-1]
            yes, no = condition("if", expression)
            enabled = parent and remaining and yes
            stack[-1] = (parent, remaining and no)
        else:
            if not stack:
                raise ValidationError("unmatched #endif")
            enabled, _ = stack.pop()
        lines.append("\n" if line.endswith("\n") else "")
    if stack:
        raise ValidationError("unterminated #if")
    return "".join(lines)


def extract_keys(path, platform):
    source = platform_source(without_comments(path.read_text(encoding="utf-8")), platform)
    masked = mask_strings(source)
    if path.name != "engine.cpp":
        match = SDK_LOOKUP.search(masked)
        if match:
            line = source.count("\n", 0, match.start()) + 1
            raise ValidationError(f"{path.name}:{line}: use the typed Probe API, not a direct SDK lookup")

    found = []
    for match in LOOKUP.finditer(masked):
        kind = match.group(1)
        offset = match.end()
        while offset < len(source) and source[offset].isspace():
            offset += 1
        literal = LITERAL.match(source, offset)
        closing = "}" if (match.group(2) or match.group(3)) == "{" else ")"
        if not literal or not source[literal.end():].lstrip().startswith(closing):
            line = source.count("\n", 0, match.start()) + 1
            raise ValidationError(f"{path.name}:{line}: {kind}Key requires a single literal")
        found.append((CATEGORIES[kind], literal.group(1)))
    return found


def validate(data_path, source_dir):
    data = parse_jsonc(data_path.read_text(encoding="utf-8"))
    try:
        game = data["csgo"]
        definitions = {name: game[name] for name in CATEGORIES.values()}
    except (TypeError, KeyError) as exc:
        raise ValidationError("gamedata requires csgo Signatures, Offsets and VTables objects") from exc
    if not all(isinstance(section, dict) for section in definitions.values()):
        raise ValidationError("Signatures, Offsets and VTables must be objects")

    references = {category: {} for category in definitions}
    sources = sorted(source_dir.glob("*.cpp")) + sorted(source_dir.glob("*.hpp"))
    if not sources:
        raise ValidationError(f"no native sources in {source_dir}")
    for platform in PLATFORMS:
        for path in sources:
            for category, key in extract_keys(path, platform):
                references[category].setdefault(key, set()).add(platform)

    problems = []
    for category, entries in definitions.items():
        for key, entry in entries.items():
            if not isinstance(entry, dict):
                problems.append(f"{category}:{key} must be an object")
                continue
            allowed = ({"library", "table"} if category == "VTables" else
                       set(PLATFORMS) | ({"library"} if category == "Signatures" else set()))
            for field in entry.keys() - allowed:
                problems.append(f"{category}:{key} has unknown field {field}")
            if key not in references[category]:
                problems.append(f"unused {category}:{key}")
            if category in ("Signatures", "VTables") and (
                not isinstance(entry.get("library"), str) or not entry["library"]
            ):
                problems.append(f"{category}:{key} missing library")
            if category == "VTables":
                if not isinstance(entry.get("table"), str) or not entry["table"]:
                    problems.append(f"VTables:{key} missing table")
                continue
            for platform in references[category].get(key, set()):
                if platform not in entry:
                    problems.append(f"{category}:{key} missing {platform}")
            for platform in PLATFORMS:
                if platform not in entry:
                    continue
                value = entry[platform]
                if category == "Offsets":
                    if type(value) is not int or not 0 <= value <= 2**31 - 1:
                        problems.append(f"Offsets:{key} {platform} must be a nonnegative int32")
                elif not isinstance(value, str) or not BYTE_PATTERN.fullmatch(value):
                    problems.append(f"Signatures:{key} {platform} is not a byte/wildcard pattern")
        for key in references[category]:
            if key not in entries:
                problems.append(f"undefined {category}:{key}")
    return sorted(problems)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gamedata", type=Path, default=DEFAULT_DATA)
    parser.add_argument("--source-dir", type=Path, default=DEFAULT_SOURCE)
    args = parser.parse_args(argv)
    try:
        problems = validate(args.gamedata, args.source_dir)
    except (OSError, ValidationError) as exc:
        print(f"native gamedata: {exc}", file=sys.stderr)
        return 1
    if problems:
        print("\n".join(problems), file=sys.stderr)
        return 1
    print("Native gamedata syntax and platform-specific source references are valid.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
