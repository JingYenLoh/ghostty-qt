#!/usr/bin/env python3
"""Regression tests for the pinned Ghostty parity inventory."""

from __future__ import annotations

import importlib.util
import os
import sys
import tempfile
import unittest
from pathlib import Path

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
        self.assertEqual(208, sum(summaries["config_keys"].values()))
        self.assertEqual(88, sum(summaries["keybinding_actions"].values()))
        self.assertEqual(19, sum(summaries["cli_actions"].values()))

    def test_drift_message_is_deterministic(self) -> None:
        self.assertEqual(
            "added upstream: beta; removed upstream: omega",
            PARITY._inventory_diff(["alpha", "omega"], ["alpha", "beta"]),
        )

    def test_revision_file_is_the_shared_pin_source(self) -> None:
        manifest = PARITY._load_manifest(ROOT / "docs" / "ghostty-parity.json")
        revision_file = manifest["upstream"]["revision_file"]
        self.assertEqual(
            Path(revision_file),
            PARITY._cmake_revision_file(ROOT / "CMakeLists.txt"),
        )
        self.assertEqual(
            PARITY._git_revision(GHOSTTY_SOURCE),
            PARITY._pinned_revision(ROOT, revision_file),
        )

    def test_revision_file_rejects_non_hash_content(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "GHOSTTY_REVISION").write_text("not-a-commit\n", encoding="utf-8")
            with self.assertRaisesRegex(
                PARITY.ParityError,
                "must contain one full lowercase Git hash",
            ):
                PARITY._pinned_revision(root, "GHOSTTY_REVISION")

    def test_schema_field_errors_are_deterministic(self) -> None:
        with self.assertRaisesRegex(
            PARITY.ParityError,
            r"sample fields are invalid \(missing: required; unsupported: extra\)",
        ):
            PARITY._require_exact_keys(
                {"present": True, "extra": True},
                "sample",
                {"present", "required"},
            )

    def test_revision_file_cannot_escape_repository(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaisesRegex(
                PARITY.ParityError,
                "must stay within the repository root",
            ):
                PARITY._pinned_revision(root, "../GHOSTTY_REVISION")


if __name__ == "__main__":
    unittest.main()
