#!/usr/bin/env python3
"""Compare two ghostty-qt renderer qualification reports."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import sys
import tempfile
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from urllib.parse import unquote_to_bytes

COMPARISON_SCHEMA_VERSION = 1
POLICY_VERSION = 1
SUPPORTED_QUALIFICATION_SCHEMA = 3
MEASURED_RUN_KINDS = (
    "pane_offscreen",
    "custom_shader_offscreen",
    "production_swapchain",
)
SUPPORTED_BACKENDS = ("opengl", "vulkan")
PANE_GRIDS = ("120x40", "240x80")
SHADER_RENDERERS = ("legacy", "retained")
SHADER_WORKLOADS = ("source-dirty", "effect-only")
SHADER_PASS_COUNTS = ("0", "1", "2", "4", "8")
CONFIGURATION_FIELDS = (
    "backends",
    "synthetic_global_scales",
    "pane_warmup",
    "pane_iterations",
    "shader_warmup",
    "shader_iterations",
    "allow_software_device",
    "build_performed",
)
DEVICE_FIELDS = (
    "rhi_backend",
    "rhi_device_name",
    "rhi_device_type",
    "rhi_vendor_id",
    "rhi_device_id",
)
PANE_HEADER_FIELDS = (
    "qt_version",
    "benchmark_contract",
    "backend",
    "platform",
    "presentation",
    "warmup",
    "iterations",
    "kitty_placements",
)
PANE_GRID_FIELDS = ("grid", "dpr", "logical", "framebuffer")
SHADER_HEADER_FIELDS = (
    "qt",
    "benchmark_contract",
    "platform",
    "graphics_api",
    "rhi_backend",
    "gpu_timestamps",
    "viewport",
    "framebuffer",
    "dpr",
    "warmup",
    "iterations",
    "measurement_rounds",
    "renderer_order",
    "measured_frames_per_round",
    "completion",
    "validation_readbacks_per_scenario",
    "gpu_scope",
    "gpu_delta_baseline",
    "uniform_snapshots",
    "transforms",
)
PRODUCTION_STRUCTURAL_FIELDS = (
    "benchmark_contract",
    "graphics_library_contract",
    "graphics_library_status",
    "qt_version",
    "platform",
    "graphics_api",
)
PRODUCTION_CONTEXT_FIELDS = (
    "dpr",
    "logical",
    "physical",
    "screen_count",
    "screen_name",
    "screen_manufacturer",
    "screen_model",
    "screen_serial",
    "screen_geometry",
    "screen_available_geometry",
    "screen_physical_mm",
    "screen_depth",
    "screen_dpr",
    "screen_logical_dpi_x",
    "screen_logical_dpi_y",
    "screen_physical_dpi_x",
    "screen_physical_dpi_y",
    "screen_refresh_millihz",
    "screen_orientation",
    "screen_primary",
    "swapchain_format",
    "swapchain_flags",
    "swapchain_samples",
    "swapchain_size",
    "swapchain_srgb",
    "swapchain_premultiplied_alpha",
    "swapchain_nonpremultiplied_alpha",
    "swapchain_no_vsync",
    "swapchain_hdr",
    "swapchain_hdr_limits",
    "swapchain_hdr_behavior",
    "swapchain_hdr_minimum",
    "swapchain_hdr_maximum",
    "swapchain_hdr_maximum_potential",
    "swapchain_hdr_sdr_white",
)
PANE_WORK_FIELDS = (
    "solid_cell_visits",
    "text_row_builds",
    "text_layouts",
    "text_fallback_cells",
    "kitty_texture_uploads",
    "kitty_node_creations",
    "kitty_node_deletions",
    "kitty_geometry_writes",
    "kitty_material_assignments",
    "kitty_texture_set_evictions",
    "kitty_texture_set_count_final",
    "kitty_texture_bytes_final",
)
SHADER_WORK_FIELDS = (
    "source_paints",
    "target_creates",
    "target_destroys",
    "pipeline_creates",
    "binding_creates",
    "source_binding_updates",
    "estimated_offscreen_bytes",
    "estimated_offscreen_targets",
    "internal_texture_bytes",
    "internal_targets",
    "live_bindings",
    "rendered_frames",
    "recorded_draws",
    "resource_generation",
    "uniform_buffer_bytes",
    "uniform_slots",
    "uniform_upload_bytes_per_frame",
)
SHA256_LENGTH = 64
DIMENSION_PATTERN = re.compile(r"^(\d+)x(\d+)$")
RECT_PATTERN = re.compile(r"^-?\d+,-?\d+,(\d+)x(\d+)$")


@dataclass(frozen=True)
class Policy:
    timing_mode: str = "auto"
    pane_cpu_percent: float = 15.0
    pane_gpu_percent: float = 15.0
    pane_cpu_minimum_us: float = 100.0
    pane_gpu_minimum_us: float = 50.0
    shader_cpu_percent: float = 15.0
    shader_gpu_percent: float = 12.0
    shader_cpu_minimum_us: float = 100.0
    shader_gpu_minimum_us: float = 50.0
    shader_cpu_minimum_ratio: float = 0.10
    shader_gpu_minimum_ratio: float = 0.08
    allow_context_changes: bool = False


class ReportError(ValueError):
    pass


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def atomic_write_json(path: Path, value: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
    )
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            json.dump(value, output, indent=2, sort_keys=True)
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary_name, path)
    finally:
        try:
            os.unlink(temporary_name)
        except FileNotFoundError:
            pass


def load_report(path: Path) -> dict[str, object]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ReportError(f"cannot read {path}: {error}") from error
    if not isinstance(value, dict):
        raise ReportError(f"{path} does not contain a JSON object")
    return value


def valid_sha256(value: object) -> bool:
    return (
        isinstance(value, str)
        and len(value) == SHA256_LENGTH
        and all(character in "0123456789abcdef" for character in value)
    )


def graphics_library_aggregate(libraries: list[dict[str, object]]) -> str:
    records = sorted(
        (
            str(library["role"]),
            str(library["name"]),
            int(library["size"]),
            str(library["sha256"]),
        )
        for library in libraries
    )
    payload = b"".join(
        b"\0".join(str(field).encode("utf-8") for field in record) + b"\n"
        for record in records
    )
    return hashlib.sha256(payload).hexdigest()


def is_valid_utf8_scalar_string(value: str) -> bool:
    try:
        value.encode("utf-8")
    except UnicodeEncodeError:
        return False
    return True


def graphics_library_identity(
    record: dict[str, str], backend: str, label: str
) -> dict[str, object]:
    if record.get("graphics_library_contract") != "1":
        raise ReportError(f"{label}.graphics_library_contract: expected 1")
    if record.get("graphics_library_status") != "complete":
        raise ReportError(f"{label}.graphics_library_status: expected complete")
    encoded_manifest = record.get("graphics_library_manifest")
    if not isinstance(encoded_manifest, str) or re.search(
        r"%(?![0-9A-Fa-f]{2})", encoded_manifest
    ):
        raise ReportError(f"{label}.graphics_library_manifest: malformed encoding")
    try:
        manifest_text = unquote_to_bytes(encoded_manifest).decode("utf-8")
        manifest = json.loads(manifest_text)
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ReportError(
            f"{label}.graphics_library_manifest: malformed JSON"
        ) from error
    if (
        not isinstance(manifest, dict)
        or set(manifest)
        != {"schema_version", "backend", "status", "libraries", "diagnostic"}
        or manifest.get("schema_version") != 1
        or manifest.get("backend") != backend
        or manifest.get("status") != record.get("graphics_library_status")
        or not isinstance(manifest.get("libraries"), list)
        or not isinstance(manifest.get("diagnostic"), (str, type(None)))
        or (
            isinstance(manifest.get("diagnostic"), str)
            and not is_valid_utf8_scalar_string(manifest["diagnostic"])
        )
    ):
        raise ReportError(f"{label}.graphics_library_manifest: contract mismatch")

    normalized: list[dict[str, object]] = []
    identities: set[tuple[str, str, int, str]] = set()
    for library in manifest["libraries"]:
        if not isinstance(library, dict) or set(library) != {
            "role",
            "name",
            "path_kind",
            "size",
            "sha256",
        }:
            raise ReportError(f"{label}.graphics_library_manifest: malformed entry")
        role = library.get("role")
        name = library.get("name")
        path_kind = library.get("path_kind")
        size = library.get("size")
        sha256 = library.get("sha256")
        if (
            role not in ("driver", "vendor_dispatch", "api_loader", "layer", "compiler")
            or not isinstance(name, str)
            or not name
            or not is_valid_utf8_scalar_string(name)
            or "/" in name
            or any(delimiter in name for delimiter in ("\0", "\n", "\r"))
            or path_kind not in ("system", "custom", "deleted")
            or isinstance(size, bool)
            or not isinstance(size, int)
            or size <= 0
            or not valid_sha256(sha256)
            or (role, name, size, sha256) in identities
        ):
            raise ReportError(
                f"{label}.graphics_library_manifest: invalid or duplicate entry"
            )
        identities.add((role, name, size, sha256))
        normalized.append({"role": role, "name": name, "size": size, "sha256": sha256})
    normalized.sort(
        key=lambda library: (
            str(library["role"]),
            str(library["name"]),
            int(library["size"]),
            str(library["sha256"]),
        )
    )
    count = record_integer(
        record.get("graphics_library_count"),
        f"{label}.graphics_library_count",
        minimum=1,
    )
    aggregate = record.get("graphics_library_sha256")
    if (
        not normalized
        or not any(
            library["role"] in ("driver", "vendor_dispatch") for library in normalized
        )
        or count != len(normalized)
        or not valid_sha256(aggregate)
        or aggregate != graphics_library_aggregate(normalized)
    ):
        raise ReportError(
            f"{label}: graphics-library count or aggregate is inconsistent"
        )
    return {
        "aggregate_sha256": aggregate,
        "library_count": count,
        "libraries": normalized,
    }


def json_integer(value: object, label: str, *, minimum: int = 0) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        raise ReportError(f"{label}: expected an integer >= {minimum}, got {value!r}")
    return value


def record_integer(value: object, label: str, *, minimum: int = 0) -> int:
    if not isinstance(value, str):
        raise ReportError(f"{label}: expected a string integer, got {value!r}")
    try:
        result = int(value)
    except ValueError as error:
        raise ReportError(
            f"{label}: expected a string integer, got {value!r}"
        ) from error
    if result < minimum:
        raise ReportError(f"{label}: expected an integer >= {minimum}, got {value!r}")
    return result


def signed_record_integer(value: object, label: str) -> int:
    if not isinstance(value, str):
        raise ReportError(f"{label}: expected a string integer, got {value!r}")
    try:
        return int(value)
    except ValueError as error:
        raise ReportError(
            f"{label}: expected a string integer, got {value!r}"
        ) from error


def record_flag(value: object, label: str) -> int:
    result = record_integer(value, label)
    if result not in (0, 1):
        raise ReportError(f"{label}: expected 0 or 1, got {value!r}")
    return result


def required_string(value: object, label: str, *, allow_empty: bool = False) -> str:
    if not isinstance(value, str) or (not allow_empty and not value):
        suffix = "string" if allow_empty else "nonempty string"
        raise ReportError(f"{label}: expected a {suffix}, got {value!r}")
    return value


def positive_record_float(value: object, label: str) -> float:
    if not isinstance(value, str):
        raise ReportError(f"{label}: expected a numeric string, got {value!r}")
    return finite_number(value, label, positive=True)


def record_dimension(value: object, label: str) -> tuple[int, int]:
    if (
        not isinstance(value, str)
        or (match := DIMENSION_PATTERN.fullmatch(value)) is None
    ):
        raise ReportError(f"{label}: expected WIDTHxHEIGHT, got {value!r}")
    result = int(match.group(1)), int(match.group(2))
    if result[0] <= 0 or result[1] <= 0:
        raise ReportError(f"{label}: dimensions must be positive, got {value!r}")
    return result


def record_rect(value: object, label: str) -> tuple[int, int]:
    if not isinstance(value, str) or (match := RECT_PATTERN.fullmatch(value)) is None:
        raise ReportError(f"{label}: expected X,Y,WIDTHxHEIGHT, got {value!r}")
    result = int(match.group(1)), int(match.group(2))
    if result[0] <= 0 or result[1] <= 0:
        raise ReportError(f"{label}: rectangle dimensions must be positive")
    return result


def typed_configuration(report: dict[str, object], role: str) -> dict[str, object]:
    configuration = report.get("configuration")
    if not isinstance(configuration, dict):
        raise ReportError(f"{role}.configuration: missing object")
    required_string(configuration.get("profile"), f"{role}.configuration.profile")

    backends = configuration.get("backends")
    if (
        not isinstance(backends, list)
        or not backends
        or any(backend not in SUPPORTED_BACKENDS for backend in backends)
        or len(backends) != len(set(backends))
    ):
        raise ReportError(
            f"{role}.configuration.backends: expected unique supported backends"
        )
    required_backends = configuration.get("required_backends")
    if (
        not isinstance(required_backends, list)
        or any(backend not in backends for backend in required_backends)
        or len(required_backends) != len(set(required_backends))
    ):
        raise ReportError(
            f"{role}.configuration.required_backends: expected a unique backend subset"
        )

    scales = configuration.get("synthetic_global_scales")
    if not isinstance(scales, list) or not scales:
        raise ReportError(
            f"{role}.configuration.synthetic_global_scales: missing nonempty array"
        )
    normalized_scales: list[str] = []
    for scale in scales:
        if not isinstance(scale, str):
            raise ReportError(
                f"{role}.configuration.synthetic_global_scales: non-string scale"
            )
        parsed = finite_number(scale, f"{role}.configuration.scale", positive=True)
        canonical = format(parsed, ".12g")
        if scale != canonical or canonical in normalized_scales:
            raise ReportError(
                f"{role}.configuration.synthetic_global_scales: "
                f"noncanonical or duplicate scale {scale!r}"
            )
        normalized_scales.append(canonical)
    if "1" not in normalized_scales:
        raise ReportError(
            f"{role}.configuration.synthetic_global_scales: scale 1 is required"
        )

    for field in (
        "pane_warmup",
        "pane_iterations",
        "shader_warmup",
        "shader_iterations",
    ):
        json_integer(
            configuration.get(field), f"{role}.configuration.{field}", minimum=1
        )
    for field in ("allow_software_device", "build_performed"):
        if not isinstance(configuration.get(field), bool):
            raise ReportError(
                f"{role}.configuration.{field}: expected a boolean, "
                f"got {configuration.get(field)!r}"
            )
    scenarios = configuration.get("renderdoc_scenarios")
    if not isinstance(scenarios, list) or any(
        not isinstance(value, str) or not value for value in scenarios
    ):
        raise ReportError(
            f"{role}.configuration.renderdoc_scenarios: expected a string array"
        )
    return configuration


def validate_host(report: dict[str, object], role: str) -> None:
    host = report.get("host")
    if not isinstance(host, dict):
        raise ReportError(f"{role}.host: missing object")
    uname = host.get("uname")
    if (
        not isinstance(uname, list)
        or len(uname) < 5
        or any(not isinstance(value, str) for value in uname)
        or any(not uname[index] for index in (0, 1, 2, 4))
    ):
        raise ReportError(f"{role}.host.uname: incomplete platform identity")
    json_integer(host.get("processors"), f"{role}.host.processors", minimum=1)
    required_string(host.get("cpu_model"), f"{role}.host.cpu_model")
    os_release = host.get("os_release")
    if (
        not isinstance(os_release, dict)
        or not os_release
        or any(
            not isinstance(key, str) or not isinstance(value, str)
            for key, value in os_release.items()
        )
    ):
        raise ReportError(f"{role}.host.os_release: incomplete object")
    required_string(host.get("repository_revision"), f"{role}.host.repository_revision")
    if not isinstance(host.get("repository_dirty"), bool):
        raise ReportError(f"{role}.host.repository_dirty: expected a boolean")
    required_string(host.get("ghostty_revision"), f"{role}.host.ghostty_revision")
    environment = host.get("environment")
    if not isinstance(environment, dict) or environment.get("XDG_SESSION_TYPE") not in (
        None,
        "",
        "wayland",
    ):
        raise ReportError(f"{role}.host.environment: incompatible session identity")
    if any(not isinstance(value, str) for value in environment.values()):
        raise ReportError(f"{role}.host.environment: values must be strings")

    wayland_peer = host.get("wayland_peer")
    if (
        not isinstance(wayland_peer, dict)
        or wayland_peer.get("identity_source") != "linux_so_peercred"
        or wayland_peer.get("diagnostic") not in (None, "")
    ):
        raise ReportError(f"{role}.host.wayland_peer: missing peer credentials")
    peer = wayland_peer.get("peer")
    if not isinstance(peer, dict):
        raise ReportError(f"{role}.host.wayland_peer.peer: missing object")
    json_integer(peer.get("pid"), f"{role}.host.wayland_peer.peer.pid", minimum=1)
    json_integer(peer.get("uid"), f"{role}.host.wayland_peer.peer.uid")
    json_integer(peer.get("gid"), f"{role}.host.wayland_peer.peer.gid")
    required_string(peer.get("comm"), f"{role}.host.wayland_peer.peer.comm")
    required_string(peer.get("executable"), f"{role}.host.wayland_peer.peer.executable")
    if not valid_sha256(peer.get("executable_sha256")):
        raise ReportError(
            f"{role}.host.wayland_peer.peer.executable_sha256: missing or malformed"
        )
    json_integer(
        peer.get("executable_size"),
        f"{role}.host.wayland_peer.peer.executable_size",
        minimum=1,
    )


def validate_report(report: dict[str, object], role: str) -> list[str]:
    errors: list[str] = []
    if report.get("schema_version") != SUPPORTED_QUALIFICATION_SCHEMA:
        errors.append(
            f"{role}.schema_version: expected {SUPPORTED_QUALIFICATION_SCHEMA}, "
            f"got {report.get('schema_version')!r}"
        )
    if report.get("status") != "pass" or report.get("reason_code") != "completed":
        errors.append(
            f"{role}.status: expected pass/completed, got "
            f"{report.get('status')!r}/{report.get('reason_code')!r}"
        )
    configuration = report.get("configuration")
    if not isinstance(configuration, dict):
        errors.append(f"{role}.configuration: missing object")
    else:
        for field in CONFIGURATION_FIELDS:
            if field not in configuration:
                errors.append(f"{role}.configuration.{field}: missing")
    runs = report.get("runs")
    if not isinstance(runs, list):
        errors.append(f"{role}.runs: missing array")
    harness = report.get("harness")
    if not isinstance(harness, dict) or not valid_sha256(harness.get("sha256")):
        errors.append(f"{role}.harness.sha256: missing or malformed")
    artifacts = report.get("artifacts")
    required_artifacts = (
        "ghostty-qt",
        "bench-terminal-pane-renderer",
        "bench-terminal-custom-shader-rhi",
    )
    if not isinstance(artifacts, dict):
        errors.append(f"{role}.artifacts: missing object")
    else:
        for name in required_artifacts:
            artifact = artifacts.get(name)
            if not isinstance(artifact, dict) or not valid_sha256(
                artifact.get("sha256")
            ):
                errors.append(f"{role}.artifacts.{name}.sha256: missing or malformed")
    if not errors:
        try:
            typed_configuration(report, role)
            validate_host(report, role)
            validate_complete_run_matrix(report, role)
        except ReportError as error:
            errors.append(str(error))
    return errors


def records_of(run: dict[str, object], record_type: str) -> list[dict[str, str]]:
    records = run.get("records")
    if not isinstance(records, list):
        return []
    return [
        record
        for record in records
        if isinstance(record, dict) and record.get("record") == record_type
    ]


def run_key(run: dict[str, object]) -> tuple[str, str, str]:
    kind = run.get("kind")
    backend = run.get("backend")
    scale = run.get("scale", "production")
    if not isinstance(kind, str) or not isinstance(backend, str):
        raise ReportError("measured run is missing kind or backend")
    if scale is None:
        scale = "production"
    if not isinstance(scale, str):
        raise ReportError(f"{kind}/{backend} has a non-string scale")
    return kind, backend, scale


def measured_runs(
    report: dict[str, object],
) -> dict[tuple[str, str, str], dict[str, object]]:
    runs = report.get("runs")
    if not isinstance(runs, list):
        raise ReportError("runs is not an array")
    result: dict[tuple[str, str, str], dict[str, object]] = {}
    for value in runs:
        if not isinstance(value, dict) or value.get("kind") not in MEASURED_RUN_KINDS:
            continue
        key = run_key(value)
        if key in result:
            raise ReportError(f"duplicate measured run: {'/'.join(key)}")
        result[key] = value
    if not result:
        raise ReportError("report has no measured renderer runs")
    return result


def record_identity(run: dict[str, object], record: dict[str, str]) -> tuple[str, ...]:
    kind, backend, scale = run_key(run)
    record_type = record.get("record", "")
    if record_type == "pane_scenario":
        fields = (record.get("grid"), record.get("scenario"))
    elif record_type == "shader_scenario":
        fields = (
            record.get("renderer"),
            record.get("workload"),
            record.get("passes"),
        )
    elif record_type == "shader_baseline":
        fields = ("baseline", record.get("workload"))
    else:
        raise ReportError(f"unsupported measurement record: {record_type!r}")
    if any(not isinstance(field, str) or not field for field in fields):
        raise ReportError(f"{kind}/{backend}/{scale} has incomplete record identity")
    return (kind, backend, scale, record_type, *fields)  # type: ignore[arg-type]


def measurement_records(
    report: dict[str, object],
) -> dict[tuple[str, ...], dict[str, str]]:
    result: dict[tuple[str, ...], dict[str, str]] = {}
    for run in measured_runs(report).values():
        for record_type in (
            "pane_scenario",
            "shader_baseline",
            "shader_scenario",
        ):
            for record in records_of(run, record_type):
                identity = record_identity(run, record)
                if identity in result:
                    raise ReportError(
                        f"duplicate measurement record: {'/'.join(identity)}"
                    )
                result[identity] = record
    if not result:
        raise ReportError("report has no renderer measurement records")
    return result


def typed_run_records(run: dict[str, object], label: str) -> list[dict[str, str]]:
    records = run.get("records")
    if not isinstance(records, list) or any(
        not isinstance(record, dict) for record in records
    ):
        raise ReportError(f"{label}.records: expected an array of objects")
    result: list[dict[str, str]] = []
    for index, record in enumerate(records):
        assert isinstance(record, dict)
        if any(
            not isinstance(key, str) or not isinstance(value, str)
            for key, value in record.items()
        ):
            raise ReportError(
                f"{label}.records[{index}]: keys and values must be strings"
            )
        result.append(record)
    return result


def validated_device(run: dict[str, object], label: str) -> dict[str, str]:
    value = run.get("rhi_device")
    if not isinstance(value, dict):
        raise ReportError(f"{label}.rhi_device: missing object")
    device: dict[str, str] = {}
    for field in DEVICE_FIELDS:
        device[field] = required_string(value.get(field), f"{label}.rhi_device.{field}")
    return device


def validate_record_device(
    record: dict[str, str], device: dict[str, str], label: str
) -> None:
    actual = {field: record.get(field) for field in DEVICE_FIELDS}
    if actual != device:
        raise ReportError(f"{label}: record device does not match run device")


def validate_framebuffer(
    logical_value: object,
    framebuffer_value: object,
    dpr_value: object,
    label: str,
) -> None:
    logical = record_dimension(logical_value, f"{label}.logical")
    framebuffer = record_dimension(framebuffer_value, f"{label}.framebuffer")
    dpr = positive_record_float(dpr_value, f"{label}.dpr")
    expected = tuple(round(value * dpr) for value in logical)
    if any(
        abs(actual - wanted) > 1
        for actual, wanted in zip(framebuffer, expected, strict=True)
    ):
        raise ReportError(f"{label}: framebuffer does not match logical size and DPR")


def validate_pane_evidence(
    run: dict[str, object],
    role: str,
    scenarios: list[str],
    configuration: dict[str, object],
) -> None:
    kind, backend, scale = run_key(run)
    label = f"{role}.run.{kind}/{backend}/{scale}"
    records = typed_run_records(run, label)
    record_counts = Counter(record.get("record") for record in records)
    expected_record_count = 1 + len(PANE_GRIDS) + len(PANE_GRIDS) * len(scenarios)
    if (
        len(records) != expected_record_count
        or record_counts["pane_header"] != 1
        or record_counts["pane_grid"] != len(PANE_GRIDS)
        or record_counts["pane_scenario"] != len(PANE_GRIDS) * len(scenarios)
        or set(record_counts) != {"pane_header", "pane_grid", "pane_scenario"}
    ):
        raise ReportError(f"{label}: incomplete pane record matrix")
    header = next(record for record in records if record["record"] == "pane_header")
    for field in PANE_HEADER_FIELDS:
        required_string(header.get(field), f"{label}.header.{field}")
    if (
        header["benchmark_contract"] != "1"
        or header["backend"] != backend
        or header["platform"] != "wayland"
        or header["presentation"] != "offscreen"
        or record_integer(header["warmup"], f"{label}.header.warmup", minimum=1)
        != configuration["pane_warmup"]
        or record_integer(header["iterations"], f"{label}.header.iterations", minimum=1)
        != configuration["pane_iterations"]
    ):
        raise ReportError(f"{label}: pane header disagrees with the run contract")

    device = validated_device(run, label)
    grids = [record for record in records if record["record"] == "pane_grid"]
    if Counter(record.get("grid") for record in grids) != Counter(PANE_GRIDS):
        raise ReportError(f"{label}: pane grids are incomplete")
    for grid in grids:
        grid_name = required_string(grid.get("grid"), f"{label}.grid")
        validate_framebuffer(
            grid.get("logical"),
            grid.get("framebuffer"),
            grid.get("dpr"),
            f"{label}.{grid_name}",
        )
        validate_record_device(grid, device, f"{label}.{grid_name}")

    scenario_records = [
        record for record in records if record["record"] == "pane_scenario"
    ]
    expected_identities = Counter(
        (grid, scenario) for grid in PANE_GRIDS for scenario in scenarios
    )
    actual_identities = Counter(
        (record.get("grid"), record.get("scenario")) for record in scenario_records
    )
    if actual_identities != expected_identities:
        raise ReportError(f"{label}: pane scenario identities are incomplete")
    expected_frames = int(configuration["pane_iterations"])
    for record in scenario_records:
        identity = f"{label}.{record['grid']}/{record['scenario']}"
        if (
            record_integer(
                record.get("measured_frames"), f"{identity}.measured_frames", minimum=1
            )
            != expected_frames
            or record_integer(record.get("paints"), f"{identity}.paints", minimum=1)
            != expected_frames
        ):
            raise ReportError(f"{identity}: measured frame count is inconsistent")
        positive_record_float(
            record.get("cpu_total_median_us"), f"{identity}.cpu_total_median_us"
        )
        positive_record_float(
            record.get("cpu_total_p90_us"), f"{identity}.cpu_total_p90_us"
        )
        complete_gpu_value(
            record,
            "gpu_median_us",
            f"{identity}.gpu_median_us",
            expected_samples=expected_frames,
            slash_samples=True,
        )
        for field in PANE_WORK_FIELDS:
            record_integer(record.get(field), f"{identity}.{field}")
        signed_record_integer(
            record.get("kitty_texture_set_count_delta"),
            f"{identity}.kitty_texture_set_count_delta",
        )


def validate_shader_evidence(
    run: dict[str, object], role: str, configuration: dict[str, object]
) -> None:
    kind, backend, scale = run_key(run)
    label = f"{role}.run.{kind}/{backend}/{scale}"
    records = typed_run_records(run, label)
    record_counts = Counter(record.get("record") for record in records)
    expected_scenarios = (
        len(SHADER_RENDERERS) * len(SHADER_WORKLOADS) * len(SHADER_PASS_COUNTS)
    )
    if (
        len(records) != 1 + len(SHADER_WORKLOADS) + expected_scenarios
        or record_counts["shader_header"] != 1
        or record_counts["shader_baseline"] != len(SHADER_WORKLOADS)
        or record_counts["shader_scenario"] != expected_scenarios
        or set(record_counts) != {"shader_header", "shader_baseline", "shader_scenario"}
    ):
        raise ReportError(f"{label}: incomplete custom-shader record matrix")
    header = next(record for record in records if record["record"] == "shader_header")
    for field in SHADER_HEADER_FIELDS:
        required_string(header.get(field), f"{label}.header.{field}")
    if (
        header["benchmark_contract"] != "1"
        or header["graphics_api"] != backend
        or header["platform"] != "wayland"
        or header["completion"] != "offscreen-end-frame"
        or header["gpu_timestamps"] not in ("supported", "unavailable")
        or record_integer(header["warmup"], f"{label}.header.warmup", minimum=1)
        != configuration["shader_warmup"]
        or record_integer(header["iterations"], f"{label}.header.iterations", minimum=1)
        != configuration["shader_iterations"]
    ):
        raise ReportError(
            f"{label}: custom-shader header disagrees with the run contract"
        )
    validate_framebuffer(
        header.get("viewport"), header.get("framebuffer"), header.get("dpr"), label
    )
    device = validated_device(run, label)
    validate_record_device(header, device, f"{label}.header")

    baselines = [record for record in records if record["record"] == "shader_baseline"]
    if Counter(record.get("workload") for record in baselines) != Counter(
        SHADER_WORKLOADS
    ):
        raise ReportError(f"{label}: custom-shader baselines are incomplete")
    for baseline in baselines:
        identity = f"{label}.baseline/{baseline['workload']}"
        positive_record_float(
            baseline.get("cpu_total_median_us"), f"{identity}.cpu_total_median_us"
        )
        record_integer(
            baseline.get("cpu_pooled_samples"),
            f"{identity}.cpu_pooled_samples",
            minimum=1,
        )
        gpu = optional_number(
            baseline.get("gpu_median_us"), f"{identity}.gpu_median_us"
        )
        gpu_samples = record_integer(
            baseline.get("gpu_pooled_samples"), f"{identity}.gpu_pooled_samples"
        )
        if (gpu is None) != (gpu_samples == 0) or (gpu is not None and gpu <= 0.0):
            raise ReportError(f"{identity}: inconsistent pooled GPU evidence")

    scenario_records = [
        record for record in records if record["record"] == "shader_scenario"
    ]
    expected_identities = Counter(
        (renderer, workload, passes)
        for renderer in SHADER_RENDERERS
        for workload in SHADER_WORKLOADS
        for passes in SHADER_PASS_COUNTS
    )
    actual_identities = Counter(
        (record.get("renderer"), record.get("workload"), record.get("passes"))
        for record in scenario_records
    )
    if actual_identities != expected_identities:
        raise ReportError(f"{label}: custom-shader scenario identities are incomplete")
    by_identity = {
        (record["renderer"], record["workload"], record["passes"]): record
        for record in scenario_records
    }
    expected_frames = int(configuration["shader_iterations"])
    gpu_values: dict[tuple[str, str, str], float | None] = {}
    for identity_key, record in by_identity.items():
        identity = f"{label}.{'/'.join(identity_key)}"
        if (
            record_integer(
                record.get("measured_frames"), f"{identity}.measured_frames", minimum=1
            )
            != expected_frames
        ):
            raise ReportError(f"{identity}: measured frame count is inconsistent")
        positive_record_float(
            record.get("cpu_total_median_us"), f"{identity}.cpu_total_median_us"
        )
        positive_record_float(
            record.get("cpu_total_p90_us"), f"{identity}.cpu_total_p90_us"
        )
        gpu_values[identity_key] = complete_gpu_value(
            record,
            "gpu_median_us",
            f"{identity}.gpu_median_us",
            expected_samples=expected_frames,
            slash_samples=False,
        )
        for field in SHADER_WORK_FIELDS:
            if field not in record:
                raise ReportError(f"{identity}.{field}: missing")
            nonnegative_integer(record.get(field), f"{identity}.{field}")
        cpu_ratio = optional_number(
            record.get("cpu_total_vs_legacy_ratio"),
            f"{identity}.cpu_total_vs_legacy_ratio",
        )
        if cpu_ratio is None or cpu_ratio <= 0.0:
            raise ReportError(
                f"{identity}.cpu_total_vs_legacy_ratio: missing or invalid"
            )

    for workload in SHADER_WORKLOADS:
        for passes in SHADER_PASS_COUNTS:
            retained_key = ("retained", workload, passes)
            legacy_key = ("legacy", workload, passes)
            record = by_identity[retained_key]
            identity = f"{label}.{'/'.join(retained_key)}"
            ratio = optional_number(
                record.get("gpu_vs_legacy_ratio"), f"{identity}.gpu_vs_legacy_ratio"
            )
            has_pair = (
                gpu_values[retained_key] is not None
                and gpu_values[legacy_key] is not None
            )
            if has_pair and (ratio is None or ratio <= 0.0):
                raise ReportError(f"{identity}.gpu_vs_legacy_ratio: missing or invalid")
            if not has_pair and ratio is not None:
                raise ReportError(
                    f"{identity}.gpu_vs_legacy_ratio: present without complete GPU evidence"
                )


def validate_production_evidence(run: dict[str, object], role: str) -> None:
    kind, backend, scale = run_key(run)
    label = f"{role}.run.{kind}/{backend}/{scale}"
    records = typed_run_records(run, label)
    if len(records) != 1 or records[0].get("record") != "swapchain":
        raise ReportError(f"{label}: expected exactly one swapchain record")
    record = records[0]
    for field in (*PRODUCTION_STRUCTURAL_FIELDS, *PRODUCTION_CONTEXT_FIELDS):
        required_string(
            record.get(field),
            f"{label}.{field}",
            allow_empty=field
            in ("screen_name", "screen_manufacturer", "screen_model", "screen_serial"),
        )
    if (
        record["benchmark_contract"] != "1"
        or record["platform"] != "wayland"
        or record["graphics_api"] != backend
    ):
        raise ReportError(f"{label}: production contract or backend mismatch")
    graphics_library_identity(record, backend, label)
    device = validated_device(run, label)
    validate_record_device(record, device, label)

    logical = record_dimension(record.get("logical"), f"{label}.logical")
    physical = record_dimension(record.get("physical"), f"{label}.physical")
    dpr = positive_record_float(record.get("dpr"), f"{label}.dpr")
    expected_physical = tuple(round(value * dpr) for value in logical)
    if any(
        abs(actual - wanted) > 1
        for actual, wanted in zip(physical, expected_physical, strict=True)
    ):
        raise ReportError(f"{label}: production physical size does not match DPR")
    if (
        record_dimension(record.get("swapchain_size"), f"{label}.swapchain_size")
        != physical
    ):
        raise ReportError(f"{label}: swapchain size does not match the surface")
    record_rect(record.get("screen_geometry"), f"{label}.screen_geometry")
    record_rect(
        record.get("screen_available_geometry"), f"{label}.screen_available_geometry"
    )
    positive_record_float(record.get("screen_dpr"), f"{label}.screen_dpr")
    record_integer(record.get("screen_count"), f"{label}.screen_count", minimum=1)
    record_integer(record.get("screen_depth"), f"{label}.screen_depth", minimum=1)
    record_integer(
        record.get("screen_refresh_millihz"), f"{label}.screen_refresh_millihz"
    )
    record_integer(record.get("screen_orientation"), f"{label}.screen_orientation")
    record_flag(record.get("screen_primary"), f"{label}.screen_primary")
    swapchain_flags = record_integer(
        record.get("swapchain_flags"), f"{label}.swapchain_flags"
    )
    record_integer(
        record.get("swapchain_samples"), f"{label}.swapchain_samples", minimum=1
    )
    swapchain_srgb = record_flag(
        record.get("swapchain_srgb"), f"{label}.swapchain_srgb"
    )
    swapchain_premultiplied_alpha = record_flag(
        record.get("swapchain_premultiplied_alpha"),
        f"{label}.swapchain_premultiplied_alpha",
    )
    swapchain_nonpremultiplied_alpha = record_flag(
        record.get("swapchain_nonpremultiplied_alpha"),
        f"{label}.swapchain_nonpremultiplied_alpha",
    )
    swapchain_no_vsync = record_flag(
        record.get("swapchain_no_vsync"), f"{label}.swapchain_no_vsync"
    )
    swapchain_hdr = record_flag(record.get("swapchain_hdr"), f"{label}.swapchain_hdr")
    if swapchain_premultiplied_alpha + swapchain_nonpremultiplied_alpha != 1:
        raise ReportError(f"{label}: swapchain must have exactly one alpha mode")
    flag_values = (
        (swapchain_premultiplied_alpha, 1 << 0, "premultiplied alpha"),
        (swapchain_nonpremultiplied_alpha, 1 << 1, "nonpremultiplied alpha"),
        (swapchain_srgb, 1 << 2, "sRGB"),
        (swapchain_no_vsync, 1 << 4, "no-vsync"),
    )
    if any(value != int(bool(swapchain_flags & bit)) for value, bit, _ in flag_values):
        raise ReportError(f"{label}: projected swapchain flags are inconsistent")
    if swapchain_no_vsync != 0:
        raise ReportError(f"{label}: no-vsync swapchain is not comparable")

    swapchain_format = record["swapchain_format"]
    if swapchain_format not in (
        "sdr",
        "hdr-extended-srgb-linear",
        "hdr10",
        "hdr-extended-display-p3-linear",
    ):
        raise ReportError(f"{label}: unknown swapchain format")
    if swapchain_hdr != int(swapchain_format != "sdr"):
        raise ReportError(f"{label}: swapchain HDR flag disagrees with its format")
    hdr_fields = (
        "swapchain_hdr_limits",
        "swapchain_hdr_behavior",
        "swapchain_hdr_minimum",
        "swapchain_hdr_maximum",
        "swapchain_hdr_maximum_potential",
        "swapchain_hdr_sdr_white",
    )
    if not swapchain_hdr:
        if any(record[field] != "na" for field in hdr_fields):
            raise ReportError(f"{label}: SDR swapchain contains HDR metadata")
    else:
        limits = record["swapchain_hdr_limits"]
        if record["swapchain_hdr_behavior"] not in (
            "scene-referred",
            "display-referred",
        ):
            raise ReportError(f"{label}: invalid HDR luminance behavior")
        maximum = finite_number(
            record["swapchain_hdr_maximum"],
            f"{label}.swapchain_hdr_maximum",
            positive=True,
        )
        sdr_white = optional_number(
            record["swapchain_hdr_sdr_white"],
            f"{label}.swapchain_hdr_sdr_white",
        )
        if sdr_white is None or sdr_white < 0.0:
            raise ReportError(f"{label}: invalid HDR SDR-white level")
        if limits == "nits":
            minimum = optional_number(
                record["swapchain_hdr_minimum"],
                f"{label}.swapchain_hdr_minimum",
            )
            if (
                minimum is None
                or minimum < 0.0
                or minimum > maximum
                or record["swapchain_hdr_maximum_potential"] != "na"
            ):
                raise ReportError(f"{label}: invalid HDR nits limits")
        elif limits == "color-component":
            maximum_potential = finite_number(
                record["swapchain_hdr_maximum_potential"],
                f"{label}.swapchain_hdr_maximum_potential",
                positive=True,
            )
            if record["swapchain_hdr_minimum"] != "na" or maximum > maximum_potential:
                raise ReportError(f"{label}: invalid HDR component limits")
        else:
            raise ReportError(f"{label}: invalid HDR limits type")

    record_integer(record.get("frame_swaps"), f"{label}.frame_swaps", minimum=30)
    positive_record_float(
        record.get("median_frame_interval_us"), f"{label}.median_frame_interval_us"
    )
    positive_record_float(
        record.get("p90_frame_interval_us"), f"{label}.p90_frame_interval_us"
    )
    record_integer(
        record.get("alpha_buffer_bits"), f"{label}.alpha_buffer_bits", minimum=1
    )
    minimum_alpha = record_integer(record.get("min_alpha"), f"{label}.min_alpha")
    maximum_alpha = record_integer(record.get("max_alpha"), f"{label}.max_alpha")
    if (
        record_flag(record.get("image_alpha"), f"{label}.image_alpha") != 1
        or record_flag(record.get("nonuniform"), f"{label}.nonuniform") != 1
        or record_integer(record.get("clear_alpha"), f"{label}.clear_alpha") != 0
        or minimum_alpha >= 255
        or maximum_alpha > 255
        or minimum_alpha > maximum_alpha
    ):
        raise ReportError(f"{label}: invalid production alpha evidence")
    pixel_count = record_integer(
        record.get("pixel_count"), f"{label}.pixel_count", minimum=1
    )
    expected_pixels = physical[0] * physical[1]
    if pixel_count != expected_pixels:
        raise ReportError(f"{label}: pixel count does not match physical size")
    minimum_half = record_integer(
        record.get("minimum_half_alpha_pixels"),
        f"{label}.minimum_half_alpha_pixels",
        minimum=1,
    )
    half = record_integer(record.get("half_alpha_pixels"), f"{label}.half_alpha_pixels")
    translucent = record_integer(
        record.get("translucent_pixels"), f"{label}.translucent_pixels", minimum=1
    )
    if (
        minimum_half != max(1, pixel_count // 20)
        or not minimum_half <= half <= translucent <= pixel_count
    ):
        raise ReportError(f"{label}: inconsistent translucent-pixel evidence")
    record_integer(record.get("panes"), f"{label}.panes", minimum=1)
    record_integer(record.get("running_panes"), f"{label}.running_panes", minimum=1)


def validate_complete_run_matrix(report: dict[str, object], role: str) -> None:
    configuration = typed_configuration(report, role)
    runs = report.get("runs")
    if not isinstance(runs, list) or any(not isinstance(run, dict) for run in runs):
        raise ReportError(f"{role}.runs: expected an array of objects")
    discovery_runs = [
        run
        for run in runs
        if isinstance(run, dict) and run.get("kind") == "scenario_discovery"
    ]
    if len(discovery_runs) != 1:
        raise ReportError(f"{role}: expected exactly one scenario-discovery run")
    discovery = discovery_runs[0]
    if discovery.get("status") != "pass" or discovery.get("reason_code") != "completed":
        raise ReportError(f"{role}: scenario discovery did not pass")
    scenarios = discovery.get("scenarios")
    if (
        not isinstance(scenarios, list)
        or not scenarios
        or any(not isinstance(value, str) or not value for value in scenarios)
        or len(scenarios) != len(set(scenarios))
    ):
        raise ReportError(
            f"{role}: scenario catalog is missing, malformed, or duplicated"
        )

    build_runs = [
        run
        for run in runs
        if isinstance(run, dict) and run.get("kind") in ("configure", "build")
    ]
    if configuration["build_performed"]:
        if Counter(run.get("kind") for run in build_runs) != Counter(
            ("configure", "build")
        ):
            raise ReportError(
                f"{role}: build_performed does not match build-run evidence"
            )
        if any(run.get("status") != "pass" for run in build_runs):
            raise ReportError(f"{role}: recorded build did not pass")
    elif build_runs:
        raise ReportError(
            f"{role}: unexpected build runs when build_performed is false"
        )

    measured = measured_runs(report)
    scales = list(configuration["synthetic_global_scales"])
    backends = list(configuration["backends"])
    for key, run in measured.items():
        kind, backend, scale = key
        if backend not in backends:
            raise ReportError(f"{role}: run for unconfigured backend {backend}")
        expected_scale = (
            scale == "production" if kind == "production_swapchain" else scale in scales
        )
        if not expected_scale:
            raise ReportError(f"{role}: invalid scale for {'/'.join(key)}")
        if run.get("status") not in ("pass", "skip"):
            raise ReportError(
                f"{role}: measured run {'/'.join(key)} did not pass or skip"
            )
        if run.get("status") == "pass" and run.get("reason_code") != "completed":
            raise ReportError(
                f"{role}: passing run {'/'.join(key)} has an invalid reason"
            )
        if run.get("status") == "skip" and run.get("reason_code") not in (
            "unsupported_backend",
            "unsupported_swapchain",
        ):
            raise ReportError(
                f"{role}: skipped run {'/'.join(key)} has an invalid reason"
            )

    passed_backends: list[str] = []
    skipped_backends: list[str] = []
    for backend in backends:
        production_key = ("production_swapchain", backend, "production")
        production = measured.get(production_key)
        complete_offscreen_keys = {
            (kind, backend, scale)
            for scale in scales
            for kind in ("pane_offscreen", "custom_shader_offscreen")
        }
        backend_keys = {key for key in measured if key[1] == backend}
        if production is not None:
            if backend_keys != complete_offscreen_keys | {production_key}:
                raise ReportError(
                    f"{role}: incomplete run matrix for backend {backend}"
                )
            if any(
                measured[key].get("status") != "pass" for key in complete_offscreen_keys
            ):
                raise ReportError(
                    f"{role}: incomplete passing offscreen matrix for {backend}"
                )
            if production.get("status") == "pass":
                passed_backends.append(backend)
            else:
                skipped_backends.append(backend)
        else:
            first_scale = scales[0]
            pane_key = ("pane_offscreen", backend, first_scale)
            shader_key = ("custom_shader_offscreen", backend, first_scale)
            pane = measured.get(pane_key)
            shader = measured.get(shader_key)
            pane_unavailable = (
                pane is not None
                and pane.get("status") == "skip"
                and backend_keys == {pane_key}
            )
            shader_unavailable = (
                pane is not None
                and pane.get("status") == "pass"
                and shader is not None
                and shader.get("status") == "skip"
                and backend_keys == {pane_key, shader_key}
            )
            if pane_unavailable or shader_unavailable:
                skipped_backends.append(backend)
            else:
                raise ReportError(
                    f"{role}: missing production or unsupported-backend evidence for {backend}"
                )

    if not passed_backends:
        raise ReportError(f"{role}: passing report has no production backend")
    missing_required = sorted(
        set(configuration["required_backends"]) - set(passed_backends)
    )
    if missing_required:
        raise ReportError(
            f"{role}: required backends did not pass: {', '.join(missing_required)}"
        )
    for key, run in measured.items():
        if run.get("status") != "pass":
            continue
        if key[0] == "pane_offscreen":
            validate_pane_evidence(run, role, scenarios, configuration)
        elif key[0] == "custom_shader_offscreen":
            validate_shader_evidence(run, role, configuration)
        else:
            validate_production_evidence(run, role)

    summary = report.get("summary")
    if not isinstance(summary, dict):
        raise ReportError(f"{role}.summary: missing object")
    if (
        summary.get("passed_backends") != passed_backends
        or summary.get("skipped_backends") != skipped_backends
    ):
        raise ReportError(f"{role}.summary: backend summary disagrees with runs")
    for status, field in (
        ("pass", "pass_runs"),
        ("skip", "skip_runs"),
        ("fail", "fail_runs"),
    ):
        expected = sum(
            isinstance(run, dict) and run.get("status") == status for run in runs
        )
        if summary.get(field) != expected:
            raise ReportError(f"{role}.summary.{field}: expected {expected}")


def selected(record: dict[str, str], fields: tuple[str, ...]) -> dict[str, str | None]:
    return {field: record.get(field) for field in fields}


def scenario_catalog(report: dict[str, object]) -> list[str] | None:
    runs = report.get("runs", [])
    if not isinstance(runs, list):
        return None
    catalogs = [
        run.get("scenarios")
        for run in runs
        if isinstance(run, dict) and run.get("kind") == "scenario_discovery"
    ]
    if len(catalogs) != 1 or not isinstance(catalogs[0], list):
        return None
    return [value for value in catalogs[0] if isinstance(value, str)]


def peer_signature(report: dict[str, object]) -> dict[str, object] | None:
    host = report.get("host")
    if not isinstance(host, dict):
        return None
    wayland_peer = host.get("wayland_peer")
    if not isinstance(wayland_peer, dict):
        return None
    peer = wayland_peer.get("peer")
    if not isinstance(peer, dict):
        return None
    return {
        key: peer.get(key)
        for key in (
            "comm",
            "executable",
            "executable_sha256",
            "executable_size",
            "uid",
            "gid",
        )
    }


def context_projection(
    report: dict[str, object],
) -> tuple[dict[str, object], dict[str, object]]:
    structural: dict[str, object] = {}
    environmental: dict[str, object] = {}
    configuration = report.get("configuration")
    assert isinstance(configuration, dict)
    for field in CONFIGURATION_FIELDS:
        structural[f"configuration.{field}"] = configuration.get(field)
    catalog = scenario_catalog(report)
    if not catalog:
        raise ReportError("missing or empty scenario catalog")
    structural["scenario_catalog"] = catalog

    host = report.get("host")
    if isinstance(host, dict):
        uname = host.get("uname")
        if isinstance(uname, list):
            environmental["host.hostname"] = uname[1] if len(uname) > 1 else None
            environmental["host.kernel"] = uname[2] if len(uname) > 2 else None
            environmental["host.architecture"] = uname[4] if len(uname) > 4 else None
        for field in ("processors", "cpu_model", "os_release"):
            environmental[f"host.{field}"] = host.get(field)
        environmental["host.wayland_peer"] = peer_signature(report)
        environment = host.get("environment")
        if isinstance(environment, dict):
            for field in (
                "XDG_CURRENT_DESKTOP",
                "XDG_SESSION_DESKTOP",
                "XDG_SESSION_TYPE",
            ):
                environmental[f"host.environment.{field}"] = environment.get(field)

    for key, run in sorted(measured_runs(report).items()):
        prefix = "/".join(key)
        structural[f"run.{prefix}.status"] = (
            run.get("status"),
            run.get("reason_code"),
        )
        structural[f"run.{prefix}.rhi_device"] = {
            field: (
                run.get("rhi_device", {}).get(field)
                if isinstance(run.get("rhi_device"), dict)
                else None
            )
            for field in DEVICE_FIELDS
        }
        if key[0] == "pane_offscreen" and run.get("status") == "pass":
            headers = records_of(run, "pane_header")
            structural[f"run.{prefix}.header"] = (
                selected(headers[0], PANE_HEADER_FIELDS) if len(headers) == 1 else None
            )
            grids = records_of(run, "pane_grid")
            structural[f"run.{prefix}.grids"] = sorted(
                (selected(record, PANE_GRID_FIELDS) for record in grids),
                key=lambda record: str(record.get("grid")),
            )
        elif key[0] == "custom_shader_offscreen" and run.get("status") == "pass":
            headers = records_of(run, "shader_header")
            structural[f"run.{prefix}.header"] = (
                selected(headers[0], SHADER_HEADER_FIELDS)
                if len(headers) == 1
                else None
            )
        elif key[0] == "production_swapchain" and run.get("status") == "pass":
            records = records_of(run, "swapchain")
            structural[f"run.{prefix}.header"] = (
                selected(records[0], PRODUCTION_STRUCTURAL_FIELDS)
                if len(records) == 1
                else None
            )
            environmental[f"run.{prefix}.output"] = (
                selected(records[0], PRODUCTION_CONTEXT_FIELDS)
                if len(records) == 1
                else None
            )
            environmental[f"run.{prefix}.graphics_libraries"] = (
                graphics_library_identity(records[0], key[1], f"run.{prefix}")
                if len(records) == 1
                else None
            )
    structural["measurement_identities"] = sorted(
        "/".join(identity) for identity in measurement_records(report)
    )
    return structural, environmental


def differences(
    baseline: dict[str, object], candidate: dict[str, object]
) -> list[dict[str, object]]:
    result: list[dict[str, object]] = []
    for field in sorted(set(baseline) | set(candidate)):
        if baseline.get(field) != candidate.get(field):
            result.append(
                {
                    "field": field,
                    "baseline": baseline.get(field),
                    "candidate": candidate.get(field),
                }
            )
    return result


def finite_number(value: str | None, label: str, *, positive: bool) -> float:
    try:
        result = float(value) if value is not None else math.nan
    except (TypeError, ValueError) as error:
        raise ReportError(f"{label} is not numeric: {value!r}") from error
    if not math.isfinite(result) or (positive and result <= 0.0):
        raise ReportError(f"{label} is not a finite positive number: {value!r}")
    return result


def optional_number(value: str | None, label: str) -> float | None:
    if value in (None, "", "na", "unavailable"):
        return None
    try:
        result = float(value)
    except (TypeError, ValueError) as error:
        raise ReportError(f"{label} is not numeric: {value!r}") from error
    if not math.isfinite(result):
        raise ReportError(f"{label} is not finite: {value!r}")
    return result


def complete_gpu_value(
    record: dict[str, str],
    field: str,
    label: str,
    *,
    expected_samples: int,
    slash_samples: bool,
) -> float | None:
    value = optional_number(record.get(field), label)
    samples = record.get("gpu_valid_samples")
    if not isinstance(samples, str):
        raise ReportError(f"{label}/gpu_valid_samples is missing")
    if slash_samples:
        match = re.fullmatch(r"(\d+)/(\d+)", samples)
        if match is None:
            raise ReportError(f"{label}/gpu_valid_samples is malformed: {samples!r}")
        valid_samples = int(match.group(1))
        total_samples = int(match.group(2))
        if total_samples != expected_samples or valid_samples > total_samples:
            raise ReportError(
                f"{label}/gpu_valid_samples disagrees with measured frames"
            )
    else:
        if not samples.isdigit():
            raise ReportError(f"{label}/gpu_valid_samples is malformed: {samples!r}")
        valid_samples = int(samples)
        if valid_samples > expected_samples:
            raise ReportError(f"{label}/gpu_valid_samples exceeds measured frames")
    if value is None:
        if valid_samples == expected_samples:
            raise ReportError(f"{label} is missing despite complete GPU samples")
        return None
    if value <= 0.0:
        raise ReportError(f"{label} is not a finite positive number")
    if valid_samples != expected_samples:
        raise ReportError(f"{label} is present without complete GPU samples")
    return value


def has_enforced_sample_count(report: dict[str, object]) -> bool:
    configuration = report.get("configuration")
    if not isinstance(configuration, dict):
        return False
    values = (
        configuration.get("pane_warmup"),
        configuration.get("pane_iterations"),
        configuration.get("shader_warmup"),
        configuration.get("shader_iterations"),
    )
    return all(isinstance(value, int) and value >= 100 for value in values)


def performance_metric(
    *,
    identity: tuple[str, ...],
    name: str,
    baseline: float,
    candidate: float,
    percent_limit: float,
    absolute_limit: float,
    unit: str,
    enforced: bool,
) -> dict[str, object]:
    delta = candidate - baseline
    percent = delta / baseline * 100.0
    regression_exceeded = delta > absolute_limit and percent > percent_limit
    improvement_exceeded = -delta > absolute_limit and -percent > percent_limit
    if regression_exceeded:
        classification = "regression" if enforced else "advisory-regression"
    elif improvement_exceeded:
        classification = "improvement"
    else:
        classification = "stable"
    return {
        "identity": list(identity),
        "metric": name,
        "category": "timing",
        "unit": unit,
        "baseline": baseline,
        "candidate": candidate,
        "delta": delta,
        "percent": percent,
        "percent_limit": percent_limit,
        "absolute_limit": absolute_limit,
        "enforced": enforced,
        "classification": classification,
    }


def work_metric(
    identity: tuple[str, ...], name: str, baseline: int, candidate: int
) -> dict[str, object]:
    delta = candidate - baseline
    return {
        "identity": list(identity),
        "metric": name,
        "category": "structural-work",
        "unit": "count",
        "baseline": baseline,
        "candidate": candidate,
        "delta": delta,
        "enforced": True,
        "classification": (
            "regression" if delta > 0 else "improvement" if delta < 0 else "stable"
        ),
    }


def nonnegative_integer(value: str | None, label: str) -> int | None:
    if value in (None, "", "na", "unavailable"):
        return None
    try:
        result = int(value)
    except (TypeError, ValueError) as error:
        raise ReportError(f"{label} is not an integer: {value!r}") from error
    if result < 0:
        raise ReportError(f"{label} is negative: {value!r}")
    return result


def compare_metrics(
    baseline: dict[str, object],
    candidate: dict[str, object],
    policy: Policy,
    timing_enforced: bool,
) -> list[dict[str, object]]:
    baseline_records = measurement_records(baseline)
    candidate_records = measurement_records(candidate)
    if set(baseline_records) != set(candidate_records):
        raise ReportError("measurement record matrices differ")
    metrics: list[dict[str, object]] = []
    for identity in sorted(baseline_records):
        baseline_record = baseline_records[identity]
        candidate_record = candidate_records[identity]
        record_type = identity[3]
        label = "/".join(identity)
        if record_type == "pane_scenario":
            baseline_cpu = finite_number(
                baseline_record.get("cpu_total_median_us"),
                f"{label}/cpu_total_median_us baseline",
                positive=True,
            )
            candidate_cpu = finite_number(
                candidate_record.get("cpu_total_median_us"),
                f"{label}/cpu_total_median_us candidate",
                positive=True,
            )
            metrics.append(
                performance_metric(
                    identity=identity,
                    name="cpu_total_median_us",
                    baseline=baseline_cpu,
                    candidate=candidate_cpu,
                    percent_limit=policy.pane_cpu_percent,
                    absolute_limit=policy.pane_cpu_minimum_us,
                    unit="us",
                    enforced=timing_enforced,
                )
            )
            baseline_frames = record_integer(
                baseline_record.get("measured_frames"),
                f"{label}/measured_frames baseline",
                minimum=1,
            )
            candidate_frames = record_integer(
                candidate_record.get("measured_frames"),
                f"{label}/measured_frames candidate",
                minimum=1,
            )
            baseline_gpu = complete_gpu_value(
                baseline_record,
                "gpu_median_us",
                f"{label}/gpu_median_us baseline",
                expected_samples=baseline_frames,
                slash_samples=True,
            )
            candidate_gpu = complete_gpu_value(
                candidate_record,
                "gpu_median_us",
                f"{label}/gpu_median_us candidate",
                expected_samples=candidate_frames,
                slash_samples=True,
            )
            if baseline_gpu is not None and candidate_gpu is None:
                metrics.append(
                    {
                        "identity": list(identity),
                        "metric": "gpu_median_us",
                        "category": "evidence",
                        "baseline": baseline_gpu,
                        "candidate": None,
                        "enforced": True,
                        "classification": "regression",
                        "reason_code": "gpu_timestamp_loss",
                    }
                )
            elif baseline_gpu is not None and candidate_gpu is not None:
                metrics.append(
                    performance_metric(
                        identity=identity,
                        name="gpu_median_us",
                        baseline=baseline_gpu,
                        candidate=candidate_gpu,
                        percent_limit=policy.pane_gpu_percent,
                        absolute_limit=policy.pane_gpu_minimum_us,
                        unit="us",
                        enforced=timing_enforced,
                    )
                )
            for field in PANE_WORK_FIELDS:
                baseline_value = nonnegative_integer(
                    baseline_record.get(field), f"{label}/{field} baseline"
                )
                candidate_value = nonnegative_integer(
                    candidate_record.get(field), f"{label}/{field} candidate"
                )
                if baseline_value is None or candidate_value is None:
                    raise ReportError(f"{label}/{field} availability differs")
                metrics.append(
                    work_metric(identity, field, baseline_value, candidate_value)
                )
        elif record_type == "shader_scenario" and identity[4] == "retained":
            baseline_cpu = finite_number(
                baseline_record.get("cpu_total_median_us"),
                f"{label}/cpu_total_median_us baseline",
                positive=True,
            )
            candidate_cpu = finite_number(
                candidate_record.get("cpu_total_median_us"),
                f"{label}/cpu_total_median_us candidate",
                positive=True,
            )
            metrics.append(
                performance_metric(
                    identity=identity,
                    name="cpu_total_median_us",
                    baseline=baseline_cpu,
                    candidate=candidate_cpu,
                    percent_limit=policy.shader_cpu_percent,
                    absolute_limit=policy.shader_cpu_minimum_us,
                    unit="us",
                    enforced=timing_enforced,
                )
            )
            baseline_frames = record_integer(
                baseline_record.get("measured_frames"),
                f"{label}/measured_frames baseline",
                minimum=1,
            )
            candidate_frames = record_integer(
                candidate_record.get("measured_frames"),
                f"{label}/measured_frames candidate",
                minimum=1,
            )
            baseline_gpu = complete_gpu_value(
                baseline_record,
                "gpu_median_us",
                f"{label}/gpu_median_us baseline",
                expected_samples=baseline_frames,
                slash_samples=False,
            )
            candidate_gpu = complete_gpu_value(
                candidate_record,
                "gpu_median_us",
                f"{label}/gpu_median_us candidate",
                expected_samples=candidate_frames,
                slash_samples=False,
            )
            if baseline_gpu is not None and candidate_gpu is None:
                metrics.append(
                    {
                        "identity": list(identity),
                        "metric": "gpu_median_us",
                        "category": "evidence",
                        "baseline": baseline_gpu,
                        "candidate": None,
                        "enforced": True,
                        "classification": "regression",
                        "reason_code": "gpu_timestamp_loss",
                    }
                )
            elif baseline_gpu is not None and candidate_gpu is not None:
                metrics.append(
                    performance_metric(
                        identity=identity,
                        name="gpu_median_us",
                        baseline=baseline_gpu,
                        candidate=candidate_gpu,
                        percent_limit=policy.shader_gpu_percent,
                        absolute_limit=policy.shader_gpu_minimum_us,
                        unit="us",
                        enforced=timing_enforced,
                    )
                )
            for metric_name, percent_limit, absolute_limit in (
                (
                    "cpu_total_vs_legacy_ratio",
                    policy.shader_cpu_percent,
                    policy.shader_cpu_minimum_ratio,
                ),
                (
                    "gpu_vs_legacy_ratio",
                    policy.shader_gpu_percent,
                    policy.shader_gpu_minimum_ratio,
                ),
            ):
                baseline_value = optional_number(
                    baseline_record.get(metric_name),
                    f"{label}/{metric_name} baseline",
                )
                candidate_value = optional_number(
                    candidate_record.get(metric_name),
                    f"{label}/{metric_name} candidate",
                )
                if metric_name == "cpu_total_vs_legacy_ratio" and (
                    baseline_value is None or candidate_value is None
                ):
                    raise ReportError(f"{label}/{metric_name} is missing")
                if (
                    metric_name == "gpu_vs_legacy_ratio"
                    and baseline_gpu is not None
                    and candidate_gpu is None
                ):
                    continue
                if baseline_value is None and candidate_value is None:
                    continue
                if baseline_value is not None and candidate_value is None:
                    metrics.append(
                        {
                            "identity": list(identity),
                            "metric": metric_name,
                            "category": "evidence",
                            "baseline": baseline_value,
                            "candidate": None,
                            "enforced": True,
                            "classification": "regression",
                            "reason_code": "shader_ratio_loss",
                        }
                    )
                    continue
                if baseline_value is None or candidate_value is None:
                    continue
                if baseline_value <= 0.0 or candidate_value <= 0.0:
                    raise ReportError(f"{label}/{metric_name} is not positive")
                metrics.append(
                    performance_metric(
                        identity=identity,
                        name=metric_name,
                        baseline=baseline_value,
                        candidate=candidate_value,
                        percent_limit=percent_limit,
                        absolute_limit=absolute_limit,
                        unit="ratio",
                        enforced=timing_enforced,
                    )
                )
            for field in SHADER_WORK_FIELDS:
                baseline_value = nonnegative_integer(
                    baseline_record.get(field), f"{label}/{field} baseline"
                )
                candidate_value = nonnegative_integer(
                    candidate_record.get(field), f"{label}/{field} candidate"
                )
                if baseline_value is None and candidate_value is None:
                    continue
                if baseline_value is None or candidate_value is None:
                    raise ReportError(f"{label}/{field} availability differs")
                metrics.append(
                    work_metric(identity, field, baseline_value, candidate_value)
                )
    return sorted(
        metrics,
        key=lambda metric: ("/".join(metric["identity"]), str(metric["metric"])),
    )


def provenance(report: dict[str, object]) -> dict[str, object]:
    host = report.get("host")
    harness = report.get("harness")
    artifacts = report.get("artifacts")
    return {
        "repository_revision": (
            host.get("repository_revision") if isinstance(host, dict) else None
        ),
        "repository_dirty": (
            host.get("repository_dirty") if isinstance(host, dict) else None
        ),
        "ghostty_revision": (
            host.get("ghostty_revision") if isinstance(host, dict) else None
        ),
        "harness_sha256": harness.get("sha256") if isinstance(harness, dict) else None,
        "artifact_sha256": (
            {
                name: value.get("sha256")
                for name, value in artifacts.items()
                if isinstance(name, str) and isinstance(value, dict)
            }
            if isinstance(artifacts, dict)
            else {}
        ),
    }


def compare_reports(
    baseline: dict[str, object],
    candidate: dict[str, object],
    policy: Policy,
) -> dict[str, object]:
    result: dict[str, object] = {
        "comparison_schema_version": COMPARISON_SCHEMA_VERSION,
        "policy_version": POLICY_VERSION,
        "policy": {
            "timing_mode": policy.timing_mode,
            "pane_cpu_percent": policy.pane_cpu_percent,
            "pane_gpu_percent": policy.pane_gpu_percent,
            "pane_cpu_minimum_us": policy.pane_cpu_minimum_us,
            "pane_gpu_minimum_us": policy.pane_gpu_minimum_us,
            "shader_cpu_percent": policy.shader_cpu_percent,
            "shader_gpu_percent": policy.shader_gpu_percent,
            "shader_cpu_minimum_us": policy.shader_cpu_minimum_us,
            "shader_gpu_minimum_us": policy.shader_gpu_minimum_us,
            "shader_cpu_minimum_ratio": policy.shader_cpu_minimum_ratio,
            "shader_gpu_minimum_ratio": policy.shader_gpu_minimum_ratio,
            "allow_context_changes": policy.allow_context_changes,
        },
        "baseline_provenance": provenance(baseline),
        "candidate_provenance": provenance(candidate),
        "metrics": [],
    }
    result["provenance_changes"] = differences(
        {
            f"provenance.{key}": value
            for key, value in result["baseline_provenance"].items()
        },
        {
            f"provenance.{key}": value
            for key, value in result["candidate_provenance"].items()
        },
    )
    errors = validate_report(baseline, "baseline")
    if candidate.get("status") == "skip":
        if candidate.get("schema_version") != SUPPORTED_QUALIFICATION_SCHEMA:
            errors.append(
                "candidate.schema_version: skipped report uses an unsupported schema"
            )
        candidate_harness = candidate.get("harness")
        if not isinstance(candidate_harness, dict) or not valid_sha256(
            candidate_harness.get("sha256")
        ):
            errors.append("candidate.harness.sha256: missing or malformed")
        if errors:
            result.update(
                {
                    "status": "incompatible",
                    "reason_code": "invalid_report",
                    "errors": errors,
                }
            )
            return result
        result.update(
            {
                "status": "skip",
                "reason_code": "candidate_qualification_skipped",
                "errors": errors,
            }
        )
        return result
    errors.extend(validate_report(candidate, "candidate"))
    if errors:
        result.update(
            {
                "status": "incompatible",
                "reason_code": "invalid_report",
                "errors": errors,
            }
        )
        return result
    try:
        baseline_structural, baseline_environmental = context_projection(baseline)
        candidate_structural, candidate_environmental = context_projection(candidate)
        structural_differences = differences(baseline_structural, candidate_structural)
        context_differences = differences(
            baseline_environmental, candidate_environmental
        )
        result["compatibility"] = {
            "structural_differences": structural_differences,
            "context_differences": context_differences,
        }
        if structural_differences or (
            context_differences and not policy.allow_context_changes
        ):
            result.update(
                {
                    "status": "incompatible",
                    "reason_code": "comparison_context_mismatch",
                }
            )
            return result
        if policy.timing_mode == "enforce":
            timing_enforced = True
        elif policy.timing_mode == "advisory":
            timing_enforced = False
        else:
            timing_enforced = has_enforced_sample_count(
                baseline
            ) and has_enforced_sample_count(candidate)
        result["effective_timing_mode"] = "enforce" if timing_enforced else "advisory"
        metrics = compare_metrics(baseline, candidate, policy, timing_enforced)
    except ReportError as error:
        result.update(
            {
                "status": "incompatible",
                "reason_code": "malformed_measurement_evidence",
                "errors": [str(error)],
            }
        )
        return result
    result["metrics"] = metrics
    regressions = [
        metric for metric in metrics if metric.get("classification") == "regression"
    ]
    advisory = [
        metric
        for metric in metrics
        if metric.get("classification") == "advisory-regression"
    ]
    improvements = [
        metric for metric in metrics if metric.get("classification") == "improvement"
    ]
    result["summary"] = {
        "metrics": len(metrics),
        "regressions": len(regressions),
        "advisory_regressions": len(advisory),
        "improvements": len(improvements),
        "context_changes": len(
            result.get("compatibility", {}).get("context_differences", [])
            if isinstance(result.get("compatibility"), dict)
            else []
        ),
    }
    if regressions:
        result.update({"status": "regression", "reason_code": "regressions_detected"})
    else:
        result.update({"status": "pass", "reason_code": "no_enforced_regressions"})
    return result


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        description="Compare two ghostty-qt renderer qualification reports."
    )
    result.add_argument("baseline", type=Path)
    result.add_argument("candidate", type=Path)
    result.add_argument("--output", type=Path)
    result.add_argument("--json", action="store_true", dest="json_output")
    result.add_argument(
        "--timing-mode", choices=("auto", "advisory", "enforce"), default="auto"
    )
    result.add_argument("--allow-context-changes", action="store_true")
    result.add_argument("--max-pane-cpu-percent", type=float, default=15.0)
    result.add_argument("--max-pane-gpu-percent", type=float, default=15.0)
    result.add_argument("--minimum-pane-cpu-us", type=float, default=100.0)
    result.add_argument("--minimum-pane-gpu-us", type=float, default=50.0)
    result.add_argument("--max-shader-cpu-percent", type=float, default=15.0)
    result.add_argument("--max-shader-gpu-percent", type=float, default=12.0)
    result.add_argument("--minimum-shader-cpu-us", type=float, default=100.0)
    result.add_argument("--minimum-shader-gpu-us", type=float, default=50.0)
    result.add_argument("--minimum-shader-cpu-ratio", type=float, default=0.10)
    result.add_argument("--minimum-shader-gpu-ratio", type=float, default=0.08)
    return result


def nonnegative_option(value: float, name: str) -> float:
    if not math.isfinite(value) or value < 0.0:
        raise ReportError(f"{name} must be a finite nonnegative number")
    return value


def print_human(result: dict[str, object]) -> None:
    status = str(result.get("status", "incompatible")).upper()
    reason = result.get("reason_code", "unknown")
    summary = result.get("summary")
    suffix = ""
    if isinstance(summary, dict):
        suffix = (
            f"; metrics={summary.get('metrics', 0)} "
            f"regressions={summary.get('regressions', 0)} "
            f"advisory={summary.get('advisory_regressions', 0)} "
            f"improvements={summary.get('improvements', 0)}"
        )
    print(f"{status}: {reason}{suffix}")
    metrics = result.get("metrics")
    if not isinstance(metrics, list):
        return
    notable = [
        metric
        for metric in metrics
        if isinstance(metric, dict)
        and metric.get("classification") in ("regression", "advisory-regression")
    ]
    notable.sort(key=lambda metric: float(metric.get("percent", 0.0)), reverse=True)
    for metric in notable[:20]:
        identity = "/".join(str(value) for value in metric.get("identity", []))
        delta = metric.get("delta")
        percent = metric.get("percent")
        detail = (
            f" delta={float(delta):+.2f}" if isinstance(delta, (int, float)) else ""
        )
        if isinstance(percent, (int, float)):
            detail += f" ({float(percent):+.2f}%)"
        if metric.get("reason_code"):
            detail += f" reason={metric['reason_code']}"
        print(
            f"  {metric.get('classification')}: {identity}/"
            f"{metric.get('metric')}{detail}"
        )


def main(arguments: list[str] | None = None) -> int:
    options = parser().parse_args(arguments)
    write_output = True
    try:
        baseline_path = options.baseline.resolve()
        candidate_path = options.candidate.resolve()
        if options.output is not None and options.output.resolve() in (
            baseline_path,
            candidate_path,
        ):
            write_output = False
            raise ReportError("output path must differ from both qualification inputs")
        policy = Policy(
            timing_mode=options.timing_mode,
            pane_cpu_percent=nonnegative_option(
                options.max_pane_cpu_percent, "max pane CPU percent"
            ),
            pane_gpu_percent=nonnegative_option(
                options.max_pane_gpu_percent, "max pane GPU percent"
            ),
            pane_cpu_minimum_us=nonnegative_option(
                options.minimum_pane_cpu_us, "minimum pane CPU microseconds"
            ),
            pane_gpu_minimum_us=nonnegative_option(
                options.minimum_pane_gpu_us, "minimum pane GPU microseconds"
            ),
            shader_cpu_percent=nonnegative_option(
                options.max_shader_cpu_percent, "max shader CPU percent"
            ),
            shader_gpu_percent=nonnegative_option(
                options.max_shader_gpu_percent, "max shader GPU percent"
            ),
            shader_cpu_minimum_us=nonnegative_option(
                options.minimum_shader_cpu_us,
                "minimum shader CPU microseconds",
            ),
            shader_gpu_minimum_us=nonnegative_option(
                options.minimum_shader_gpu_us,
                "minimum shader GPU microseconds",
            ),
            shader_cpu_minimum_ratio=nonnegative_option(
                options.minimum_shader_cpu_ratio, "minimum shader CPU ratio"
            ),
            shader_gpu_minimum_ratio=nonnegative_option(
                options.minimum_shader_gpu_ratio, "minimum shader GPU ratio"
            ),
            allow_context_changes=options.allow_context_changes,
        )
        baseline = load_report(options.baseline)
        candidate = load_report(options.candidate)
        comparison = compare_reports(baseline, candidate, policy)
        comparison["baseline"] = {
            "path": str(options.baseline.resolve()),
            "sha256": file_sha256(options.baseline),
        }
        comparison["candidate"] = {
            "path": str(options.candidate.resolve()),
            "sha256": file_sha256(options.candidate),
        }
        comparator_path = Path(__file__).resolve()
        comparison["comparator"] = {
            "path": str(comparator_path),
            "sha256": file_sha256(comparator_path),
        }
    except ReportError as error:
        comparison = {
            "comparison_schema_version": COMPARISON_SCHEMA_VERSION,
            "policy_version": POLICY_VERSION,
            "status": "incompatible",
            "reason_code": "input_error",
            "errors": [str(error)],
        }
    if options.output is not None and write_output:
        atomic_write_json(options.output, comparison)
    if options.json_output:
        json.dump(comparison, sys.stdout, indent=2, sort_keys=True)
        sys.stdout.write("\n")
    else:
        print_human(comparison)
        if options.output is not None and write_output:
            print(f"comparison={options.output.resolve()}")
    status = comparison.get("status")
    if status == "pass":
        return 0
    if status == "regression":
        return 1
    if status == "skip":
        return 77
    return 2


if __name__ == "__main__":
    sys.exit(main())
