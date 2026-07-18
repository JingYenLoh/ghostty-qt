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


def _require_exact_keys(
    value: Any, label: str, expected: set[str]
) -> dict[str, Any]:
    result = _require_dict(value, label)
    missing = sorted(expected - set(result))
    unsupported = sorted(set(result) - expected)
    problems: list[str] = []
    if missing:
        problems.append("missing: " + ", ".join(missing))
    if unsupported:
        problems.append("unsupported: " + ", ".join(unsupported))
    if problems:
        raise ParityError(f"{label} fields are invalid ({'; '.join(problems)})")
    return result


def _require_string_list(value: Any, label: str) -> list[str]:
    if (
        not isinstance(value, list)
        or any(not isinstance(item, str) or not item for item in value)
    ):
        raise ParityError(f"{label} must be an array of non-empty strings")
    return value


def _require_string_map(value: Any, label: str) -> dict[str, str]:
    result = _require_dict(value, label)
    if not result or any(
        not isinstance(item, str) or not item for item in result.values()
    ):
        raise ParityError(f"{label} must map names to non-empty strings")
    return result


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


def _cmake_revision_file(path: Path) -> Path:
    text = _read(path)
    match = re.search(
        r"set\s*\(\s*GHOSTTY_QT_GHOSTTY_REVISION_FILE\s+"
        r'"\$\{CMAKE_CURRENT_SOURCE_DIR\}/([^"]+)"\s*\)',
        text,
        re.MULTILINE,
    )
    if not match:
        raise ParityError(
            f"cannot find GHOSTTY_QT_GHOSTTY_REVISION_FILE in {path}"
        )
    return Path(match.group(1))


def _repository_path(root: Path, relative: str, label: str) -> Path:
    root = root.resolve()
    relative_path = Path(relative)
    if relative_path.is_absolute():
        raise ParityError(f"{label} must be relative to the repository root")
    path = (root / relative_path).resolve()
    try:
        path.relative_to(root)
    except ValueError as error:
        raise ParityError(
            f"{label} must stay within the repository root"
        ) from error
    return path


