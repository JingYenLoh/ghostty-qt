from __future__ import annotations

import copy
import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path
from urllib.parse import quote, unquote

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "compare-renderer-qualification.py"
SPEC = importlib.util.spec_from_file_location("compare_renderer_qualification", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
comparison = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = comparison
SPEC.loader.exec_module(comparison)


DEVICE = {
    "rhi_backend": "Vulkan",
    "rhi_device_name": "Test%20GPU",
    "rhi_device_type": "integrated",
    "rhi_vendor_id": "0x1002",
    "rhi_device_id": "0x1234",
}


def graphics_library_record_fields(
    libraries: list[dict[str, object]] | None = None,
    *,
    backend: str = "vulkan",
    status: str = "complete",
    diagnostic: str | None = None,
) -> dict[str, str]:
    effective_libraries = (
        [
            {
                "role": "driver",
                "name": "libvulkan_radeon.so",
                "path_kind": "system",
                "size": 123456,
                "sha256": "1" * 64,
            }
        ]
        if libraries is None
        else libraries
    )
    manifest = {
        "schema_version": 1,
        "backend": backend,
        "status": status,
        "libraries": effective_libraries,
        "diagnostic": diagnostic,
    }
    return {
        "graphics_library_contract": "1",
        "graphics_library_status": status,
        "graphics_library_count": str(len(effective_libraries)),
        "graphics_library_sha256": comparison.graphics_library_aggregate(
            effective_libraries
        ),
        "graphics_library_manifest": quote(
            json.dumps(manifest, separators=(",", ":"), sort_keys=True), safe=""
        ),
    }


def make_report(profile: str = "quick") -> dict[str, object]:
    full = profile == "full"
    pane_warmup = 200 if full else 8
    pane_iterations = 200 if full else 12
    shader_warmup = 200 if full else 8
    shader_iterations = 100 if full else 6

    def pane_record(grid: str, scenario: str) -> dict[str, str]:
        return {
            "record": "pane_scenario",
            "grid": grid,
            "scenario": scenario,
            "cpu_total_median_us": "1000",
            "cpu_total_p90_us": "1100",
            "gpu_median_us": "500",
            "gpu_valid_samples": f"{pane_iterations}/{pane_iterations}",
            "measured_frames": str(pane_iterations),
            "paints": str(pane_iterations),
            "glyph_atlas_entry_count_delta": "0",
            "glyph_atlas_bytes_delta": "0",
            "kitty_texture_set_count_delta": "0",
            **{field: "0" for field in comparison.PANE_WORK_FIELDS},
        }

    def shader_record(renderer: str, workload: str, passes: str) -> dict[str, str]:
        return {
            "record": "shader_scenario",
            "renderer": renderer,
            "workload": workload,
            "passes": passes,
            "cpu_total_median_us": "1000",
            "cpu_total_p90_us": "1100",
            "cpu_total_vs_legacy_ratio": "1.00",
            "gpu_median_us": "500",
            "gpu_vs_legacy_ratio": "1.00",
            "gpu_valid_samples": str(shader_iterations),
            "measured_frames": str(shader_iterations),
            "validation_readbacks": "2",
            **{field: "0" for field in comparison.SHADER_WORK_FIELDS},
        }

    shader_baselines = [
        {
            "record": "shader_baseline",
            "workload": workload,
            "cpu_total_median_us": "1000",
            "cpu_pooled_samples": str(shader_iterations * 2),
            "gpu_median_us": "500",
            "gpu_pooled_samples": str(shader_iterations * 2),
        }
        for workload in comparison.SHADER_WORKLOADS
    ]
    shader_records = [
        shader_record(renderer, workload, passes)
        for renderer in ("retained", "legacy")
        for workload in comparison.SHADER_WORKLOADS
        for passes in comparison.SHADER_PASS_COUNTS
    ]
    swapchain_record = {
        "record": "swapchain",
        "benchmark_contract": "1",
        "qt_version": "6.11.1",
        "platform": "wayland",
        "graphics_api": "vulkan",
        "dpr": "1",
        "logical": "1100x720",
        "physical": "1100x720",
        "screen_count": "1",
        "screen_name": "eDP-1",
        "screen_manufacturer": "Test",
        "screen_model": "Panel",
        "screen_serial": "123",
        "screen_geometry": "0,0,1920x1080",
        "screen_available_geometry": "0,0,1920x1040",
        "screen_physical_mm": "300x170",
        "screen_depth": "24",
        "screen_dpr": "1",
        "screen_logical_dpi_x": "96",
        "screen_logical_dpi_y": "96",
        "screen_physical_dpi_x": "160",
        "screen_physical_dpi_y": "160",
        "screen_refresh_millihz": "60000",
        "screen_orientation": "1",
        "screen_primary": "1",
        "swapchain_format": "sdr",
        "swapchain_flags": "1",
        "swapchain_samples": "1",
        "swapchain_size": "1100x720",
        "swapchain_srgb": "0",
        "swapchain_premultiplied_alpha": "1",
        "swapchain_nonpremultiplied_alpha": "0",
        "swapchain_no_vsync": "0",
        "swapchain_hdr": "0",
        "swapchain_hdr_limits": "na",
        "swapchain_hdr_behavior": "na",
        "swapchain_hdr_minimum": "na",
        "swapchain_hdr_maximum": "na",
        "swapchain_hdr_maximum_potential": "na",
        "swapchain_hdr_sdr_white": "na",
        "frame_swaps": "30",
        "median_frame_interval_us": "16666",
        "p90_frame_interval_us": "17000",
        "alpha_buffer_bits": "8",
        "clear_alpha": "0",
        "image_alpha": "1",
        "nonuniform": "1",
        "min_alpha": "128",
        "max_alpha": "255",
        "translucent_pixels": "760000",
        "half_alpha_pixels": "750000",
        "pixel_count": "792000",
        "minimum_half_alpha_pixels": "39600",
        "panes": "1",
        "running_panes": "1",
        **graphics_library_record_fields(),
        **DEVICE,
    }
    return {
        "schema_version": comparison.SUPPORTED_QUALIFICATION_SCHEMA,
        "status": "pass",
        "reason_code": "completed",
        "configuration": {
            "profile": profile,
            "backends": ["vulkan"],
            "required_backends": [],
            "synthetic_global_scales": ["1"],
            "pane_warmup": pane_warmup,
            "pane_iterations": pane_iterations,
            "shader_warmup": shader_warmup,
            "shader_iterations": shader_iterations,
            "allow_software_device": False,
            "build_performed": False,
            "renderdoc_scenarios": [],
        },
        "host": {
            "uname": ["Linux", "test-host", "1.0", "build", "x86_64", ""],
            "processors": 8,
            "cpu_model": "Test CPU",
            "os_release": {"id": "test", "version_id": "1"},
            "repository_revision": "a" * 40,
            "repository_dirty": False,
            "ghostty_revision": "b" * 40,
            "environment": {
                "XDG_CURRENT_DESKTOP": "KDE",
                "XDG_SESSION_DESKTOP": "plasma",
                "XDG_SESSION_TYPE": "wayland",
            },
            "wayland_peer": {
                "identity_source": "linux_so_peercred",
                "diagnostic": None,
                "peer": {
                    "pid": 123,
                    "uid": 1000,
                    "gid": 1000,
                    "comm": "kwin_wayland",
                    "executable": "/usr/bin/kwin_wayland",
                    "executable_sha256": "c" * 64,
                    "executable_size": 123456,
                },
            },
        },
        "harness": {"sha256": "d" * 64},
        "artifacts": {
            "ghostty-qt": {"sha256": "e" * 64},
            "bench-terminal-pane-renderer": {"sha256": "f" * 64},
            "bench-terminal-custom-shader-rhi": {"sha256": "0" * 64},
        },
        "runs": [
            {
                "kind": "scenario_discovery",
                "status": "pass",
                "reason_code": "completed",
                "scenarios": ["metadata"],
            },
            {
                "kind": "pane_offscreen",
                "backend": "vulkan",
                "scale": "1",
                "status": "pass",
                "reason_code": "completed",
                "rhi_device": dict(DEVICE),
                "records": [
                    {
                        "record": "pane_header",
                        "qt_version": "6.11.1",
                        "benchmark_contract": "1",
                        "backend": "vulkan",
                        "platform": "wayland",
                        "presentation": "offscreen",
                        "warmup": str(pane_warmup),
                        "iterations": str(pane_iterations),
                        "kitty_placements": "512",
                    },
                    {
                        "record": "pane_grid",
                        "grid": "120x40",
                        "dpr": "1",
                        "logical": "960x680",
                        "framebuffer": "960x680",
                        **DEVICE,
                    },
                    {
                        "record": "pane_grid",
                        "grid": "240x80",
                        "dpr": "1",
                        "logical": "1920x1360",
                        "framebuffer": "1920x1360",
                        **DEVICE,
                    },
                    pane_record("120x40", "metadata"),
                    pane_record("240x80", "metadata"),
                ],
            },
            {
                "kind": "custom_shader_offscreen",
                "backend": "vulkan",
                "scale": "1",
                "status": "pass",
                "reason_code": "completed",
                "rhi_device": dict(DEVICE),
                "records": [
                    {
                        "record": "shader_header",
                        "qt": "6.11.1",
                        "benchmark_contract": "1",
                        "platform": "wayland",
                        "graphics_api": "vulkan",
                        "rhi_backend": "Vulkan",
                        "gpu_timestamps": "supported",
                        **{
                            field: value
                            for field, value in DEVICE.items()
                            if field != "rhi_backend"
                        },
                        "viewport": "1280x720",
                        "framebuffer": "1280x720",
                        "dpr": "1",
                        "warmup": str(shader_warmup),
                        "iterations": str(shader_iterations),
                        "measurement_rounds": "2",
                        "renderer_order": "legacy-retained/retained-legacy",
                        "measured_frames_per_round": (
                            f"{shader_iterations // 2}/{shader_iterations // 2}"
                        ),
                        "completion": "offscreen-end-frame",
                        "validation_readbacks_per_scenario": "2",
                        "gpu_scope": "whole-command-buffer",
                        "gpu_delta_baseline": "pooled-workload-pass0",
                        "uniform_snapshots": "shared",
                        "transforms": "ordered-affine",
                    },
                    *shader_baselines,
                    *shader_records,
                ],
            },
            {
                "kind": "production_swapchain",
                "backend": "vulkan",
                "status": "pass",
                "reason_code": "completed",
                "rhi_device": dict(DEVICE),
                "records": [swapchain_record],
            },
        ],
        "summary": {
            "passed_backends": ["vulkan"],
            "skipped_backends": [],
            "pass_runs": 4,
            "skip_runs": 0,
            "fail_runs": 0,
        },
    }


def find_record(report: dict[str, object], record_type: str) -> dict[str, str]:
    runs = report["runs"]
    assert isinstance(runs, list)
    for run in runs:
        if not isinstance(run, dict):
            continue
        records = run.get("records", [])
        if not isinstance(records, list):
            continue
        for record in records:
            if isinstance(record, dict) and record.get("record") == record_type:
                return record
    raise AssertionError(f"missing {record_type}")


class RendererQualificationComparisonTest(unittest.TestCase):
    def test_identical_quick_report_passes_with_advisory_timing(self) -> None:
        report = make_report()
        result = comparison.compare_reports(
            report, copy.deepcopy(report), comparison.Policy()
        )
        self.assertEqual(result["status"], "pass")
        self.assertEqual(result["effective_timing_mode"], "advisory")
        self.assertEqual(result["summary"]["regressions"], 0)

    def test_quick_timing_regression_is_advisory(self) -> None:
        baseline = make_report()
        candidate = copy.deepcopy(baseline)
        find_record(candidate, "pane_scenario")["cpu_total_median_us"] = "1400"
        result = comparison.compare_reports(baseline, candidate, comparison.Policy())
        self.assertEqual(result["status"], "pass")
        self.assertEqual(result["summary"]["advisory_regressions"], 1)

    def test_full_timing_and_shader_ratio_regressions_are_enforced(self) -> None:
        baseline = make_report("full")
        candidate = copy.deepcopy(baseline)
        find_record(candidate, "pane_scenario")["cpu_total_median_us"] = "1400"
        find_record(candidate, "shader_scenario")["cpu_total_vs_legacy_ratio"] = "1.30"
        result = comparison.compare_reports(baseline, candidate, comparison.Policy())
        self.assertEqual(result["status"], "regression")
        self.assertEqual(result["effective_timing_mode"], "enforce")
        self.assertEqual(result["summary"]["regressions"], 2)

        quick_baseline = make_report()
        quick_candidate = copy.deepcopy(quick_baseline)
        find_record(quick_candidate, "pane_scenario")["cpu_total_median_us"] = "1400"
        forced = comparison.compare_reports(
            quick_baseline,
            quick_candidate,
            comparison.Policy(timing_mode="enforce"),
        )
        self.assertEqual(forced["status"], "regression")

    def test_percentage_and_absolute_limits_must_both_be_exceeded(self) -> None:
        baseline = make_report("full")
        candidate = copy.deepcopy(baseline)
        find_record(candidate, "pane_scenario")["cpu_total_median_us"] = "1100"
        policy = comparison.Policy(
            timing_mode="enforce",
            pane_cpu_percent=5,
            pane_cpu_minimum_us=100,
        )
        result = comparison.compare_reports(baseline, candidate, policy)
        self.assertEqual(result["status"], "pass")

    def test_structural_work_increase_is_always_enforced(self) -> None:
        baseline = make_report()
        candidate = copy.deepcopy(baseline)
        find_record(candidate, "pane_scenario")["text_layouts"] = "1"
        result = comparison.compare_reports(baseline, candidate, comparison.Policy())
        self.assertEqual(result["status"], "regression")
        self.assertEqual(result["summary"]["regressions"], 1)

        improved_baseline = make_report()
        find_record(improved_baseline, "pane_scenario")["text_layouts"] = "2"
        improved_candidate = copy.deepcopy(improved_baseline)
        find_record(improved_candidate, "pane_scenario")["text_layouts"] = "1"
        improved = comparison.compare_reports(
            improved_baseline, improved_candidate, comparison.Policy()
        )
        self.assertEqual(improved["status"], "pass")
        self.assertEqual(improved["summary"]["improvements"], 1)

    def test_lost_batched_glyph_coverage_is_always_enforced(self) -> None:
        baseline = make_report()
        find_record(baseline, "pane_scenario")["batched_glyphs"] = "100"
        candidate = copy.deepcopy(baseline)
        find_record(candidate, "pane_scenario")["batched_glyphs"] = "99"
        result = comparison.compare_reports(baseline, candidate, comparison.Policy())
        self.assertEqual(result["status"], "regression")
        self.assertEqual(result["summary"]["regressions"], 1)

        expanded = copy.deepcopy(baseline)
        find_record(expanded, "pane_scenario")["batched_glyphs"] = "101"
        improved = comparison.compare_reports(baseline, expanded, comparison.Policy())
        self.assertEqual(improved["status"], "pass")
        self.assertEqual(improved["summary"]["improvements"], 1)

    def test_gpu_timestamp_loss_is_a_regression(self) -> None:
        baseline = make_report()
        candidate = copy.deepcopy(baseline)
        candidate_record = find_record(candidate, "pane_scenario")
        candidate_record["gpu_median_us"] = "unavailable"
        candidate_record["gpu_valid_samples"] = "0/12"
        result = comparison.compare_reports(baseline, candidate, comparison.Policy())
        self.assertEqual(result["status"], "regression")
        self.assertEqual(result["summary"]["regressions"], 1)

        unavailable_baseline = make_report()
        unavailable_candidate = copy.deepcopy(unavailable_baseline)
        for report in (unavailable_baseline, unavailable_candidate):
            record = find_record(report, "pane_scenario")
            record["gpu_median_us"] = "na"
            record["gpu_valid_samples"] = "0/12"
        unavailable = comparison.compare_reports(
            unavailable_baseline,
            unavailable_candidate,
            comparison.Policy(),
        )
        self.assertEqual(unavailable["status"], "pass")

    def test_context_change_requires_explicit_override(self) -> None:
        baseline = make_report()
        candidate = copy.deepcopy(baseline)
        host = candidate["host"]
        assert isinstance(host, dict)
        host["cpu_model"] = "Different CPU"
        incompatible = comparison.compare_reports(
            baseline, candidate, comparison.Policy()
        )
        self.assertEqual(incompatible["status"], "incompatible")

        allowed = comparison.compare_reports(
            baseline,
            candidate,
            comparison.Policy(allow_context_changes=True),
        )
        self.assertEqual(allowed["status"], "pass")
        self.assertEqual(allowed["summary"]["context_changes"], 1)

        output_candidate = copy.deepcopy(baseline)
        find_record(output_candidate, "swapchain")["screen_refresh_millihz"] = "120000"
        output_mismatch = comparison.compare_reports(
            baseline, output_candidate, comparison.Policy()
        )
        self.assertEqual(output_mismatch["status"], "incompatible")

    def test_device_change_remains_structurally_incompatible(self) -> None:
        baseline = make_report()
        candidate = copy.deepcopy(baseline)
        runs = candidate["runs"]
        assert isinstance(runs, list)
        for run in runs:
            if isinstance(run, dict) and isinstance(run.get("rhi_device"), dict):
                run["rhi_device"]["rhi_device_name"] = "Other%20GPU"
        result = comparison.compare_reports(
            baseline,
            candidate,
            comparison.Policy(allow_context_changes=True),
        )
        self.assertEqual(result["status"], "incompatible")

    def test_effective_configuration_not_profile_label_controls_compatibility(
        self,
    ) -> None:
        baseline = make_report()
        candidate = copy.deepcopy(baseline)
        candidate["configuration"]["profile"] = "renamed-profile"
        result = comparison.compare_reports(baseline, candidate, comparison.Policy())
        self.assertEqual(result["status"], "pass")

        candidate["configuration"]["build_performed"] = True
        result = comparison.compare_reports(baseline, candidate, comparison.Policy())
        self.assertEqual(result["status"], "incompatible")

    def test_reordering_is_allowed_but_missing_and_duplicate_records_are_not(
        self,
    ) -> None:
        baseline = make_report()
        reordered = copy.deepcopy(baseline)
        runs = reordered["runs"]
        assert isinstance(runs, list)
        runs.reverse()
        result = comparison.compare_reports(baseline, reordered, comparison.Policy())
        self.assertEqual(result["status"], "pass")

        missing = copy.deepcopy(baseline)
        shader_run = next(
            run
            for run in missing["runs"]
            if isinstance(run, dict) and run.get("kind") == "custom_shader_offscreen"
        )
        shader_run["records"] = [
            record
            for record in shader_run["records"]
            if record.get("record") != "shader_scenario"
        ]
        result = comparison.compare_reports(baseline, missing, comparison.Policy())
        self.assertEqual(result["status"], "incompatible")

        duplicate = copy.deepcopy(baseline)
        pane = find_record(duplicate, "pane_scenario")
        pane_run = next(
            run
            for run in duplicate["runs"]
            if isinstance(run, dict) and run.get("kind") == "pane_offscreen"
        )
        pane_run["records"].append(dict(pane))
        result = comparison.compare_reports(baseline, duplicate, comparison.Policy())
        self.assertEqual(result["status"], "incompatible")

    def test_invalid_and_skipped_reports_never_pass(self) -> None:
        baseline = make_report()
        malformed = copy.deepcopy(baseline)
        malformed["harness"]["sha256"] = "bad"
        result = comparison.compare_reports(baseline, malformed, comparison.Policy())
        self.assertEqual(result["status"], "incompatible")

        old_schema = copy.deepcopy(baseline)
        old_schema["schema_version"] = 2
        result = comparison.compare_reports(baseline, old_schema, comparison.Policy())
        self.assertEqual(result["status"], "incompatible")

        skipped = copy.deepcopy(baseline)
        skipped["status"] = "skip"
        skipped["reason_code"] = "no_supported_backend"
        result = comparison.compare_reports(baseline, skipped, comparison.Policy())
        self.assertEqual(result["status"], "skip")

        nonfinite = copy.deepcopy(baseline)
        find_record(nonfinite, "pane_scenario")["cpu_total_median_us"] = "nan"
        result = comparison.compare_reports(baseline, nonfinite, comparison.Policy())
        self.assertEqual(result["status"], "incompatible")

    def test_provenance_changes_are_reported_but_remain_comparable(self) -> None:
        baseline = make_report()
        candidate = copy.deepcopy(baseline)
        candidate["host"]["repository_revision"] = "9" * 40
        candidate["harness"]["sha256"] = "8" * 64
        candidate["artifacts"]["ghostty-qt"]["sha256"] = "7" * 64
        result = comparison.compare_reports(baseline, candidate, comparison.Policy())
        self.assertEqual(result["status"], "pass")
        self.assertEqual(len(result["provenance_changes"]), 3)

    def test_independently_rejects_truncated_or_incomplete_matrices(self) -> None:
        baseline = make_report()
        truncated = copy.deepcopy(baseline)
        truncated["runs"] = [
            run
            for run in truncated["runs"]
            if not isinstance(run, dict) or run.get("kind") != "production_swapchain"
        ]
        result = comparison.compare_reports(
            truncated, copy.deepcopy(truncated), comparison.Policy()
        )
        self.assertEqual(result["status"], "incompatible")

    def test_rejects_internally_impossible_production_records(self) -> None:
        mutations = (
            ("swapchain_format", "hdr10"),
            ("swapchain_flags", "0"),
            ("swapchain_nonpremultiplied_alpha", "1"),
            ("swapchain_srgb", "1"),
            ("swapchain_hdr_limits", "nits"),
            ("min_alpha", "255"),
        )
        for field, value in mutations:
            with self.subTest(field=field):
                malformed = make_report()
                find_record(malformed, "swapchain")[field] = value
                result = comparison.compare_reports(
                    malformed,
                    copy.deepcopy(malformed),
                    comparison.Policy(),
                )
                self.assertEqual(result["status"], "incompatible")

        missing_scenario = make_report()
        missing_scenario["runs"][0]["scenarios"].append("cursor-only")
        result = comparison.compare_reports(
            missing_scenario,
            copy.deepcopy(missing_scenario),
            comparison.Policy(),
        )
        self.assertEqual(result["status"], "incompatible")

        missing_device = make_report()
        runs = missing_device["runs"]
        assert isinstance(runs, list)
        next(
            run
            for run in runs
            if isinstance(run, dict) and run.get("kind") == "pane_offscreen"
        )["rhi_device"].pop("rhi_device_name")
        result = comparison.compare_reports(
            missing_device, copy.deepcopy(missing_device), comparison.Policy()
        )
        self.assertEqual(result["status"], "incompatible")

    def test_rejects_malformed_typed_configuration_and_host_identity(self) -> None:
        malformed = make_report("full")
        malformed["configuration"]["pane_iterations"] = "200"
        result = comparison.compare_reports(
            malformed, copy.deepcopy(malformed), comparison.Policy()
        )
        self.assertEqual(result["status"], "incompatible")

        unknown_peer = make_report()
        unknown_peer["host"]["wayland_peer"]["peer"]["executable_sha256"] = None
        result = comparison.compare_reports(
            unknown_peer, copy.deepcopy(unknown_peer), comparison.Policy()
        )
        self.assertEqual(result["status"], "incompatible")

    def test_rejects_nonpositive_gpu_values_and_malformed_sample_evidence(
        self,
    ) -> None:
        zero = make_report()
        find_record(zero, "pane_scenario")["gpu_median_us"] = "0"
        result = comparison.compare_reports(
            zero, copy.deepcopy(zero), comparison.Policy()
        )
        self.assertEqual(result["status"], "incompatible")

        malformed = make_report()
        find_record(malformed, "pane_scenario")["gpu_valid_samples"] = "broken"
        result = comparison.compare_reports(
            malformed, copy.deepcopy(malformed), comparison.Policy()
        )
        self.assertEqual(result["status"], "incompatible")

        shader_loss = make_report()
        shader = find_record(shader_loss, "shader_scenario")
        shader["gpu_valid_samples"] = "0"
        result = comparison.compare_reports(
            shader_loss, copy.deepcopy(shader_loss), comparison.Policy()
        )
        self.assertEqual(result["status"], "incompatible")

    def test_rejects_malformed_graphics_library_evidence(self) -> None:
        def candidate_with(fields: dict[str, str]) -> dict[str, object]:
            candidate = make_report()
            find_record(candidate, "swapchain").update(fields)
            return candidate

        malformed_candidates = (
            candidate_with(graphics_library_record_fields([])),
            candidate_with(
                {
                    **graphics_library_record_fields(),
                    "graphics_library_manifest": "%ZZ",
                }
            ),
            candidate_with(
                {
                    **graphics_library_record_fields(),
                    "graphics_library_count": "2",
                }
            ),
            candidate_with(
                graphics_library_record_fields(
                    [
                        {
                            "role": "driver",
                            "name": "libvulkan_radeon.so",
                            "path_kind": "system",
                            "size": 123456,
                            "sha256": "BAD",
                        }
                    ]
                )
            ),
            candidate_with(
                graphics_library_record_fields(
                    [
                        {
                            "role": "driver",
                            "name": "libvulkan_radeon.so",
                            "path_kind": "system",
                            "size": 123456,
                            "sha256": "1" * 64,
                        },
                        {
                            "role": "driver",
                            "name": "libvulkan_radeon.so",
                            "path_kind": "custom",
                            "size": 123456,
                            "sha256": "1" * 64,
                        },
                    ]
                )
            ),
            candidate_with(
                graphics_library_record_fields(
                    [
                        {
                            "role": "layer",
                            "name": "libVkLayer_test.so",
                            "path_kind": "system",
                            "size": 1000,
                            "sha256": "3" * 64,
                        }
                    ]
                )
            ),
            candidate_with(
                graphics_library_record_fields(
                    [
                        {
                            "role": "driver",
                            "name": "driver\nname.so",
                            "path_kind": "system",
                            "size": 123456,
                            "sha256": "1" * 64,
                        }
                    ]
                )
            ),
            candidate_with(
                {
                    **graphics_library_record_fields(),
                    "graphics_library_sha256": "0" * 64,
                }
            ),
            candidate_with(graphics_library_record_fields(status="unavailable")),
        )
        for candidate in malformed_candidates:
            with self.subTest(
                status=find_record(candidate, "swapchain").get(
                    "graphics_library_status"
                )
            ):
                result = comparison.compare_reports(
                    make_report(), candidate, comparison.Policy()
                )
                self.assertEqual(result["status"], "incompatible")

    def test_graphics_library_content_is_context_but_audit_fields_are_not(
        self,
    ) -> None:
        baseline = make_report()
        audit_only = copy.deepcopy(baseline)
        find_record(audit_only, "swapchain").update(
            graphics_library_record_fields(
                [
                    {
                        "role": "driver",
                        "name": "libvulkan_radeon.so",
                        "path_kind": "custom",
                        "size": 123456,
                        "sha256": "1" * 64,
                    }
                ],
                diagnostic="resolved through a custom search path",
            )
        )
        result = comparison.compare_reports(baseline, audit_only, comparison.Policy())
        self.assertEqual(result["status"], "pass")

        changed = copy.deepcopy(baseline)
        find_record(changed, "swapchain").update(
            graphics_library_record_fields(
                [
                    {
                        "role": "driver",
                        "name": "libvulkan_radeon.so",
                        "path_kind": "system",
                        "size": 123456,
                        "sha256": "2" * 64,
                    }
                ]
            )
        )
        strict = comparison.compare_reports(baseline, changed, comparison.Policy())
        self.assertEqual(strict["status"], "incompatible")
        allowed = comparison.compare_reports(
            baseline,
            changed,
            comparison.Policy(allow_context_changes=True),
        )
        self.assertEqual(allowed["status"], "pass")
        self.assertGreater(allowed["summary"]["context_changes"], 0)

    def test_graphics_library_manifest_rejects_lone_unicode_surrogates(self) -> None:
        baseline = make_report()
        encoded = find_record(baseline, "swapchain")["graphics_library_manifest"]
        manifest = json.loads(unquote(encoded))
        manifest["libraries"][0]["name"] = "\ud800"
        surrogate_name = quote(
            json.dumps(manifest, separators=(",", ":"), sort_keys=True), safe=""
        )
        manifest["libraries"][0]["name"] = "libvulkan_radeon.so"
        manifest["diagnostic"] = "\udfff"
        surrogate_diagnostic = quote(
            json.dumps(manifest, separators=(",", ":"), sort_keys=True), safe=""
        )

        for malformed in (surrogate_name, surrogate_diagnostic, "\ud800"):
            with self.subTest(manifest=ascii(malformed)[:80]):
                candidate = make_report()
                find_record(candidate, "swapchain")["graphics_library_manifest"] = (
                    malformed
                )
                result = comparison.compare_reports(
                    baseline, candidate, comparison.Policy()
                )
                self.assertEqual(result["status"], "incompatible")

        unicode_report = make_report()
        find_record(unicode_report, "swapchain").update(
            graphics_library_record_fields(
                [
                    {
                        "role": "driver",
                        "name": "lib\U0001f600.so",
                        "path_kind": "custom",
                        "size": 123456,
                        "sha256": "4" * 64,
                    }
                ]
            )
        )
        result = comparison.compare_reports(
            unicode_report, copy.deepcopy(unicode_report), comparison.Policy()
        )
        self.assertEqual(result["status"], "pass")

    def test_absolute_shader_timings_are_gated_independently_of_ratios(self) -> None:
        baseline = make_report("full")
        candidate = copy.deepcopy(baseline)
        shader = find_record(candidate, "shader_scenario")
        shader["cpu_total_median_us"] = "1400"
        shader["gpu_median_us"] = "700"
        result = comparison.compare_reports(baseline, candidate, comparison.Policy())
        self.assertEqual(result["status"], "regression")
        regressions = {
            metric["metric"]
            for metric in result["metrics"]
            if metric["classification"] == "regression"
        }
        self.assertEqual(regressions, {"cpu_total_median_us", "gpu_median_us"})

    def test_timing_improvements_use_symmetric_noise_floors(self) -> None:
        baseline = make_report("full")
        within_floor = copy.deepcopy(baseline)
        find_record(within_floor, "pane_scenario")["cpu_total_median_us"] = "950"
        result = comparison.compare_reports(baseline, within_floor, comparison.Policy())
        pane_metric = next(
            metric
            for metric in result["metrics"]
            if metric["identity"][3] == "pane_scenario"
            and metric["metric"] == "cpu_total_median_us"
        )
        self.assertEqual(pane_metric["classification"], "stable")

        beyond_floor = copy.deepcopy(baseline)
        find_record(beyond_floor, "pane_scenario")["cpu_total_median_us"] = "799"
        result = comparison.compare_reports(baseline, beyond_floor, comparison.Policy())
        pane_metric = next(
            metric
            for metric in result["metrics"]
            if metric["identity"][3] == "pane_scenario"
            and metric["metric"] == "cpu_total_median_us"
        )
        self.assertEqual(pane_metric["classification"], "improvement")

    def test_cli_refuses_to_overwrite_either_input(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            baseline = root / "baseline.json"
            candidate = root / "candidate.json"
            baseline_content = json.dumps(make_report())
            candidate_content = json.dumps(make_report())
            baseline.write_text(baseline_content, encoding="utf-8")
            candidate.write_text(candidate_content, encoding="utf-8")

            self.assertEqual(
                comparison.main(
                    [str(baseline), str(candidate), "--output", str(baseline)]
                ),
                2,
            )
            self.assertEqual(baseline.read_text(encoding="utf-8"), baseline_content)
            self.assertEqual(
                comparison.main(
                    [str(baseline), str(candidate), "--output", str(candidate)]
                ),
                2,
            )
            self.assertEqual(candidate.read_text(encoding="utf-8"), candidate_content)

    def test_cli_writes_atomic_comparison_report(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            baseline = root / "baseline.json"
            candidate = root / "candidate.json"
            output = root / "comparison.json"
            baseline.write_text(json.dumps(make_report()), encoding="utf-8")
            candidate.write_text(json.dumps(make_report()), encoding="utf-8")
            exit_code = comparison.main(
                [str(baseline), str(candidate), "--output", str(output)]
            )
            self.assertEqual(exit_code, 0)
            self.assertEqual(
                json.loads(output.read_text(encoding="utf-8"))["status"], "pass"
            )
            self.assertEqual(list(root.glob(".*.tmp")), [])


if __name__ == "__main__":
    unittest.main()
