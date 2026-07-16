#!/usr/bin/env python3
"""Regression tests for the pinned Ghostty parity inventory."""

from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
GHOSTTY_SOURCE = Path(
    os.environ.get("GHOSTTY_QT_TEST_GHOSTTY_SOURCE_DIR", ROOT / "ghostty")
)
CHECKER = ROOT / "scripts" / "check-ghostty-parity.py"
sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location("ghostty_parity_check", CHECKER)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load parity checker from {CHECKER}")
PARITY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PARITY)


class ParityManifestTest(unittest.TestCase):
    def test_manifest_matches_pinned_source(self) -> None:
        summaries = PARITY.check_repository(ROOT, source_path=GHOSTTY_SOURCE)
        self.assertEqual(205, sum(summaries["config_keys"].values()))
        self.assertEqual(85, sum(summaries["keybinding_actions"].values()))
        self.assertEqual(18, sum(summaries["cli_actions"].values()))

    def test_drift_message_is_deterministic(self) -> None:
        self.assertEqual(
            "added upstream: beta; removed upstream: omega",
            PARITY._inventory_diff(["alpha", "omega"], ["alpha", "beta"]),
        )


if __name__ == "__main__":
    unittest.main()