def _pinned_revision(root: Path, relative: str) -> str:
    path = _repository_path(root, relative, "upstream.revision_file")
    text = _read(path)
    revision = text.strip()
    if (
        not re.fullmatch(r"[0-9a-f]{40}", revision)
        or text not in {revision, revision + "\n"}
    ):
        raise ParityError(
            f"{path} must contain one full lowercase Git hash"
        )
    return revision


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
    manifest = _require_exact_keys(
        _load_manifest(path),
        "manifest",
        {
            "schema_version",
            "upstream",
            "scope",
            "status_definitions",
            "scope_definitions",
            "inventories",
        },
    )

    if manifest.get("schema_version") != 2:
        raise ParityError("schema_version must be 2")

    upstream = _require_exact_keys(
        manifest.get("upstream"),
        "upstream",
        {"project", "revision_file", "source_directory"},
    )
    if _require_string(upstream.get("project"), "upstream.project") != "ghostty":
        raise ParityError("upstream.project must be 'ghostty'")
    revision_file = _require_string(
        upstream.get("revision_file"), "upstream.revision_file"
    )
    expected_revision = _pinned_revision(root, revision_file)

    scope = _require_exact_keys(
        manifest.get("scope"),
        "scope",
        {
            "host_os",
            "display_servers",
            "toolkit",
            "shared_config",
            "toolkit_mapping_policy",
            "excluded_platforms",
        },
    )
    _require_string_list(scope.get("host_os"), "scope.host_os")
    _require_string_list(scope.get("display_servers"), "scope.display_servers")
    _require_string(scope.get("toolkit"), "scope.toolkit")
    _require_string(
        scope.get("toolkit_mapping_policy"), "scope.toolkit_mapping_policy"
    )
    _require_string_list(
        scope.get("excluded_platforms"), "scope.excluded_platforms"
    )
    shared_config = _require_exact_keys(
        scope.get("shared_config"),
        "scope.shared_config",
        {"enabled", "format", "policy"},
    )
    if not isinstance(shared_config.get("enabled"), bool):
        raise ParityError("scope.shared_config.enabled must be a boolean")
    _require_string(shared_config.get("format"), "scope.shared_config.format")
    _require_string(shared_config.get("policy"), "scope.shared_config.policy")

    source = (
        source_path.resolve()
        if source_path is not None
        else _repository_path(
            root,
            _require_string(
                upstream.get("source_directory"), "upstream.source_directory"
            ),
            "upstream.source_directory",
        )
    )
    actual_revision = _git_revision(source)
    if actual_revision != expected_revision:
        raise ParityError(
            f"Ghostty checkout is {actual_revision}, ledger expects {expected_revision}"
        )
    cmake_revision_file = _cmake_revision_file(root / "CMakeLists.txt")
    if cmake_revision_file != Path(revision_file):
        raise ParityError(
            f"CMake reads {cmake_revision_file}, ledger reads {revision_file}"
        )

    statuses = set(
        _require_string_map(
            manifest.get("status_definitions"), "status_definitions"
        )
    )
    scopes = set(
        _require_string_map(
            manifest.get("scope_definitions"), "scope_definitions"
        )
    )

    inventories = _require_dict(manifest.get("inventories"), "inventories")
    if set(inventories) != set(_EXTRACTORS):
        raise ParityError(
            "inventories must contain exactly: " + ", ".join(sorted(_EXTRACTORS))
        )

    summaries: dict[str, Counter[str]] = {}
    for inventory_name, extractor in _EXTRACTORS.items():
        inventory = _require_exact_keys(
            inventories[inventory_name],
            f"inventories.{inventory_name}",
            {"source", "default", "entries", "overrides"},
        )
        source_file = source / _require_string(
            inventory.get("source"), f"inventories.{inventory_name}.source"
        )
        expected = inventory.get("entries")
        if not isinstance(expected, list) or any(
            not isinstance(item, str) for item in expected
        ):
            raise ParityError(
                f"inventories.{inventory_name}.entries must be a string array"
            )
        if expected != sorted(set(expected)):
            raise ParityError(
                f"inventories.{inventory_name}.entries must be sorted and unique"
            )

        actual = extractor(source_file)
        if len(actual) != len(set(actual)):
            raise ParityError(
                f"extractor found duplicate {inventory_name} in {source_file}"
            )
        if expected != actual:
            raise ParityError(
                f"{inventory_name} drifted from {source_file}: "
                + _inventory_diff(expected, actual)
            )

        default = _require_exact_keys(
            inventory.get("default"),
            f"inventories.{inventory_name}.default",
            {"status", "scope"},
        )
        default_status = _require_string(
            default.get("status"), f"inventories.{inventory_name}.default.status"
        )
        default_scope = _require_string(
            default.get("scope"), f"inventories.{inventory_name}.default.scope"
        )
        if default_status not in statuses:
            raise ParityError(
                f"unknown default status {default_status!r} in {inventory_name}"
            )
        if default_scope not in scopes:
            raise ParityError(
                f"unknown default scope {default_scope!r} in {inventory_name}"
            )

        overrides = _require_dict(
            inventory.get("overrides", {}),
            f"inventories.{inventory_name}.overrides",
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
                raise ParityError(
                    f"unknown status {status!r} for {inventory_name}.{name}"
                )
            if scope not in scopes:
                raise ParityError(
                    f"unknown scope {scope!r} for {inventory_name}.{name}"
                )
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
    revision = _pinned_revision(
        args.root.resolve(), manifest["upstream"]["revision_file"]
    )
    print(f"Ghostty parity inventory matches {revision}")
    for name in sorted(summaries):
        counts = summaries[name]
        detail = ", ".join(f"{status}={counts[status]}" for status in sorted(counts))
        print(f"  {name}: {sum(counts.values())} ({detail})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
