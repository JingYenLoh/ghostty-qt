#!/usr/bin/env python3
"""Qualify ghostty-qt's renderer on a real Wayland host.

The detailed renderer benchmarks use QQuickRenderControl and deliberately do
not present. This runner combines them with the production executable's
frame-swapped test hook so one report contains both deterministic renderer
invariants and evidence from a real native swapchain.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import math
import os
import platform
import re
import shlex
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time
import traceback
from collections import Counter
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from urllib.parse import unquote

SCHEMA_VERSION = 1
SUPPORTED_BACKENDS = ("opengl", "vulkan")
RENDER_ENVIRONMENT_KEYS = (
    "QT_QPA_PLATFORM",
    "QSG_RHI_BACKEND",
    "QT_SCALE_FACTOR",
    "QT_SCALE_FACTOR_ROUNDING_POLICY",
    "WAYLAND_DISPLAY",
    "XDG_CURRENT_DESKTOP",
    "XDG_SESSION_DESKTOP",
    "XDG_SESSION_TYPE",
)
SCRUBBED_RENDER_VARIABLES = (
    "GHOSTTY_QT_CUSTOM_SHADER_PIPELINE",
    "LIBGL_ALWAYS_SOFTWARE",
    "QT_QUICK_BACKEND",
    "QT_SCREEN_SCALE_FACTORS",
    "QSG_INFO",
    "QSG_RHI_PREFER_SOFTWARE_RENDERER",
    "QSG_RENDER_LOOP",
    "QT_OPENGL",
)
DIMENSION_PATTERN = re.compile(r"^(\d+)x(\d+)$")
PANE_GRIDS = ("120x40", "240x80")
SHADER_RENDERERS = ("legacy", "retained")
SHADER_WORKLOADS = ("source-dirty", "effect-only")
SHADER_PASS_COUNTS = ("0", "1", "2", "4", "8")
SOFTWARE_DEVICE_NAME_FRAGMENTS = (
    "lavapipe",
    "llvmpipe",
    "softpipe",
    "software rasterizer",
    "swiftshader",
    "swrast",
)
ACTIVE_REPORT_PATH: Path | None = None
ACTIVE_REPORT: dict[str, object] | None = None


class HarnessTermination(KeyboardInterrupt):
    """Turn a process termination signal into ordinary harness cleanup."""

    def __init__(self, signum: int) -> None:
        super().__init__(signal.Signals(signum).name)
        self.signum = signum


def handle_termination_signal(signum: int, _frame: object) -> None:
    raise HarnessTermination(signum)


@dataclass(frozen=True)
class QualificationProfile:
    scales: tuple[str, ...]
    pane_warmup: int
    pane_iterations: int
    shader_warmup: int
    shader_iterations: int


PROFILES = {
    "quick": QualificationProfile(
        scales=("1", "1.25"),
        pane_warmup=8,
        pane_iterations=12,
        shader_warmup=8,
        shader_iterations=6,
    ),
    "full": QualificationProfile(
        scales=("1", "1.25", "1.5", "2"),
        pane_warmup=200,
        pane_iterations=200,
        shader_warmup=200,
        shader_iterations=100,
    ),
}


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")


def parse_key_value_record(line: str) -> dict[str, str] | None:
    """Parse one stable benchmark/test-hook record.

    Prefix tokens without an equals sign are retained as ``record`` and, for
    pane scenario lines, as ``grid`` and ``scenario``. Values are intentionally
    kept as strings so evidence remains lossless across benchmark revisions.
    """

    try:
        tokens = shlex.split(line)
    except ValueError:
        return None
    if not tokens:
        return None

    record: dict[str, str] = {}
    first_key_value = next(
        (index for index, token in enumerate(tokens) if "=" in token),
        len(tokens),
    )
    prefix = tokens[:first_key_value]
    if prefix:
        if prefix[0] == "renderer_qualification":
            record["record"] = "swapchain"
        elif DIMENSION_PATTERN.fullmatch(prefix[0]) and len(prefix) == 2:
            record["record"] = "pane_scenario"
            record["grid"] = prefix[0]
            record["scenario"] = prefix[1]
        elif DIMENSION_PATTERN.fullmatch(prefix[0]):
            return None
        elif prefix[0] == "baseline":
            record["record"] = "shader_baseline"
        else:
            return None

    for token in tokens[first_key_value:]:
        if "=" not in token:
            return None
        key, value = token.split("=", 1)
        if not key:
            return None
        record[key] = value

    if "record" not in record:
        if "backend" in record and "platform" in record:
            record["record"] = "pane_header"
        elif "grid" in record and "rhi_backend" in record:
            record["record"] = "pane_grid"
        elif "renderer" in record:
            record["record"] = "shader_scenario"
        elif "qt" in record and "graphics_api" in record:
            record["record"] = "shader_header"
        elif "renderdoc_capture" in record:
            record["record"] = "renderdoc"
        else:
            return None
    return record


def parse_records(output: str) -> list[dict[str, str]]:
    return [
        record
        for line in output.splitlines()
        if (record := parse_key_value_record(line)) is not None
    ]


def parse_positive_float(value: str | None) -> float | None:
    if value is None:
        return None
    try:
        parsed = float(value)
    except ValueError:
        return None
    return parsed if math.isfinite(parsed) and parsed > 0.0 else None


def parse_nonnegative_int(value: str | None) -> int | None:
    if value is None:
        return None
    try:
        parsed = int(value)
    except ValueError:
        return None
    return parsed if parsed >= 0 else None


def parse_dimension(value: str | None) -> tuple[int, int] | None:
    if value is None or (match := DIMENSION_PATTERN.fullmatch(value)) is None:
        return None
    result = (int(match.group(1)), int(match.group(2)))
    return result if result[0] > 0 and result[1] > 0 else None


def dimension_matches_dpr(
    logical: tuple[int, int], physical: tuple[int, int], dpr: float
) -> bool:
    return all(
        abs(actual - round(source * dpr)) <= 1
        for source, actual in zip(logical, physical, strict=True)
    )


def text_output(value: str | bytes | None) -> str:
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    return value


def is_software_rhi_device(device_type: str | None, device_name: str | None) -> bool:
    decoded_name = unquote(device_name or "").casefold()
    return device_type == "cpu" or any(
        fragment in decoded_name for fragment in SOFTWARE_DEVICE_NAME_FRAGMENTS
    )


def wayland_socket_path(environment: Mapping[str, str]) -> Path | None:
    display = environment.get("WAYLAND_DISPLAY", "")
    if not display:
        return None
    path = Path(display)
    if path.is_absolute():
        return path
    runtime_directory = environment.get("XDG_RUNTIME_DIR", "")
    return Path(runtime_directory) / path if runtime_directory else None


def validate_wayland_environment(
    environment: Mapping[str, str],
) -> tuple[bool, str, Path | None]:
    if environment.get("XDG_SESSION_TYPE", "").lower() not in ("", "wayland"):
        return False, "not_wayland_session", wayland_socket_path(environment)
    path = wayland_socket_path(environment)
    if path is None:
        return False, "missing_wayland_environment", None
    try:
        mode = path.stat().st_mode
    except OSError:
        return False, "unreachable_wayland_socket", path
    if not stat.S_ISSOCK(mode):
        return False, "invalid_wayland_socket", path
    return True, "ready", path


def qualification_environment(
    base: Mapping[str, str],
    backend: str,
    scale: str | None,
) -> tuple[dict[str, str], dict[str, str]]:
    environment = dict(base)
    for variable in SCRUBBED_RENDER_VARIABLES:
        environment.pop(variable, None)
    environment["QT_QPA_PLATFORM"] = "wayland"
    environment["QSG_RHI_BACKEND"] = backend
    if scale is None:
        environment.pop("QT_SCALE_FACTOR", None)
        environment.pop("QT_SCALE_FACTOR_ROUNDING_POLICY", None)
    else:
        environment["QT_SCALE_FACTOR"] = scale
        environment["QT_SCALE_FACTOR_ROUNDING_POLICY"] = "PassThrough"
    overrides = {
        key: environment[key] for key in RENDER_ENVIRONMENT_KEYS if key in environment
    }
    for variable in SCRUBBED_RENDER_VARIABLES:
        if variable in base:
            overrides[variable] = "<unset>"
    return environment, overrides


def run_command(
    kind: str,
    command: Sequence[str],
    cwd: Path,
    environment: Mapping[str, str],
    environment_overrides: Mapping[str, str] | None = None,
    timeout: float = 120,
) -> dict[str, object]:
    started = time.monotonic()
    result: dict[str, object] = {
        "kind": kind,
        "command": list(command),
        "environment": dict(environment_overrides or {}),
        "started_at": utc_now(),
    }
    print(f"START {kind}: {shlex.join(command)}", flush=True)
    try:
        process = subprocess.Popen(
            command,
            cwd=cwd,
            env=dict(environment),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            start_new_session=True,
        )
    except OSError as error:
        result.update(
            {
                "status": "fail",
                "reason_code": "launch_failed",
                "return_code": None,
                "stdout": "",
                "stderr": str(error),
            }
        )
    else:
        try:
            stdout, stderr = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired as error:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                stdout, stderr = process.communicate(timeout=2)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                try:
                    stdout, stderr = process.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    if process.stdout is not None:
                        process.stdout.close()
                    if process.stderr is not None:
                        process.stderr.close()
                    process.wait(timeout=2)
                    stdout, stderr = error.stdout, error.stderr
            result.update(
                {
                    "status": "fail",
                    "reason_code": "timeout",
                    "return_code": process.returncode,
                    "stdout": text_output(stdout),
                    "stderr": text_output(stderr),
                }
            )
        except BaseException:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                pass
            raise
        else:
            result.update(
                {
                    "status": "pending",
                    "reason_code": "unclassified",
                    "return_code": process.returncode,
                    "stdout": stdout,
                    "stderr": stderr,
                    "records": parse_records(stdout),
                }
            )
    result["duration_ms"] = round((time.monotonic() - started) * 1000.0, 2)
    result["completed_at"] = utc_now()
    print(
        f"DONE {kind}: return_code={result.get('return_code')} "
        f"duration_ms={result['duration_ms']}",
        flush=True,
    )
    return result


def mark_run(run: dict[str, object], status: str, reason_code: str) -> None:
    run["status"] = status
    run["reason_code"] = reason_code


def records_of(run: Mapping[str, object], record_type: str) -> list[dict[str, str]]:
    records = run.get("records", [])
    if not isinstance(records, list):
        return []
    return [
        record
        for record in records
        if isinstance(record, dict) and record.get("record") == record_type
    ]


def validate_pane_run(
    run: dict[str, object],
    backend: str,
    scenarios: Sequence[str],
    expected_iterations: int,
    allow_software_device: bool = False,
) -> float | None:
    return_code = run.get("return_code")
    if return_code == 77:
        mark_run(run, "skip", "unsupported_backend")
        return None
    if return_code != 0:
        diagnostic = "\n".join((str(run.get("stdout", "")), str(run.get("stderr", ""))))
        reason = (
            "backend_fallback"
            if "but Qt selected graphics API" in diagnostic
            else "benchmark_failed"
        )
        mark_run(run, "fail", reason)
        return None
    headers = records_of(run, "pane_header")
    grid_headers = records_of(run, "pane_grid")
    scenario_records = records_of(run, "pane_scenario")
    if len(headers) != 1 or len(grid_headers) != len(PANE_GRIDS):
        mark_run(run, "fail", "invalid_pane_metadata")
        return None
    header = headers[0]
    if (
        header.get("platform") != "wayland"
        or header.get("presentation") != "offscreen"
        or not header.get("qt_version")
    ):
        mark_run(run, "fail", "wrong_platform")
        return None
    if header.get("backend") != backend:
        mark_run(run, "fail", "backend_fallback")
        return None

    expected_pairs = Counter(
        (grid, scenario) for grid in PANE_GRIDS for scenario in scenarios
    )
    actual_pairs = Counter(
        (record.get("grid"), record.get("scenario")) for record in scenario_records
    )
    if actual_pairs != expected_pairs or any(
        parse_nonnegative_int(record.get("measured_frames")) != expected_iterations
        for record in scenario_records
    ):
        mark_run(run, "fail", "incomplete_scenario_matrix")
        return None

    if Counter(record.get("grid") for record in grid_headers) != Counter(PANE_GRIDS):
        mark_run(run, "fail", "invalid_pane_metadata")
        return None
    device_signatures: set[tuple[str | None, ...]] = set()
    for item in grid_headers:
        dpr = parse_positive_float(item.get("dpr"))
        logical = parse_dimension(item.get("logical"))
        framebuffer = parse_dimension(item.get("framebuffer"))
        device_signature = tuple(
            item.get(key)
            for key in (
                "rhi_backend",
                "rhi_device_name",
                "rhi_device_type",
                "rhi_vendor_id",
                "rhi_device_id",
            )
        )
        if (
            dpr is None
            or logical is None
            or framebuffer is None
            or not dimension_matches_dpr(logical, framebuffer, dpr)
            or any(not value for value in device_signature)
        ):
            mark_run(run, "fail", "invalid_pane_metadata")
            return None
        device_signatures.add(device_signature)
    if len(device_signatures) != 1:
        mark_run(run, "fail", "inconsistent_rhi_device")
        return None
    device_type = next(iter(device_signatures))[2]
    device_name = next(iter(device_signatures))[1]
    run["rhi_device_type"] = device_type
    run["rhi_device_name"] = unquote(device_name or "")
    run["rhi_device"] = {
        key: grid_headers[0].get(key)
        for key in (
            "rhi_backend",
            "rhi_device_name",
            "rhi_device_type",
            "rhi_vendor_id",
            "rhi_device_id",
        )
    }
    if is_software_rhi_device(device_type, device_name) and not allow_software_device:
        mark_run(run, "fail", "software_rhi_device")
        return None

    dprs = {parse_positive_float(item.get("dpr")) for item in grid_headers}
    dprs.discard(None)
    if len(dprs) != 1:
        mark_run(run, "fail", "invalid_dpr_metadata")
        return None
    mark_run(run, "pass", "completed")
    return next(iter(dprs))


def validate_shader_run(
    run: dict[str, object],
    backend: str,
    expected_iterations: int,
    allow_software_device: bool = False,
) -> float | None:
    return_code = run.get("return_code")
    if return_code == 3:
        mark_run(run, "skip", "unsupported_backend")
        return None
    if return_code != 0:
        reason = (
            "backend_fallback"
            if "was requested, but Qt selected" in str(run.get("stderr", ""))
            else "shader_benchmark_failed"
        )
        mark_run(run, "fail", reason)
        return None
    headers = records_of(run, "shader_header")
    scenario_records = records_of(run, "shader_scenario")
    baselines = records_of(run, "shader_baseline")
    if len(headers) != 1:
        mark_run(run, "fail", "invalid_shader_metadata")
        return None
    header = headers[0]
    dpr = parse_positive_float(header.get("dpr"))
    viewport = parse_dimension(header.get("viewport"))
    framebuffer = parse_dimension(header.get("framebuffer"))
    if (
        header.get("platform") != "wayland"
        or not header.get("qt")
        or not header.get("rhi_backend")
        or header.get("completion") != "offscreen-end-frame"
        or dpr is None
        or viewport is None
        or framebuffer is None
        or not dimension_matches_dpr(viewport, framebuffer, dpr)
    ):
        mark_run(run, "fail", "wrong_platform")
        return None
    if header.get("graphics_api") != backend:
        mark_run(run, "fail", "backend_fallback")
        return None
    device = {
        key: header.get(key)
        for key in (
            "rhi_backend",
            "rhi_device_name",
            "rhi_device_type",
            "rhi_vendor_id",
            "rhi_device_id",
        )
    }
    if any(not value for value in device.values()):
        mark_run(run, "fail", "invalid_shader_metadata")
        return None
    if (
        is_software_rhi_device(
            str(device["rhi_device_type"]), str(device["rhi_device_name"])
        )
        and not allow_software_device
    ):
        mark_run(run, "fail", "software_rhi_device")
        return None
    run["rhi_device"] = device

    expected_matrix = Counter(
        (renderer, workload, passes)
        for renderer in SHADER_RENDERERS
        for workload in SHADER_WORKLOADS
        for passes in SHADER_PASS_COUNTS
    )
    actual_matrix = Counter(
        (
            record.get("renderer"),
            record.get("workload"),
            record.get("passes"),
        )
        for record in scenario_records
    )
    baseline_workloads = Counter(record.get("workload") for record in baselines)
    if (
        actual_matrix != expected_matrix
        or baseline_workloads != Counter(SHADER_WORKLOADS)
        or any(
            parse_nonnegative_int(record.get("measured_frames")) != expected_iterations
            for record in scenario_records
        )
    ):
        mark_run(run, "fail", "incomplete_shader_matrix")
        return None
    mark_run(run, "pass", "completed")
    return dpr


def validate_swapchain_run(
    run: dict[str, object],
    backend: str,
    expected_frames: int,
    allow_software_device: bool = False,
    expected_device: Mapping[str, object] | None = None,
) -> float | None:
    if run.get("return_code") == 77:
        mark_run(run, "skip", "unsupported_swapchain")
        return None
    if run.get("return_code") != 0:
        mark_run(run, "fail", "swapchain_probe_failed")
        return None
    records = records_of(run, "swapchain")
    if len(records) != 1:
        mark_run(run, "fail", "invalid_swapchain_metadata")
        return None
    record = records[0]
    if record.get("platform") != "wayland":
        mark_run(run, "fail", "wrong_platform")
        return None
    if record.get("graphics_api") != backend:
        mark_run(run, "fail", "backend_fallback")
        return None
    frame_swaps = parse_nonnegative_int(record.get("frame_swaps"))
    alpha_bits = parse_nonnegative_int(record.get("alpha_buffer_bits"))
    clear_alpha = parse_nonnegative_int(record.get("clear_alpha"))
    image_alpha = parse_nonnegative_int(record.get("image_alpha"))
    nonuniform = parse_nonnegative_int(record.get("nonuniform"))
    minimum_alpha = parse_nonnegative_int(record.get("min_alpha"))
    maximum_alpha = parse_nonnegative_int(record.get("max_alpha"))
    translucent_pixels = parse_nonnegative_int(record.get("translucent_pixels"))
    half_alpha_pixels = parse_nonnegative_int(record.get("half_alpha_pixels"))
    pixel_count = parse_nonnegative_int(record.get("pixel_count"))
    minimum_half_alpha_pixels = parse_nonnegative_int(
        record.get("minimum_half_alpha_pixels")
    )
    panes = parse_nonnegative_int(record.get("panes"))
    running_panes = parse_nonnegative_int(record.get("running_panes"))
    dpr = parse_positive_float(record.get("dpr"))
    logical = parse_dimension(record.get("logical"))
    physical = parse_dimension(record.get("physical"))
    median_interval = parse_positive_float(record.get("median_frame_interval_us"))
    p90_interval = parse_positive_float(record.get("p90_frame_interval_us"))
    expected_pixel_count = physical[0] * physical[1] if physical is not None else None
    expected_half_alpha_pixels = (
        max(1, expected_pixel_count // 20) if expected_pixel_count is not None else None
    )
    alpha_counts_valid = (
        expected_pixel_count is not None
        and expected_half_alpha_pixels is not None
        and pixel_count == expected_pixel_count
        and minimum_half_alpha_pixels == expected_half_alpha_pixels
        and translucent_pixels is not None
        and 0 < translucent_pixels <= expected_pixel_count
        and half_alpha_pixels is not None
        and expected_half_alpha_pixels <= half_alpha_pixels <= translucent_pixels
    )
    if (
        frame_swaps is None
        or frame_swaps < expected_frames
        or alpha_bits is None
        or alpha_bits == 0
        or clear_alpha != 0
        or image_alpha != 1
        or nonuniform != 1
        or minimum_alpha is None
        or minimum_alpha >= 255
        or maximum_alpha is None
        or maximum_alpha > 255
        or minimum_alpha > maximum_alpha
        or not alpha_counts_valid
        or panes is None
        or panes == 0
        or running_panes is None
        or running_panes == 0
        or dpr is None
        or logical is None
        or physical is None
        or not dimension_matches_dpr(logical, physical, dpr)
        or median_interval is None
        or p90_interval is None
    ):
        mark_run(run, "fail", "invalid_presented_surface")
        return None
    device = {
        key: record.get(key)
        for key in (
            "rhi_backend",
            "rhi_device_name",
            "rhi_device_type",
            "rhi_vendor_id",
            "rhi_device_id",
        )
    }
    if any(not value for value in device.values()):
        mark_run(run, "fail", "invalid_rhi_device")
        return None
    if (
        is_software_rhi_device(
            str(device["rhi_device_type"]), str(device["rhi_device_name"])
        )
        and not allow_software_device
    ):
        mark_run(run, "fail", "software_rhi_device")
        return None
    if expected_device is not None and device != dict(expected_device):
        run["expected_rhi_device"] = dict(expected_device)
        run["actual_rhi_device"] = device
        mark_run(run, "fail", "inconsistent_production_device")
        return None
    run["rhi_device"] = device
    mark_run(run, "pass", "completed")
    return dpr


def validate_scale_ratios(
    runs: Iterable[tuple[str, dict[str, object], float | None]],
) -> None:
    successful = [
        (float(scale), run, dpr)
        for scale, run, dpr in runs
        if run.get("status") == "pass" and dpr is not None
    ]
    base = next((entry for entry in successful if entry[0] == 1.0), None)
    if base is None:
        return
    base_dpr = base[2]
    assert base_dpr is not None
    for scale, run, dpr in successful:
        assert dpr is not None
        expected = base_dpr * scale
        if not math.isclose(dpr, expected, rel_tol=0.01, abs_tol=0.02):
            mark_run(run, "fail", "dpr_scale_mismatch")
            run["expected_dpr"] = expected
            run["actual_dpr"] = dpr


def validate_scale_devices(
    runs: Iterable[tuple[str, dict[str, object], float | None]],
) -> dict[str, object] | None:
    successful = [
        run
        for _, run, _ in runs
        if run.get("status") == "pass" and isinstance(run.get("rhi_device"), Mapping)
    ]
    if not successful:
        return None
    expected = dict(successful[0]["rhi_device"])
    consistent = True
    for run in successful[1:]:
        actual = dict(run["rhi_device"])
        if actual == expected:
            continue
        run["expected_rhi_device"] = expected
        run["actual_rhi_device"] = actual
        mark_run(run, "fail", "inconsistent_scale_rhi_device")
        consistent = False
    return expected if consistent else None


def git_output(root: Path, *arguments: str) -> str | None:
    try:
        completed = subprocess.run(
            ("git", *arguments),
            cwd=root,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            timeout=10,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    return completed.stdout.strip() if completed.returncode == 0 else None


def host_metadata(root: Path, environment: Mapping[str, str]) -> dict[str, object]:
    status = git_output(root, "status", "--porcelain")
    return {
        "uname": list(platform.uname()),
        "python": platform.python_version(),
        "processors": os.cpu_count(),
        "repository_revision": git_output(root, "rev-parse", "HEAD"),
        "repository_dirty": bool(status) if status is not None else None,
        "ghostty_revision": git_output(root / "ghostty", "rev-parse", "HEAD"),
        "environment": {
            key: environment[key]
            for key in (
                "WAYLAND_DISPLAY",
                "XDG_CURRENT_DESKTOP",
                "XDG_SESSION_DESKTOP",
                "XDG_SESSION_TYPE",
            )
            if key in environment
        },
    }


def file_metadata(path: Path) -> dict[str, object]:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    status = path.stat()
    return {
        "path": str(path),
        "sha256": digest.hexdigest(),
        "size": status.st_size,
        "mtime_ns": status.st_mtime_ns,
    }


def atomic_write_json(path: Path, value: Mapping[str, object]) -> None:
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


def finalize_active_report(
    status: str, reason_code: str, diagnostic: str
) -> Path | None:
    if ACTIVE_REPORT_PATH is None or ACTIVE_REPORT is None:
        return None
    ACTIVE_REPORT.update(
        {
            "status": status,
            "reason_code": reason_code,
            "diagnostic": diagnostic,
            "completed_at": utc_now(),
        }
    )
    try:
        atomic_write_json(ACTIVE_REPORT_PATH, ACTIVE_REPORT)
    except OSError:
        return None
    return ACTIVE_REPORT_PATH


def finish_termination(error: HarnessTermination) -> int:
    signal_name = signal.Signals(error.signum).name
    path = finalize_active_report("aborted", "terminated", signal_name)
    print(
        f"ABORTED: terminated by {signal_name}; report={path or 'unavailable'}",
        file=sys.stderr,
    )
    return 128 + error.signum


def default_report_directory(root: Path) -> Path:
    timestamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%d-%H%M%S-%f")
    revision = git_output(root, "rev-parse", "--short=10", "HEAD") or "unknown"
    return (
        root
        / "tmp"
        / "renderer-qualification"
        / f"{timestamp}-{revision}-p{os.getpid()}"
    )


def nproc() -> int:
    try:
        output = subprocess.check_output(
            ("nproc",), text=True, stderr=subprocess.DEVNULL, timeout=5
        ).strip()
        count = int(output)
    except (OSError, ValueError, subprocess.SubprocessError):
        count = os.cpu_count() or 1
    return max(1, count)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(
        description=(
            "Run offscreen renderer invariants and a real ghostty-qt Wayland "
            "swapchain probe, then write one atomic JSON evidence report."
        )
    )
    result.add_argument("--profile", choices=tuple(PROFILES), default="quick")
    result.add_argument(
        "--backend",
        action="append",
        choices=SUPPORTED_BACKENDS,
        dest="backends",
        help="Backend to exercise; repeat to select both (default: both).",
    )
    result.add_argument(
        "--require-backend",
        action="append",
        choices=SUPPORTED_BACKENDS,
        default=[],
        help="Treat an unavailable selected backend as failure.",
    )
    result.add_argument(
        "--scale",
        action="append",
        help="Synthetic global scale for offscreen checks; repeat as needed.",
    )
    result.add_argument("--pane-warmup", type=int)
    result.add_argument("--pane-iterations", type=int)
    result.add_argument("--shader-warmup", type=int)
    result.add_argument("--shader-iterations", type=int)
    result.add_argument("--skip-build", action="store_true")
    result.add_argument(
        "--allow-software-device",
        action="store_true",
        help=(
            "Allow QRhi CPU devices such as llvmpipe/lavapipe; by default "
            "they fail production-GPU qualification."
        ),
    )
    result.add_argument(
        "--output-directory",
        type=Path,
        help="Report directory (default: tmp/renderer-qualification/<run>).",
    )
    result.add_argument(
        "--renderdoc-scenario",
        action="append",
        default=[],
        help="Optionally capture a pane scenario after a backend passes.",
    )
    result.add_argument(
        "--require-renderdoc",
        action="store_true",
        help="Fail instead of skip when renderdoccmd is unavailable.",
    )
    return result


def resolved_positive(override: int | None, fallback: int, name: str) -> int:
    result = fallback if override is None else override
    if result <= 0:
        raise ValueError(f"{name} must be a positive integer")
    return result


def resolved_scales(
    values: Sequence[str] | None, profile: QualificationProfile
) -> list[str]:
    result = list(profile.scales if not values else values)
    normalized: list[str] = []
    for value in result:
        parsed = parse_positive_float(value)
        if parsed is None:
            raise ValueError(f"invalid positive scale: {value}")
        canonical = format(parsed, ".12g")
        if canonical not in normalized:
            normalized.append(canonical)
    if "1" not in normalized:
        normalized.insert(0, "1")
    return normalized


def discover_scenarios(
    executable: Path, root: Path, environment: Mapping[str, str]
) -> tuple[list[str], dict[str, object]]:
    run = run_command(
        "scenario_discovery",
        (str(executable), "--list-scenarios"),
        root,
        environment,
        timeout=30,
    )
    if run.get("return_code") != 0:
        mark_run(run, "fail", "scenario_discovery_failed")
        return [], run
    scenarios = [
        line.strip() for line in str(run.get("stdout", "")).splitlines() if line.strip()
    ]
    if not scenarios or len(set(scenarios)) != len(scenarios):
        mark_run(run, "fail", "invalid_scenario_catalog")
        return [], run
    mark_run(run, "pass", "completed")
    run["scenarios"] = scenarios
    return scenarios, run


def main(arguments: Sequence[str] | None = None) -> int:
    global ACTIVE_REPORT, ACTIVE_REPORT_PATH
    options = parser().parse_args(arguments)
    root = Path(__file__).resolve().parents[1]
    profile = PROFILES[options.profile]
    try:
        scales = resolved_scales(options.scale, profile)
        pane_warmup = resolved_positive(
            options.pane_warmup, profile.pane_warmup, "pane warmup"
        )
        pane_iterations = resolved_positive(
            options.pane_iterations, profile.pane_iterations, "pane iterations"
        )
        shader_warmup = resolved_positive(
            options.shader_warmup, profile.shader_warmup, "shader warmup"
        )
        shader_iterations = resolved_positive(
            options.shader_iterations,
            profile.shader_iterations,
            "shader iterations",
        )
    except ValueError as error:
        parser().error(str(error))

    backends = list(dict.fromkeys(options.backends or SUPPORTED_BACKENDS))
    required_backends = set(options.require_backend)
    if not required_backends.issubset(backends):
        parser().error("every required backend must also be selected with --backend")

    report_directory = (
        options.output_directory.resolve()
        if options.output_directory
        else default_report_directory(root)
    )
    report_path = report_directory / "results.json"
    report: dict[str, object] = {
        "schema_version": SCHEMA_VERSION,
        "started_at": utc_now(),
        "completed_at": None,
        "status": "running",
        "reason_code": "in_progress",
        "host": host_metadata(root, os.environ),
        "harness": file_metadata(Path(__file__).resolve()),
        "configuration": {
            "profile": options.profile,
            "backends": backends,
            "required_backends": sorted(required_backends),
            "synthetic_global_scales": scales,
            "pane_warmup": pane_warmup,
            "pane_iterations": pane_iterations,
            "shader_warmup": shader_warmup,
            "shader_iterations": shader_iterations,
            "renderdoc_scenarios": options.renderdoc_scenario,
            "allow_software_device": options.allow_software_device,
        },
        "runs": [],
    }
    ACTIVE_REPORT_PATH = report_path
    ACTIVE_REPORT = report
    atomic_write_json(report_path, report)

    ready, reason, socket_path = validate_wayland_environment(os.environ)
    report["wayland_socket"] = str(socket_path) if socket_path else None
    if not ready:
        report.update(
            {
                "status": "skip",
                "reason_code": reason,
                "completed_at": utc_now(),
            }
        )
        atomic_write_json(report_path, report)
        print(f"SKIP: {reason}; report={report_path}")
        return 77

    runs = report["runs"]
    assert isinstance(runs, list)
    base_environment = dict(os.environ)
    build_environment = dict(base_environment)
    build_commands = (
        (
            "cmake",
            "--preset",
            "release",
            "-DGHOSTTY_QT_BUILD_RENDER_BENCHMARKS=ON",
        ),
        (
            "cmake",
            "--build",
            "--preset",
            "release",
            "--target",
            "ghostty-qt",
            "bench-terminal-pane-renderer",
            "bench-terminal-custom-shader-rhi",
            f"-j{nproc()}",
        ),
    )
    if not options.skip_build:
        for index, command in enumerate(build_commands):
            run = run_command(
                "configure" if index == 0 else "build",
                command,
                root,
                build_environment,
                timeout=900,
            )
            runs.append(run)
            if run.get("return_code") != 0:
                mark_run(
                    run, "fail", "configure_failed" if index == 0 else "build_failed"
                )
                report.update(
                    {
                        "status": "fail",
                        "reason_code": str(run["reason_code"]),
                        "completed_at": utc_now(),
                    }
                )
                atomic_write_json(report_path, report)
                print(f"FAIL: {run['reason_code']}; report={report_path}")
                return 1
            mark_run(run, "pass", "completed")
            atomic_write_json(report_path, report)

    pane_executable = root / "build/release/tests/bench-terminal-pane-renderer"
    shader_executable = root / "build/release/tests/bench-terminal-custom-shader-rhi"
    application_executable = root / "build/release/ghostty-qt"
    missing = [
        str(path)
        for path in (pane_executable, shader_executable, application_executable)
        if not path.is_file() or not os.access(path, os.X_OK)
    ]
    if missing:
        report.update(
            {
                "status": "fail",
                "reason_code": "missing_executable",
                "missing_executables": missing,
                "completed_at": utc_now(),
            }
        )
        atomic_write_json(report_path, report)
        print(f"FAIL: missing executable; report={report_path}")
        return 1
    report["artifacts"] = {
        path.name: file_metadata(path)
        for path in (
            pane_executable,
            shader_executable,
            application_executable,
        )
    }
    atomic_write_json(report_path, report)

    discovery_environment, _ = qualification_environment(
        base_environment, backends[0], "1"
    )
    scenarios, discovery_run = discover_scenarios(
        pane_executable, root, discovery_environment
    )
    runs.append(discovery_run)
    if not scenarios:
        report.update(
            {
                "status": "fail",
                "reason_code": "scenario_discovery_failed",
                "completed_at": utc_now(),
            }
        )
        atomic_write_json(report_path, report)
        print(f"FAIL: scenario discovery; report={report_path}")
        return 1
    unknown_capture_scenarios = sorted(
        set(options.renderdoc_scenario).difference(scenarios)
    )
    if unknown_capture_scenarios:
        report.update(
            {
                "status": "fail",
                "reason_code": "unknown_renderdoc_scenario",
                "unknown_renderdoc_scenarios": unknown_capture_scenarios,
                "completed_at": utc_now(),
            }
        )
        atomic_write_json(report_path, report)
        print(f"FAIL: unknown RenderDoc scenario; report={report_path}")
        return 1
    atomic_write_json(report_path, report)

    backend_available: dict[str, bool] = {}
    for backend in backends:
        pane_scale_runs: list[tuple[str, dict[str, object], float | None]] = []
        shader_scale_runs: list[tuple[str, dict[str, object], float | None]] = []
        backend_available[backend] = True
        for scale_index, scale in enumerate(scales):
            environment, overrides = qualification_environment(
                base_environment, backend, scale
            )
            pane_run = run_command(
                "pane_offscreen",
                (
                    str(pane_executable),
                    "--graphics-api",
                    backend,
                    "--warmup",
                    str(pane_warmup),
                    "--iterations",
                    str(pane_iterations),
                ),
                root,
                environment,
                overrides,
                timeout=900,
            )
            pane_run.update({"backend": backend, "scale": scale})
            pane_dpr = validate_pane_run(
                pane_run,
                backend,
                scenarios,
                pane_iterations,
                options.allow_software_device,
            )
            runs.append(pane_run)
            pane_scale_runs.append((scale, pane_run, pane_dpr))
            atomic_write_json(report_path, report)
            if pane_run.get("status") == "skip" and scale_index == 0:
                backend_available[backend] = False
                break
            if pane_run.get("status") == "skip":
                mark_run(pane_run, "fail", "scaled_backend_unavailable")
                atomic_write_json(report_path, report)
                break
            if pane_run.get("status") != "pass":
                break

            shader_run = run_command(
                "custom_shader_offscreen",
                (
                    str(shader_executable),
                    "--graphics-api",
                    backend,
                    "--warmup",
                    str(shader_warmup),
                    "--iterations",
                    str(shader_iterations),
                ),
                root,
                environment,
                overrides,
                timeout=900,
            )
            shader_run.update({"backend": backend, "scale": scale})
            shader_dpr = validate_shader_run(
                shader_run,
                backend,
                shader_iterations,
                options.allow_software_device,
            )
            runs.append(shader_run)
            shader_scale_runs.append((scale, shader_run, shader_dpr))
            atomic_write_json(report_path, report)
            if shader_run.get("status") == "skip" and scale_index == 0:
                backend_available[backend] = False
                break
            if shader_run.get("status") == "skip":
                mark_run(shader_run, "fail", "scaled_backend_unavailable")
                atomic_write_json(report_path, report)
                break
            if shader_run.get("status") != "pass":
                break

        validate_scale_ratios(pane_scale_runs)
        validate_scale_ratios(shader_scale_runs)
        expected_device = validate_scale_devices((*pane_scale_runs, *shader_scale_runs))
        atomic_write_json(report_path, report)
        if not backend_available[backend] or any(
            run.get("status") == "fail"
            for _, run, _ in (*pane_scale_runs, *shader_scale_runs)
        ):
            continue

        swapchain_environment, swapchain_overrides = qualification_environment(
            base_environment, backend, None
        )
        isolated_root = report_directory / "xdg" / backend
        for kind in ("config", "cache", "data", "state"):
            path = isolated_root / kind
            path.mkdir(parents=True, exist_ok=True)
            variable = f"XDG_{kind.upper()}_HOME"
            swapchain_environment[variable] = str(path)
            swapchain_overrides[variable] = str(path)
        ghostty_config = isolated_root / "config" / "ghostty" / "config"
        ghostty_config.parent.mkdir(parents=True, exist_ok=True)
        ghostty_config.write_text(
            "background = 102030\n"
            "foreground = ffffff\n"
            "background-opacity = 0.5\n"
            "background-opacity-cells = true\n",
            encoding="utf-8",
        )
        swapchain_environment["GHOSTTY_QT_TEST_RENDERER_QUALIFICATION"] = "1"
        swapchain_environment["GHOSTTY_QT_TEST_RENDERER_QUALIFICATION_FRAMES"] = "30"
        swapchain_environment["GHOSTTY_QT_TEST_RENDERER_EXPECT_TRANSLUCENT"] = "1"
        swapchain_overrides["GHOSTTY_QT_TEST_RENDERER_QUALIFICATION"] = "1"
        swapchain_overrides["GHOSTTY_QT_TEST_RENDERER_QUALIFICATION_FRAMES"] = "30"
        swapchain_overrides["GHOSTTY_QT_TEST_RENDERER_EXPECT_TRANSLUCENT"] = "1"
        application_class = (
            f"io.github.JingYenLoh.ghostty_qt.qualification.p{os.getpid()}.{backend}"
        )
        sentinel_command = (
            "printf '\\033[2J\\033[H\\033[48;2;192;32;64m"
            + "\\033[38;2;255;255;255m GHOSTTY-QT RENDERER "
            + "QUALIFICATION \\033[0m\\r\\n'; sleep 30"
        )
        swapchain_run = run_command(
            "production_swapchain",
            (
                str(application_executable),
                "--single-instance=false",
                f"--class={application_class}",
                "--",
                "/bin/sh",
                "-c",
                sentinel_command,
            ),
            root,
            swapchain_environment,
            swapchain_overrides,
            timeout=30,
        )
        swapchain_run["backend"] = backend
        validate_swapchain_run(
            swapchain_run,
            backend,
            30,
            options.allow_software_device,
            expected_device if isinstance(expected_device, Mapping) else None,
        )
        runs.append(swapchain_run)
        if swapchain_run.get("status") == "skip":
            backend_available[backend] = False
        atomic_write_json(report_path, report)

    if options.renderdoc_scenario:
        renderdoc = shutil.which("renderdoccmd")
        if renderdoc is None:
            renderdoc_run: dict[str, object] = {
                "kind": "renderdoc",
                "status": "fail" if options.require_renderdoc else "skip",
                "reason_code": "missing_renderdoc",
                "return_code": None,
                "scenarios": list(options.renderdoc_scenario),
            }
            runs.append(renderdoc_run)
        else:
            capture_script = root / "scripts/capture-terminal-pane-renderdoc.sh"
            for backend in backends:
                if not backend_available.get(backend, False):
                    continue
                backend_failed = any(
                    run.get("backend") == backend and run.get("status") == "fail"
                    for run in runs
                )
                if backend_failed:
                    continue
                for scenario in options.renderdoc_scenario:
                    environment, overrides = qualification_environment(
                        base_environment, backend, None
                    )
                    capture_run = run_command(
                        "renderdoc",
                        (str(capture_script), backend, scenario),
                        root,
                        environment,
                        overrides,
                        timeout=900,
                    )
                    capture_run.update({"backend": backend, "scenario": scenario})
                    mark_run(
                        capture_run,
                        "pass" if capture_run.get("return_code") == 0 else "fail",
                        (
                            "completed"
                            if capture_run.get("return_code") == 0
                            else "renderdoc_capture_failed"
                        ),
                    )
                    runs.append(capture_run)
                    atomic_write_json(report_path, report)

    failures = [run for run in runs if run.get("status") == "fail"]
    passed_backends = [
        backend
        for backend in backends
        if any(
            run.get("kind") == "production_swapchain"
            and run.get("backend") == backend
            and run.get("status") == "pass"
            for run in runs
        )
    ]
    skipped_backends = [
        backend for backend in backends if not backend_available.get(backend, False)
    ]
    unavailable_required = sorted(required_backends.intersection(skipped_backends))
    if unavailable_required:
        failures.append(
            {
                "kind": "required_backend",
                "status": "fail",
                "reason_code": "required_backend_unavailable",
                "backends": unavailable_required,
            }
        )
        runs.extend(failures[-1:])

    report["summary"] = {
        "passed_backends": passed_backends,
        "skipped_backends": skipped_backends,
        "pass_runs": sum(run.get("status") == "pass" for run in runs),
        "skip_runs": sum(run.get("status") == "skip" for run in runs),
        "fail_runs": sum(run.get("status") == "fail" for run in runs),
    }
    report["completed_at"] = utc_now()
    if failures:
        report["status"] = "fail"
        report["reason_code"] = "qualification_failed"
        exit_code = 1
    elif not passed_backends:
        report["status"] = "skip"
        report["reason_code"] = "no_supported_backend"
        exit_code = 77
    else:
        report["status"] = "pass"
        report["reason_code"] = "completed"
        exit_code = 0
    atomic_write_json(report_path, report)
    print(
        f"{report['status'].upper()}: {report['reason_code']}; " f"report={report_path}"
    )
    return exit_code


if __name__ == "__main__":
    signal.signal(signal.SIGHUP, handle_termination_signal)
    signal.signal(signal.SIGTERM, handle_termination_signal)
    try:
        sys.exit(main())
    except HarnessTermination as error:
        sys.exit(finish_termination(error))
    except KeyboardInterrupt:
        path = finalize_active_report("aborted", "interrupted", "KeyboardInterrupt")
        print(f"ABORTED: interrupted; report={path or 'unavailable'}", file=sys.stderr)
        sys.exit(130)
    except Exception as error:  # noqa: BLE001  # pragma: no cover
        diagnostic = traceback.format_exc()
        path = finalize_active_report("fail", "harness_error", diagnostic)
        print(
            f"FAIL: harness_error ({type(error).__name__}: {error}); "
            f"report={path or 'unavailable'}",
            file=sys.stderr,
        )
        sys.exit(1)
