#!/usr/bin/env python3
"""Regression tests for frontend-owned shell completion artifacts."""

from __future__ import annotations

import json
import re
import shlex
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
COMPLETION_DIRECTORY = ROOT / "dist" / "shell-completions"
SPEC_PATH = COMPLETION_DIRECTORY / "spec.json"
GENERATOR = ROOT / "scripts" / "generate-shell-completions.py"


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=ROOT,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def bash_completions(words: list[str]) -> list[str]:
    quoted_words = " ".join(shlex.quote(word) for word in words)
    script = "\n".join(
        [
            f"source {shlex.quote(str(COMPLETION_DIRECTORY / 'ghostty-qt'))}",
            f"COMP_WORDS=({quoted_words})",
            f"COMP_CWORD={len(words) - 1}",
            "_ghostty_qt",
            "printf '%s\\n' \"${COMPREPLY[@]}\"",
        ]
    )
    return [line.rstrip() for line in run(["bash", "-c", script]).stdout.splitlines()]


def pinned_action_option_fields(action: str) -> set[str]:
    filename = action[1:].replace("-", "_") + ".zig"
    source = (ROOT / "ghostty" / "src" / "cli" / filename).read_text(encoding="utf-8")
    marker = "pub const Options = struct {"
    start = source.find(marker)
    if start < 0:
        return set()
    body_start = start + len(marker)
    line_start = source.find("\n", body_start)
    if line_start < 0:
        return set()
    if "}" in source[body_start:line_start]:
        return set()

    depth = 1
    function_signature = False
    function_body_seen = False
    fields: set[str] = set()
    for line in source[line_start + 1 :].splitlines():
        code = line.split("//", 1)[0]
        if depth == 1:
            if re.match(r"\s*pub\s+fn\s+", code):
                function_signature = True
            if not function_signature:
                match = re.match(
                    r'\s*(?:@"([^"]+)"|([a-zA-Z_][a-zA-Z0-9_]*))\s*:',
                    code,
                )
                if match:
                    name = match.group(1) or match.group(2)
                    if not name.startswith("_") and name != "help":
                        fields.add(f"--{name}")
        if function_signature and "{" in code:
            function_body_seen = True
        depth += code.count("{") - code.count("}")
        if function_signature and function_body_seen and depth == 1:
            function_signature = False
            function_body_seen = False
        if depth == 0:
            break
    return fields


class ShellCompletionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.spec = json.loads(SPEC_PATH.read_text(encoding="utf-8"))

    def test_generated_files_are_current(self) -> None:
        run([sys.executable, str(GENERATOR), "--check"])

    def test_actions_match_frontend_catalog_and_parity_manifest(self) -> None:
        header = (ROOT / "src" / "app" / "ghostty_cli_delegation.h").read_text(
            encoding="utf-8"
        )
        catalog = re.findall(r'\{"(\+[a-z0-9-]+)"\s*,', header)
        spec_actions = [action["name"] for action in self.spec["actions"]]
        self.assertEqual(catalog, spec_actions)
        application_ipc_actions = re.findall(
            r'\{"(\+[a-z0-9-]+)"\s*,\s*'
            r"GhosttyCliFrontendSupport::ApplicationIpc\}",
            header,
        )
        self.assertEqual(application_ipc_actions, self.spec["application_ipc_actions"])

        manifest = json.loads(
            (ROOT / "docs" / "ghostty-parity.json").read_text(encoding="utf-8")
        )
        parity_actions = manifest["inventories"]["cli_actions"]["entries"]
        self.assertEqual(sorted(parity_actions), sorted(name[1:] for name in catalog))

    def test_action_options_cover_pinned_public_fields(self) -> None:
        manual_ipc_options = {
            "+new-window": {
                "--working-directory",
                "--command",
                "--shell-integration",
                "--title",
                "-e",
            },
            "+new-tab": {
                "--working-directory",
                "--command",
                "--shell-integration",
                "--title",
                "-e",
            },
        }
        for action in self.spec["actions"]:
            expected = pinned_action_option_fields(action["name"])
            expected.update(manual_ipc_options.get(action["name"], set()))
            actual = {option["name"] for option in action["options"]}
            self.assertEqual(expected, actual, action["name"])

    def test_top_level_options_match_public_frontend_parser(self) -> None:
        source = (ROOT / "src" / "app" / "launch_options.cpp").read_text(
            encoding="utf-8"
        )
        constructors = re.findall(
            r"(?:const\s+)?QCommandLineOption\s+(\w+)\s*\(\s*"
            r"(\{[^}]+\}|QStringLiteral\(\"[^\"]+\"\))\s*,",
            source,
            flags=re.DOTALL,
        )
        hidden = set(
            re.findall(r"(\w+)\.setFlags\(QCommandLineOption::HiddenFromHelp\)", source)
        )
        parser_options: set[str] = {"-e"}
        for variable, name_expression in constructors:
            if variable in hidden:
                continue
            for name in re.findall(r'QStringLiteral\("([^\"]+)"\)', name_expression):
                parser_options.add(f"-{name}" if len(name) == 1 else f"--{name}")

        completion_options = {
            name for option in self.spec["options"] for name in option["names"]
        }
        self.assertEqual(parser_options, completion_options)

    def test_bash_syntax_and_frontend_scoping(self) -> None:
        bash = COMPLETION_DIRECTORY / "ghostty-qt"
        run(["bash", "-n", str(bash)])
        contents = bash.read_text(encoding="utf-8")
        self.assertIn(
            "complete -o nospace -o bashdefault -F _ghostty_qt ghostty-qt", contents
        )
        self.assertNotIn("--gtk-single-instance", contents)
        self.assertNotIn("--theme=", contents)

        config_disabled = COMPLETION_DIRECTORY / "config-disabled" / "ghostty-qt"
        run(["bash", "-n", str(config_disabled)])
        config_disabled_contents = config_disabled.read_text(encoding="utf-8")
        self.assertIn("+new-tab", config_disabled_contents)
        self.assertNotIn("+show-config", config_disabled_contents)

    def test_bash_top_level_action_and_value_completion(self) -> None:
        self.assertEqual(
            ["--single-instance="],
            bash_completions(["ghostty-qt", "--sing"]),
        )
        self.assertIn("+new-tab", bash_completions(["ghostty-qt", "+new-t"]))
        self.assertEqual(
            [
                "--single-instance=false",
                "--single-instance=true",
                "--single-instance=detect",
            ],
            bash_completions(["ghostty-qt", "--single-instance="]),
        )

    def test_bash_action_completion_includes_forwarded_ipc_options(self) -> None:
        self.assertEqual(
            ["--color="],
            bash_completions(["ghostty-qt", "+list-themes", "--c"]),
        )
        self.assertEqual(
            ["--working-directory="],
            bash_completions(["ghostty-qt", "+new-tab", "--w"]),
        )

    @unittest.skipUnless(shutil.which("fish"), "fish is not installed")
    def test_fish_syntax_and_behavior(self) -> None:
        fish = COMPLETION_DIRECTORY / "ghostty-qt.fish"
        run(["fish", "--no-execute", str(fish)])
        script = "\n".join(
            [
                f"source {shlex.quote(str(fish))}",
                "complete -C 'ghostty-qt +show-face --sty'",
            ]
        )
        self.assertIn("--style", run(["fish", "-c", script]).stdout.splitlines()[0])

    @unittest.skipUnless(shutil.which("zsh"), "zsh is not installed")
    def test_zsh_syntax(self) -> None:
        run(["zsh", "-n", str(COMPLETION_DIRECTORY / "_ghostty-qt")])


if __name__ == "__main__":
    unittest.main()
