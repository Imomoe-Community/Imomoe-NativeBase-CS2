#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Executable regressions for the native gamedata/source contract."""

import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/validate-native-gamedata.py"
SPEC = importlib.util.spec_from_file_location("native_gamedata", SCRIPT)
VALIDATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VALIDATOR)


class NativeGamedataTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source_dir = self.root / "src"
        self.source_dir.mkdir()
        self.data_path = self.root / "gamedata.jsonc"
        self.source = '''
constexpr auto kBoth = SignatureKey{"Both"};
constexpr auto kOffset = OffsetKey{"Slot"};
constexpr auto kTable = VTableKey{"Class"};
void run() {
    ProbeSignature(kBoth);
    ProbeOffset(kOffset);
    ProbeVTable(kTable);
    ProbeSignature(SignatureKey{"Inline"});
#if defined(_WIN32)
    constexpr auto kPlatform = SignatureKey{"WindowsOnly"};
#else
    constexpr auto kPlatform = SignatureKey{"LinuxOnly"};
#endif
    ProbeSignature(kPlatform);
}
'''
        self.data = {"csgo": {
            "Signatures": {
                "Both": {"library": "server", "win64": "48 ? 8B ??", "linuxsteamrt64": "55 FF"},
                "Inline": {"library": "server", "win64": "AB", "linuxsteamrt64": "cd"},
                "WindowsOnly": {"library": "server", "win64": "01"},
                "LinuxOnly": {"library": "server", "linuxsteamrt64": "02"},
            },
            "Offsets": {"Slot": {"win64": 0, "linuxsteamrt64": 12}},
            "VTables": {"Class": {"library": "server", "table": "Class"}},
        }}

    def run_check(self, data=None, source=None):
        self.data_path.write_text(json.dumps(self.data if data is None else data), encoding="utf-8")
        (self.source_dir / "sample.cpp").write_text(self.source if source is None else source, encoding="utf-8")
        return VALIDATOR.validate(self.data_path, self.source_dir)

    def test_dual_platform_and_intentional_platform_specific_keys(self):
        self.assertEqual(self.run_check(), [])
        changed = copy.deepcopy(self.data)
        del changed["csgo"]["Signatures"]["Both"]["linuxsteamrt64"]
        self.assertIn("Signatures:Both missing linuxsteamrt64", self.run_check(changed))
        changed = copy.deepcopy(self.data)
        del changed["csgo"]["Signatures"]["WindowsOnly"]["win64"]
        self.assertIn("Signatures:WindowsOnly missing win64", self.run_check(changed))

    def test_signature_formats_and_offset_types(self):
        for invalid in ("", "4", "GG", "4?", "48 ZZ", "48\t8B", "48\n8B", "? ??", 123):
            with self.subTest(signature=invalid):
                changed = copy.deepcopy(self.data)
                changed["csgo"]["Signatures"]["Both"]["win64"] = invalid
                self.assertIn("Signatures:Both win64 is not a byte/wildcard pattern", self.run_check(changed))
        for invalid in (-1, 0.5, True, "0", 2**32):
            with self.subTest(offset=invalid):
                changed = copy.deepcopy(self.data)
                changed["csgo"]["Offsets"]["Slot"]["linuxsteamrt64"] = invalid
                self.assertIn("Offsets:Slot linuxsteamrt64 must be a nonnegative int32", self.run_check(changed))

    def test_undefined_unused_and_wrong_category_are_reported(self):
        source = self.source.replace('SignatureKey{"Inline"}', 'SignatureKey{"Inlien"}')
        problems = self.run_check(source=source)
        self.assertIn("undefined Signatures:Inlien", problems)
        self.assertIn("unused Signatures:Inline", problems)
        source = self.source.replace('OffsetKey{"Slot"}', 'SignatureKey{"Slot"}')
        problems = self.run_check(source=source)
        self.assertIn("undefined Signatures:Slot", problems)
        self.assertIn("unused Offsets:Slot", problems)

    def test_type_checked_arrays_structs_and_header_keys(self):
        source = self.source.replace('constexpr auto kBoth = SignatureKey{"Both"};', '''
constexpr std::array keys{SignatureKey{"Both"}};
struct Input { SignatureKey signature; };
constexpr Input input{keys[0]};
constexpr auto kBoth = input.signature;
''').replace('constexpr auto kTable = VTableKey{"Class"};', '')
        (self.source_dir / "policy.hpp").write_text('constexpr auto kTable = VTableKey{"Class"};', encoding="utf-8")
        self.assertEqual(self.run_check(source=source), [])
        source = source.replace('SignatureKey{"Both"}', 'SignatureKey{"Btoh"}')
        self.assertIn("undefined Signatures:Btoh", self.run_check(source=source))
        source = self.source.replace('constexpr auto kBoth = SignatureKey{"Both"};',
                                     'constexpr SignatureKey kBoth{"Both"};')
        self.assertEqual(self.run_check(source=source), [])
        source = self.source.replace('SignatureKey{"Both"}', 'SignatureKey("Both")')
        self.assertEqual(self.run_check(source=source), [])

    def test_untyped_sdk_calls_and_nonliteral_typed_keys_are_rejected(self):
        source = self.source + '\nvoid bad() { s2sdk::GetGameConfigSignature(config, "Both"); }\n'
        with self.assertRaisesRegex(VALIDATOR.ValidationError, "use the typed Probe API"):
            self.run_check(source=source)
        source = self.source + '\nvoid bad() { GetGameConfigOffset(config, "Slot"); }\n'
        with self.assertRaisesRegex(VALIDATOR.ValidationError, "use the typed Probe API"):
            self.run_check(source=source)
        source = self.source + '\nvoid bad() { __s2sdk_GetGameConfigVTable(config, "Class"); }\n'
        with self.assertRaisesRegex(VALIDATOR.ValidationError, "use the typed Probe API"):
            self.run_check(source=source)
        for expression in ('runtime_name', '"Bo" "th"'):
            with self.subTest(expression=expression), self.assertRaisesRegex(
                VALIDATOR.ValidationError, "SignatureKey requires a single literal"
            ):
                self.run_check(source=self.source.replace('SignatureKey{"Both"}', f'SignatureKey{{{expression}}}'))

    def test_vtable_structure_unknown_fields_and_comments(self):
        changed = copy.deepcopy(self.data)
        del changed["csgo"]["VTables"]["Class"]["table"]
        changed["csgo"]["Offsets"]["Slot"]["windows"] = 3
        problems = self.run_check(changed)
        self.assertIn("VTables:Class missing table", problems)
        self.assertIn("Offsets:Slot has unknown field windows", problems)
        self.assertEqual(VALIDATOR.parse_jsonc('{"url":"https://example/a//b", /* hi */ "a": [1,],}'),
                         {"url": "https://example/a//b", "a": [1]})
        with self.assertRaisesRegex(VALIDATOR.ValidationError, "duplicate JSON key"):
            VALIDATOR.parse_jsonc('{"a": 1, "a": 2}')
        source = self.source + '\n// ProbeOffset("bogus")\n' + \
            'const char* note = "ProbeOffset(\\\"bogus\\\")";\n'
        self.assertEqual(self.run_check(source=source), [])

    def test_cli_reports_failure_and_success(self):
        self.assertEqual(self.run_check(), [])
        command = [sys.executable, "-B", str(SCRIPT), "--gamedata", str(self.data_path),
                   "--source-dir", str(self.source_dir)]
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        (self.source_dir / "sample.cpp").write_text(self.source.replace('"Inline"', '"Missing"'), encoding="utf-8")
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("undefined Signatures:Missing", result.stderr)

    def test_real_native_contract(self):
        self.assertEqual(VALIDATOR.validate(VALIDATOR.DEFAULT_DATA, VALIDATOR.DEFAULT_SOURCE), [])


if __name__ == "__main__":
    unittest.main()
