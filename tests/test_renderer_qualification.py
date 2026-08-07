from __future__ import annotations

import importlib.util
import json
import os
import signal
import stat
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "qualify-wayland-renderer.py"
SPEC = importlib.util.spec_from_file_location("qualify_wayland_renderer", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
qualification = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = qualification
SPEC.loader.exec_module(qualification)


class RendererQualificationTest(unittest.TestCase):
    def test_parses_benchmark_and_swapchain_records(self) -> None:
        pane_header = qualification.parse_key_value_record(
            "qt_version=6.11.1 backend=vulkan platform=wayland "
            "presentation=offscreen warmup=8 iterations=12"
        )
        self.assertEqual(pane_header["record"], "pane_header")
        self.assertEqual(pane_header["backend"], "vulkan")

        pane_scenario = qualification.parse_key_value_record(
            "120x40 cursor-only cpu_total_median_us=123.4 measured_frames=12"
        )
        self.assertEqual(pane_scenario["record"], "pane_scenario")
        self.assertEqual(pane_scenario["grid"], "120x40")
        self.assertEqual(pane_scenario["scenario"], "cursor-only")

        swapchain = qualification.parse_key_value_record(
            "renderer_qualification platform=wayland graphics_api=opengl "
            "dpr=1.25 frame_swaps=30"
        )
        self.assertEqual(swapchain["record"], "swapchain")
        self.assertEqual(swapchain["graphics_api"], "opengl")

    def test_rejects_malformed_records(self) -> None:
        self.assertIsNone(qualification.parse_key_value_record("ordinary log"))
        self.assertIsNone(
            qualification.parse_key_value_record("120x40 cursor-only broken")
        )

    def test_wayland_socket_validation(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            socket_path = Path(directory) / "wayland-test"
            environment = {
                "XDG_RUNTIME_DIR": directory,
                "WAYLAND_DISPLAY": socket_path.name,
                "XDG_SESSION_TYPE": "wayland",
            }
            socket_stat = os.stat_result(
                (stat.S_IFSOCK | 0o600, 0, 0, 1, 1000, 1000, 0, 0, 0, 0)
            )
            with mock.patch.object(Path, "stat", return_value=socket_stat):
                ready, reason, actual = qualification.validate_wayland_environment(
                    environment
                )
            self.assertTrue(ready)
            self.assertEqual(reason, "ready")
            self.assertEqual(actual, socket_path)

            wrong_type = qualification.validate_wayland_environment(
                {**environment, "XDG_SESSION_TYPE": "x11"}
            )
            self.assertFalse(wrong_type[0])
            self.assertEqual(wrong_type[1], "not_wayland_session")

    def test_records_wayland_peer_process_identity(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable = root / "compositor"
            executable.write_bytes(b"test-compositor")
            process_root = root / "proc" / "42"
            process_root.mkdir(parents=True)
            (process_root / "comm").write_text("test-wayland\n", encoding="utf-8")
            (process_root / "cmdline").write_bytes(b"test-wayland\0--flag\0")
            (process_root / "exe").symlink_to(executable)

            identity = qualification.process_identity(42, root / "proc")
            self.assertEqual(identity["comm"], "test-wayland")
            self.assertEqual(identity["executable"], str(executable))
            self.assertEqual(identity["command"], ["test-wayland", "--flag"])
            self.assertRegex(str(identity["executable_sha256"]), r"^[0-9a-f]{64}$")

            peer_socket = mock.Mock()
            peer_socket.getsockopt.return_value = qualification.struct.pack(
                "3i", 42, 1000, 1001
            )
            expected_identity = {"pid": 42, "comm": "test-wayland"}
            with (
                mock.patch.object(
                    qualification.socket, "socket", return_value=peer_socket
                ),
                mock.patch.object(
                    qualification,
                    "process_identity",
                    return_value=expected_identity,
                ),
            ):
                metadata = qualification.wayland_peer_metadata(
                    {
                        "XDG_RUNTIME_DIR": directory,
                        "WAYLAND_DISPLAY": "wayland-test",
                    }
                )
            peer_socket.connect.assert_called_once_with(str(root / "wayland-test"))
            peer_socket.close.assert_called_once()
            self.assertEqual(
                metadata["peer"],
                {"pid": 42, "comm": "test-wayland", "uid": 1000, "gid": 1001},
            )

    def test_environment_scrubs_renderer_overrides(self) -> None:
        environment, evidence = qualification.qualification_environment(
            {
                "WAYLAND_DISPLAY": "wayland-0",
                "XDG_RUNTIME_DIR": "/run/user/1000",
                "LIBGL_ALWAYS_SOFTWARE": "1",
                "QT_QUICK_BACKEND": "software",
                "QT_SCALE_FACTOR": "9",
            },
            "vulkan",
            "1.25",
        )
        self.assertEqual(environment["QT_QPA_PLATFORM"], "wayland")
        self.assertEqual(environment["QSG_RHI_BACKEND"], "vulkan")
        self.assertEqual(environment["QT_SCALE_FACTOR"], "1.25")
        self.assertNotIn("LIBGL_ALWAYS_SOFTWARE", environment)
        self.assertNotIn("QT_QUICK_BACKEND", environment)
        self.assertEqual(evidence["LIBGL_ALWAYS_SOFTWARE"], "<unset>")

        unscaled, _ = qualification.qualification_environment(
            environment, "opengl", None
        )
        self.assertNotIn("QT_SCALE_FACTOR", unscaled)
        self.assertNotIn("QT_SCALE_FACTOR_ROUNDING_POLICY", unscaled)

    def test_scale_resolution_is_positive_unique_and_includes_one(self) -> None:
        resolved = qualification.resolved_scales(
            ["1.25", "2.0", "1.250"], qualification.PROFILES["quick"]
        )
        self.assertEqual(resolved, ["1", "1.25", "2"])
        with self.assertRaises(ValueError):
            qualification.resolved_scales(["0"], qualification.PROFILES["quick"])

    @staticmethod
    def pane_run(
        dpr: str = "1.25",
        scenarios: tuple[str, ...] = ("cursor-only",),
        device_type: str = "integrated",
        device_name: str = "Test%20GPU",
    ) -> dict[str, object]:
        scale = float(dpr)
        lines = [
            "qt_version=6.11.1 backend=opengl platform=wayland "
            + "presentation=offscreen benchmark_contract=1 warmup=8 iterations=12"
        ]
        for grid, logical in (("120x40", (960, 680)), ("240x80", (1920, 1360))):
            framebuffer = tuple(round(value * scale) for value in logical)
            lines.append(
                f"grid={grid} rhi_backend=OpenGLES2 dpr={dpr} "
                f"logical={logical[0]}x{logical[1]} "
                f"framebuffer={framebuffer[0]}x{framebuffer[1]} "
                f"rhi_device_name={device_name} "
                f"rhi_device_type={device_type} rhi_vendor_id=0x1002 "
                "rhi_device_id=0x1234"
            )
            lines.extend(
                f"{grid} {scenario} cpu_total_median_us=1 measured_frames=12"
                for scenario in scenarios
            )
        output = "\n".join(lines)
        return {
            "return_code": 0,
            "status": "pending",
            "records": qualification.parse_records(output),
        }

    def test_validates_complete_pane_run_and_explicit_skip(self) -> None:
        run = self.pane_run()
        dpr = qualification.validate_pane_run(run, "opengl", ("cursor-only",), 12)
        self.assertEqual(dpr, 1.25)
        self.assertEqual(run["status"], "pass")

        skipped: dict[str, object] = {"return_code": 77, "records": []}
        self.assertIsNone(
            qualification.validate_pane_run(skipped, "opengl", ("cursor-only",), 12)
        )
        self.assertEqual(skipped["status"], "skip")
        self.assertEqual(skipped["reason_code"], "unsupported_backend")

    def test_rejects_incomplete_pane_matrix(self) -> None:
        run = self.pane_run()
        qualification.validate_pane_run(run, "opengl", ("cursor-only", "metadata"), 12)
        self.assertEqual(run["status"], "fail")
        self.assertEqual(run["reason_code"], "incomplete_scenario_matrix")

        duplicate = self.pane_run(scenarios=("cursor-only", "cursor-only"))
        qualification.validate_pane_run(
            duplicate, "opengl", ("cursor-only", "metadata"), 12
        )
        self.assertEqual(duplicate["reason_code"], "incomplete_scenario_matrix")

    def test_rejects_software_rhi_unless_explicitly_allowed(self) -> None:
        rejected = self.pane_run(device_type="cpu")
        qualification.validate_pane_run(rejected, "opengl", ("cursor-only",), 12)
        self.assertEqual(rejected["reason_code"], "software_rhi_device")

        allowed = self.pane_run(device_type="cpu")
        qualification.validate_pane_run(allowed, "opengl", ("cursor-only",), 12, True)
        self.assertEqual(allowed["status"], "pass")

        llvmpipe = self.pane_run(
            device_type="unknown",
            device_name="llvmpipe%20%28LLVM%2020.1.8%29",
        )
        qualification.validate_pane_run(llvmpipe, "opengl", ("cursor-only",), 12)
        self.assertEqual(llvmpipe["reason_code"], "software_rhi_device")

    @staticmethod
    def shader_run(dpr: str = "1.25") -> dict[str, object]:
        scale = float(dpr)
        lines = [
            "".join(
                (
                    "qt=6.11.1 platform=wayland graphics_api=vulkan ",
                    "benchmark_contract=1 ",
                    "rhi_backend=Vulkan viewport=1280x720 ",
                    "rhi_device_name=Test%20GPU rhi_device_type=integrated ",
                    "rhi_vendor_id=0x1002 rhi_device_id=0x1234 ",
                    f"framebuffer={round(1280 * scale)}x{round(720 * scale)} ",
                    f"dpr={dpr} warmup=8 iterations=6 ",
                    "completion=offscreen-end-frame",
                )
            )
        ]
        lines.extend(
            f"baseline workload={workload} cpu_total_median_us=1"
            for workload in qualification.SHADER_WORKLOADS
        )
        lines.extend(
            f"renderer={renderer} workload={workload} passes={passes} "
            "cpu_total_median_us=100 measured_frames=6"
            for renderer in qualification.SHADER_RENDERERS
            for workload in qualification.SHADER_WORKLOADS
            for passes in qualification.SHADER_PASS_COUNTS
        )
        return {
            "return_code": 0,
            "records": qualification.parse_records("\n".join(lines)),
        }

    @staticmethod
    def swapchain_output(
        *,
        dpr: str = "1",
        logical: str = "100x100",
        physical: str = "100x100",
        translucent_pixels: int = 6000,
        half_alpha_pixels: int = 5500,
        pixel_count: int = 10000,
        minimum_half_alpha_pixels: int = 500,
        screen_dpr: str | None = None,
    ) -> str:
        effective_screen_dpr = dpr if screen_dpr is None else screen_dpr
        return (
            "renderer_qualification benchmark_contract=1 qt_version=6.11.1 "
            "platform=wayland graphics_api=vulkan "
            f"dpr={dpr} logical={logical} physical={physical} frame_swaps=30 "
            "median_frame_interval_us=16666 p90_frame_interval_us=17000 "
            "screen_count=1 screen_name=eDP-1 screen_manufacturer=Test "
            "screen_model=Panel screen_serial=123 "
            "screen_geometry=0,0,1920x1080 "
            "screen_available_geometry=0,0,1920x1040 "
            "screen_physical_mm=300x170 screen_depth=24 "
            f"screen_dpr={effective_screen_dpr} screen_logical_dpi_x=96 "
            "screen_logical_dpi_y=96 screen_physical_dpi_x=160 "
            "screen_physical_dpi_y=160 screen_refresh_millihz=60000 "
            "screen_orientation=1 screen_primary=1 swapchain_format=sdr "
            f"swapchain_flags=1 swapchain_samples=1 swapchain_size={physical} "
            "swapchain_srgb=0 swapchain_premultiplied_alpha=1 "
            "swapchain_nonpremultiplied_alpha=0 swapchain_no_vsync=0 "
            "swapchain_hdr=0 swapchain_hdr_limits=na "
            "swapchain_hdr_behavior=na swapchain_hdr_minimum=na "
            "swapchain_hdr_maximum=na swapchain_hdr_maximum_potential=na "
            "swapchain_hdr_sdr_white=na alpha_buffer_bits=8 clear_alpha=0 "
            "image_alpha=1 nonuniform=1 min_alpha=128 max_alpha=255 "
            f"translucent_pixels={translucent_pixels} "
            f"half_alpha_pixels={half_alpha_pixels} pixel_count={pixel_count} "
            f"minimum_half_alpha_pixels={minimum_half_alpha_pixels} "
            "panes=1 running_panes=1 rhi_backend=Vulkan "
            "rhi_device_name=Test%20GPU rhi_device_type=integrated "
            "rhi_vendor_id=0x1002 rhi_device_id=0x1234"
        )

    def test_validates_shader_and_swapchain_metadata(self) -> None:
        shader = self.shader_run()
        self.assertEqual(qualification.validate_shader_run(shader, "vulkan", 6), 1.25)
        self.assertEqual(shader["status"], "pass")

        swapchain_output = self.swapchain_output(
            dpr="1.5",
            logical="1100x720",
            physical="1650x1080",
            translucent_pixels=800000,
            half_alpha_pixels=750000,
            pixel_count=1782000,
            minimum_half_alpha_pixels=89100,
        )
        swapchain: dict[str, object] = {
            "return_code": 0,
            "records": qualification.parse_records(swapchain_output),
        }
        self.assertEqual(
            qualification.validate_swapchain_run(swapchain, "vulkan", 30),
            1.5,
        )
        self.assertEqual(swapchain["status"], "pass")

        fractional_window_scale: dict[str, object] = {
            "return_code": 0,
            "records": qualification.parse_records(
                self.swapchain_output(
                    dpr="1.5",
                    logical="1100x720",
                    physical="1650x1080",
                    translucent_pixels=800000,
                    half_alpha_pixels=750000,
                    pixel_count=1782000,
                    minimum_half_alpha_pixels=89100,
                    screen_dpr="2",
                )
            ),
        }
        self.assertEqual(
            qualification.validate_swapchain_run(fractional_window_scale, "vulkan", 30),
            1.5,
        )

        unnamed_unknown_refresh: dict[str, object] = {
            "return_code": 0,
            "records": qualification.parse_records(
                self.swapchain_output()
                .replace("screen_name=eDP-1", "screen_name=")
                .replace("screen_refresh_millihz=60000", "screen_refresh_millihz=0")
            ),
        }
        self.assertEqual(
            qualification.validate_swapchain_run(unnamed_unknown_refresh, "vulkan", 30),
            1,
        )

    def test_rejects_incomplete_shader_matrix(self) -> None:
        missing = self.shader_run()
        records = missing["records"]
        assert isinstance(records, list)
        records.pop()
        qualification.validate_shader_run(missing, "vulkan", 6)
        self.assertEqual(missing["status"], "fail")
        self.assertEqual(missing["reason_code"], "incomplete_shader_matrix")

        duplicate = self.shader_run()
        records = duplicate["records"]
        assert isinstance(records, list)
        records[-1] = dict(records[-2])
        qualification.validate_shader_run(duplicate, "vulkan", 6)
        self.assertEqual(duplicate["reason_code"], "incomplete_shader_matrix")

        software = self.shader_run()
        records = software["records"]
        assert isinstance(records, list)
        records[0]["rhi_device_type"] = "cpu"
        qualification.validate_shader_run(software, "vulkan", 6)
        self.assertEqual(software["reason_code"], "software_rhi_device")

    def test_rejects_truncated_swapchain_evidence_and_accepts_skip(self) -> None:
        truncated: dict[str, object] = {
            "return_code": 0,
            "records": qualification.parse_records(
                "renderer_qualification platform=wayland graphics_api=vulkan "
                "dpr=1 frame_swaps=30 alpha_buffer_bits=8 clear_alpha=0"
            ),
        }
        qualification.validate_swapchain_run(truncated, "vulkan", 30)
        self.assertEqual(truncated["reason_code"], "invalid_presented_surface")

        unavailable: dict[str, object] = {"return_code": 77, "records": []}
        qualification.validate_swapchain_run(unavailable, "vulkan", 30)
        self.assertEqual(unavailable["status"], "skip")
        self.assertEqual(unavailable["reason_code"], "unsupported_swapchain")

        transparent: dict[str, object] = {
            "return_code": 0,
            "records": qualification.parse_records(
                self.swapchain_output(
                    translucent_pixels=0,
                    half_alpha_pixels=0,
                )
            ),
        }
        qualification.validate_swapchain_run(transparent, "vulkan", 30)
        self.assertEqual(transparent["reason_code"], "invalid_presented_surface")

        edge_only: dict[str, object] = {
            "return_code": 0,
            "records": qualification.parse_records(
                self.swapchain_output(
                    translucent_pixels=10,
                    half_alpha_pixels=1,
                )
            ),
        }
        qualification.validate_swapchain_run(edge_only, "vulkan", 30)
        self.assertEqual(edge_only["reason_code"], "invalid_presented_surface")

        inconsistent_counts: dict[str, object] = {
            "return_code": 0,
            "records": qualification.parse_records(
                self.swapchain_output(
                    translucent_pixels=400,
                    half_alpha_pixels=600,
                    pixel_count=9999,
                )
            ),
        }
        qualification.validate_swapchain_run(inconsistent_counts, "vulkan", 30)
        self.assertEqual(
            inconsistent_counts["reason_code"], "invalid_presented_surface"
        )

    def test_rejects_internally_inconsistent_swapchain_metadata(self) -> None:
        mutations = (
            ("swapchain_format=sdr", "swapchain_format=hdr10"),
            ("swapchain_flags=1", "swapchain_flags=0"),
            (
                "swapchain_nonpremultiplied_alpha=0",
                "swapchain_nonpremultiplied_alpha=1",
            ),
            ("swapchain_srgb=0", "swapchain_srgb=1"),
            ("swapchain_no_vsync=0", "swapchain_no_vsync=1"),
            ("swapchain_hdr_limits=na", "swapchain_hdr_limits=nits"),
        )
        for original, replacement in mutations:
            with self.subTest(replacement=replacement):
                run: dict[str, object] = {
                    "return_code": 0,
                    "records": qualification.parse_records(
                        self.swapchain_output().replace(original, replacement)
                    ),
                }
                qualification.validate_swapchain_run(run, "vulkan", 30)
                self.assertEqual(run["reason_code"], "invalid_presented_surface")

    def test_accepts_coherent_hdr_swapchain_metadata(self) -> None:
        output = self.swapchain_output()
        replacements = (
            ("swapchain_format=sdr", "swapchain_format=hdr10"),
            ("swapchain_hdr=0", "swapchain_hdr=1"),
            ("swapchain_hdr_limits=na", "swapchain_hdr_limits=nits"),
            ("swapchain_hdr_behavior=na", "swapchain_hdr_behavior=display-referred"),
            ("swapchain_hdr_minimum=na", "swapchain_hdr_minimum=0.01"),
            ("swapchain_hdr_maximum=na", "swapchain_hdr_maximum=1000"),
            ("swapchain_hdr_sdr_white=na", "swapchain_hdr_sdr_white=203"),
        )
        for original, replacement in replacements:
            output = output.replace(original, replacement)
        run: dict[str, object] = {
            "return_code": 0,
            "records": qualification.parse_records(output),
        }
        self.assertEqual(
            qualification.validate_swapchain_run(run, "vulkan", 30),
            1,
        )

    def test_backend_fallback_is_not_an_optional_skip(self) -> None:
        pane: dict[str, object] = {
            "return_code": 1,
            "stdout": "requested opengl, but Qt selected graphics API 1",
            "stderr": "",
            "records": [],
        }
        qualification.validate_pane_run(pane, "opengl", ("cursor-only",), 12)
        self.assertEqual(pane["status"], "fail")
        self.assertEqual(pane["reason_code"], "backend_fallback")

        shader: dict[str, object] = {
            "return_code": 1,
            "stderr": "vulkan RHI was requested, but Qt selected graphics API 1",
            "records": [],
        }
        qualification.validate_shader_run(shader, "vulkan", 6)
        self.assertEqual(shader["status"], "fail")
        self.assertEqual(shader["reason_code"], "backend_fallback")

    def test_scale_ratio_marks_only_mismatched_run(self) -> None:
        base = self.pane_run("1")
        scaled = self.pane_run("1.1")
        qualification.validate_pane_run(base, "opengl", ("cursor-only",), 12)
        qualification.validate_pane_run(scaled, "opengl", ("cursor-only",), 12)
        qualification.validate_scale_ratios((("1", base, 1.0), ("1.25", scaled, 1.1)))
        self.assertEqual(base["status"], "pass")
        self.assertEqual(scaled["status"], "fail")
        self.assertEqual(scaled["reason_code"], "dpr_scale_mismatch")

    def test_rejects_device_changes_between_scale_runs(self) -> None:
        base = self.pane_run("1", device_name="Integrated%20GPU")
        scaled = self.pane_run("1.25", device_name="Discrete%20GPU")
        qualification.validate_pane_run(base, "opengl", ("cursor-only",), 12)
        qualification.validate_pane_run(scaled, "opengl", ("cursor-only",), 12)
        expected = qualification.validate_scale_devices(
            (("1", base, 1.0), ("1.25", scaled, 1.25))
        )
        self.assertIsNone(expected)
        self.assertEqual(base["status"], "pass")
        self.assertEqual(scaled["status"], "fail")
        self.assertEqual(scaled["reason_code"], "inconsistent_scale_rhi_device")

        pane = self.pane_run("1")
        shader = self.shader_run("1")
        qualification.validate_pane_run(pane, "opengl", ("cursor-only",), 12)
        qualification.validate_shader_run(shader, "vulkan", 6)
        expected = qualification.validate_scale_devices(
            (("1", pane, 1.0), ("1", shader, 1.0))
        )
        self.assertIsNone(expected)
        self.assertEqual(shader["reason_code"], "inconsistent_scale_rhi_device")

    def test_timeout_output_remains_json_serializable(self) -> None:
        run = qualification.run_command(
            "timeout",
            (
                sys.executable,
                "-c",
                "import sys,time; print('out', flush=True); "
                + "print('err', file=sys.stderr, flush=True); "
                + "time.sleep(1)",
            ),
            ROOT,
            os.environ,
            timeout=0.05,
        )
        self.assertEqual(run["reason_code"], "timeout")
        self.assertIsInstance(run["stdout"], str)
        self.assertIsInstance(run["stderr"], str)
        json.dumps(run)

    def test_timeout_terminates_descendant_process_group(self) -> None:
        run = qualification.run_command(
            "descendant-timeout",
            (
                sys.executable,
                "-c",
                "import subprocess,sys,time; "
                + "child=subprocess.Popen([sys.executable,'-c',"
                + "'import time; time.sleep(30)']); "
                + "print(child.pid, flush=True); time.sleep(30)",
            ),
            ROOT,
            os.environ,
            timeout=0.1,
        )
        self.assertEqual(run["reason_code"], "timeout")
        child_pid = int(str(run["stdout"]).splitlines()[0])
        process_path = Path("/proc") / str(child_pid)
        for _ in range(50):
            if not process_path.exists():
                break
            fields = (process_path / "stat").read_text(encoding="utf-8").split()
            if len(fields) > 2 and fields[2] == "Z":
                break
            time.sleep(0.02)
        else:
            self.fail(f"timed-out descendant {child_pid} is still running")

    def test_atomic_json_report_replaces_prior_content(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "result.json"
            qualification.atomic_write_json(path, {"status": "running"})
            qualification.atomic_write_json(path, {"status": "pass"})
            self.assertEqual(
                json.loads(path.read_text(encoding="utf-8")),
                {"status": "pass"},
            )
            self.assertEqual(list(path.parent.glob(".*.tmp")), [])

    def test_termination_signal_uses_cleanup_exception(self) -> None:
        with self.assertRaises(qualification.HarnessTermination) as caught:
            qualification.handle_termination_signal(signal.SIGTERM, None)
        self.assertEqual(caught.exception.signum, signal.SIGTERM)

    def test_termination_signal_kills_child_and_finalizes_report(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            child_pid_path = root / "child.pid"
            report_path = root / "results.json"
            prior_path = qualification.ACTIVE_REPORT_PATH
            prior_report = qualification.ACTIVE_REPORT
            prior_handler = signal.signal(
                signal.SIGTERM, qualification.handle_termination_signal
            )
            qualification.ACTIVE_REPORT_PATH = report_path
            qualification.ACTIVE_REPORT = {"status": "running"}

            def terminate_when_child_starts() -> None:
                for _ in range(200):
                    if child_pid_path.exists():
                        break
                    time.sleep(0.005)
                os.kill(os.getpid(), signal.SIGTERM)

            terminator = threading.Thread(target=terminate_when_child_starts)
            terminator.start()
            try:
                with self.assertRaises(qualification.HarnessTermination) as caught:
                    qualification.run_command(
                        "signal-cleanup",
                        (
                            sys.executable,
                            "-c",
                            "import os,pathlib,time; "
                            + f"pathlib.Path({str(child_pid_path)!r}).write_text("
                            + "str(os.getpid()), encoding='utf-8'); time.sleep(30)",
                        ),
                        ROOT,
                        os.environ,
                    )
                exit_code = qualification.finish_termination(caught.exception)
            finally:
                terminator.join()
                signal.signal(signal.SIGTERM, prior_handler)
                qualification.ACTIVE_REPORT_PATH = prior_path
                qualification.ACTIVE_REPORT = prior_report

            self.assertEqual(exit_code, 128 + signal.SIGTERM)
            self.assertEqual(
                json.loads(report_path.read_text(encoding="utf-8"))["reason_code"],
                "terminated",
            )
            child_pid = int(child_pid_path.read_text(encoding="utf-8"))
            process_path = Path("/proc") / str(child_pid)
            for _ in range(50):
                if not process_path.exists():
                    break
                fields = (process_path / "stat").read_text(encoding="utf-8").split()
                if len(fields) > 2 and fields[2] == "Z":
                    break
                time.sleep(0.02)
            else:
                self.fail(f"signal-cancelled child {child_pid} is still running")


if __name__ == "__main__":
    unittest.main()
