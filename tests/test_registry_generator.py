#!/usr/bin/env python3
"""The generator must reject corrupt or incomplete Vanilla state reports."""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("registry", Path(__file__).resolve().parents[1] / "tools/generate_registry.py")
registry = importlib.util.module_from_spec(spec)
spec.loader.exec_module(registry)


class RegistryGenerator(unittest.TestCase):
    def setUp(self):
        self.report = {"minecraft:test": {"properties": {"axis": ["x", "y"]}, "states": [
            {"id": 0, "default": True, "properties": {"axis": "x"}},
            {"id": 1, "properties": {"axis": "y"}}]}}

    def load(self, report):
        with tempfile.NamedTemporaryFile(mode="w", suffix=".json") as f:
            json.dump(report, f)
            f.flush()
            return registry.load_report(f.name)

    def test_complete(self):
        blocks, maximum = self.load(self.report)
        self.assertEqual((len(blocks), maximum), (1, 1))

    def test_invalid(self):
        for mode in ("domain", "missing", "duplicate", "hole", "default", "bool_id", "overflow"):
            report = copy.deepcopy(self.report)
            states = report["minecraft:test"]["states"]
            if mode == "domain": states[1]["properties"]["axis"] = "z"
            if mode == "missing": states.pop()
            if mode == "duplicate": states[1]["properties"]["axis"] = "x"
            if mode == "hole": states[1]["id"] = 2
            if mode == "default": states[1]["default"] = True
            if mode == "bool_id": states[0]["id"] = False
            if mode == "overflow": states[1]["id"] = 32768
            with self.subTest(mode=mode), self.assertRaises(SystemExit):
                self.load(report)


class BlockIDs(unittest.TestCase):
    def test_exact_and_invalid_ids(self):
        spec = importlib.util.spec_from_file_location("block_ids", Path(__file__).resolve().parents[1] / "tools/gen_block_ids.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            blocks = root / "blocks.json"
            registries = root / "registries.json"
            blocks.write_text(json.dumps({"minecraft:a": {}, "minecraft:b": {}}))
            for ids in ((1, 0), (0, 0), (0, 2), (False, 1)):
                registries.write_text(json.dumps({"minecraft:block": {"entries": {
                    "minecraft:a": {"protocol_id": ids[0]}, "minecraft:b": {"protocol_id": ids[1]}}}}))
                args = (blocks, registries, root / "out.c", root / "out.h")
                if ids == (1, 0):
                    module.generate(*args)
                    self.assertIn("1, /* minecraft:a */", (root / "out.c").read_text())
                else:
                    with self.assertRaises(ValueError): module.generate(*args)
            registries.write_text(json.dumps({"minecraft:block": {"entries": {}}}))
            with self.assertRaises(ValueError): module.generate(*args)


if __name__ == "__main__":
    unittest.main()
