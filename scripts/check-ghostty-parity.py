#!/usr/bin/env python3
"""Validate the Ghostty parity ledger against the pinned upstream source.

The extractor intentionally understands only the three small Zig declarations
recorded in docs/ghostty-parity.json. If upstream changes their shape, this
check fails loudly instead of silently producing an incomplete inventory.
"""

from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path
import re
import subprocess
import sys
from typing import Any, Callable


class ParityError(RuntimeError):
    """A deterministic parity-ledger validation failure."""


_CONFIG_FIELD = re.compile(
    r'^(?:@"(?P<quoted>[^"]+)"|(?P<plain>[A-Za-z][A-Za-z0-9_-]*))\s*:'
)
_BINDING_VARIANT = re.compile(
    r'^    (?:@"(?P<quoted>[^"]+)"|(?P<plain>[A-Za-z_][A-Za-z0-9_]*))'
    r'\s*(?::[^,]+)?\s*,(?:\s*//.*)?$'
)
_CLI_VARIANT = re.compile(
    r'^    (?:@"(?P<quoted>[^"]+)"|(?P<plain>[A-Za-z_][A-Za-z0-9_-]*))'
    r'\s*,(?:\s*//.*)?$'
)


def _read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as error:
        raise ParityError(f"cannot read {path}: {error}") from error


def _slice(text: str, start_marker: str, end_marker: str, source: Path) -> str:
    try:
        start = text.index(start_marker) + len(start_marker)
        end = text.index(end_marker, start)
    except ValueError as error:
        raise ParityError(
            f"cannot locate expected declaration markers in {source}; "
            "update the parity extractor intentionally"
        ) from error
    return text[start:end]


def _match_names(text: str, pattern: re.Pattern[str]) -> list[str]:
    names: list[str] = []
    for line in text.splitlines():
        match = pattern.match(line)
        if match:
            names.append(match.group("quoted") or match.group("plain"))
    return names


def extract_config_keys(path: Path) -> list[str]:
    prefix = _slice(_read(path), "", "pub fn deinit", path)
    return sorted(
        name
        for name in _match_names(prefix, _CONFIG_FIELD)
        if not name.startswith("_")
    )


def extract_binding_actions(path: Path) -> list[str]:
    body = _slice(
        _read(path),
        "pub const Action = union(enum) {",
        "    pub const Key =",
        path,
    )
    return sorted(_match_names(body, _BINDING_VARIANT))


def extract_cli_actions(path: Path) -> list[str]:
    body = _slice(
        _read(path),
        "pub const Action = enum {",
        "    pub fn detectSpecialCase",
        path,
    )
    return sorted(_match_names(body, _CLI_VARIANT))


_EXTRACTORS: dict[str, Callable[[Path], list[str]]] = {
    "config_keys": extract_config_keys,
    "keybinding_actions": extract_binding_actions,
    "cli_actions": extract_cli_actions,
}


def _require_dict(value: Any, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ParityError(f"{label} must be a JSON object")
    return value


def _require_string(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise ParityError(f"{label} must be a non-empty string")
    return value


def _inventory_diff(expected: list[str], actual: list[str]) -> str:
    expected_set = set(expected)
    actual_set = set(actual)
    lines: list[str] = []
    added = sorted(actual_set - expected_set)
    removed = sorted(expected_set - actual_set)
    if added:
        lines.append("added upstream: " + ", ".join(added))
    if removed:
        lines.append("removed upstream: " + ", ".join(removed))
    if not lines and expected != actual:
        lines.append("inventory is not sorted deterministically")
    return "; ".join(lines)


def _git_revision(source: Path) -> str:
    try:
        result = subprocess.run(
            ["git", "-C", str(source), "rev-parse", "HEAD"],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        detail = getattr(error, "stderr", "")
        raise ParityError(
            f"cannot determine Ghostty revision in {source}: {detail.strip()}"
        ) from error
    return result.stdout.strip()


def _cmake_revision(path: Path) -> str:
    text = _read(path)
    match = re.search(
        r"set\s*\(\s*GHOSTTY_QT_GHOSTTY_REVISION\s+\"([0-9a-f]{40})\"",
        text,
        re.MULTILINE,
    )
    if not match:
        raise ParityError(f"cannot find GHOSTTY_QT_GHOSTTY_REVISION in {path}")
    return match.group(1)


def _load_manifest(path: Path) -> dict[str, Any]:
    try:
        manifest = json.loads(_read(path))
    except json.JSONDecodeError as error:
        raise ParityError(f"invalid JSON in {path}: {error}") from error
    return _require_dict(manifest, "manifest")


def check_repository(
    root: Path,
    manifest_path: Path | None = None,
    source_path: Path | None = None,
) -> dict[str, Counter[str]]:
    root = root.resolve()
    path = (manifest_path or root / "docs" / "ghostty-parity.json").resolve()
    manifest = _load_manifest(path)

    if manifest.get("schema_version") != 1:
        raise ParityError("schema_version must be 1")

    upstream = _require_dict(manifest.get("upstream"), "upstream")
    expected_revision = _require_string(upstream.get("revision"), "upstream.revision")
    if not re.fullmatch(r"[0-9a-f]{40}", expected_revision):
        raise ParityError("upstream.revision must be a full lowercase Git hash")

    source = (
        source_path.resolve()
        if source_path is not None
        else root
        / _require_string(
            upstream.get("source_directory"), "upstream.source_directory"
        )
    )
    actual_revision = _git_revision(source)
    if actual_revision != expected_revision:
        raise ParityError(
            f"Ghostty checkout is {actual_revision}, ledger expects {expected_revision}"
        )
    cmake_revision = _cmake_revision(root / "CMakeLists.txt")
    if cmake_revision != expected_revision:
        raise ParityError(
            f"CMake pins {cmake_revision}, ledger expects {expected_revision}"
        )

    statuses = set(_require_dict(manifest.get("status_definitions"), "status_definitions"))
    scopes = set(_require_dict(manifest.get("scope_definitions"), "scope_definitions"))
    if not statuses or not scopes:
        raise ParityError("status_definitions and scope_definitions cannot be empty")

    inventories = _require_dict(manifest.get("inventories"), "inventories")
    if set(inventories) != set(_EXTRACTORS):
        raise ParityError(
            "inventories must contain exactly: " + ", ".join(sorted(_EXTRACTORS))
        )

    summaries: dict[str, Counter[str]] = {}
    for inventory_name, extractor in _EXTRACTORS.items():
        inventory = _require_dict(inventories[inventory_name], f"inventories.{inventory_name}")
        source_file = source / _require_string(
            inventory.get("source"), f"inventories.{inventory_name}.source"
        )
        expected = inventory.get("entries")
        if not isinstance(expected, list) or any(not isinstance(item, str) for item in expected):
            raise ParityError(f"inventories.{inventory_name}.entries must be a string array")
        if expected != sorted(set(expected)):
            raise ParityError(
                f"inventories.{inventory_name}.entries must be sorted and unique"
            )

        actual = extractor(source_file)
        if len(actual) != len(set(actual)):
            raise ParityError(f"extractor found duplicate {inventory_name} in {source_file}")
        if expected != actual:
            raise ParityError(
                f"{inventory_name} drifted from {source_file}: "
                + _inventory_diff(expected, actual)
            )

        default = _require_dict(inventory.get("default"), f"inventories.{inventory_name}.default")
        default_status = _require_string(
            default.get("status"), f"inventories.{inventory_name}.default.status"
        )
        default_scope = _require_string(
            default.get("scope"), f"inventories.{inventory_name}.default.scope"
        )
        if default_status not in statuses:
            raise ParityError(f"unknown default status {default_status!r} in {inventory_name}")
        if default_scope not in scopes:
            raise ParityError(f"unknown default scope {default_scope!r} in {inventory_name}")

        overrides = _require_dict(
            inventory.get("overrides", {}), f"inventories.{inventory_name}.overrides"
        )
        unknown = sorted(set(overrides) - set(expected))
        if unknown:
            raise ParityError(
                f"unknown override entries in {inventory_name}: " + ", ".join(unknown)
            )

        counts: Counter[str] = Counter()
        for name in expected:
            override = _require_dict(
                overrides.get(name, {}),
                f"inventories.{inventory_name}.overrides.{name}",
            )
            status = override.get("status", default_status)
            scope = override.get("scope", default_scope)
            if status not in statuses:
                raise ParityError(f"unknown status {status!r} for {inventory_name}.{name}")
            if scope not in scopes:
                raise ParityError(f"unknown scope {scope!r} for {inventory_name}.{name}")
            if set(override) - {"status", "scope", "note"}:
                raise ParityError(
                    f"unsupported override fields for {inventory_name}.{name}: "
                    + ", ".join(sorted(set(override) - {"status", "scope", "note"}))
                )
            counts[str(status)] += 1
        summaries[inventory_name] = counts

    return summaries


def main(argv: list[str] | None = None) -> int:
    default_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=default_root)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument(
        "--source",
        type=Path,
        help="Ghostty checkout to audit instead of the manifest-relative default",
    )
    args = parser.parse_args(argv)

    try:
        summaries = check_repository(args.root, args.manifest, args.source)
    except ParityError as error:
        print(f"parity check failed: {error}", file=sys.stderr)
        return 1

    manifest = _load_manifest(
        args.manifest or args.root / "docs" / "ghostty-parity.json"
    )
    revision = manifest["upstream"]["revision"]
    print(f"Ghostty parity inventory matches {revision}")
    for name in sorted(summaries):
        counts = summaries[name]
        detail = ", ".join(f"{status}={counts[status]}" for status in sorted(counts))
        print(f"  {name}: {sum(counts.values())} ({detail})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
