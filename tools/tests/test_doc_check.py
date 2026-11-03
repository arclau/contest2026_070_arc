#!/usr/bin/env python3
"""Behavior tests on isolated doc trees; never touch the real workspace."""

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
SCRIPT = Path(__file__).resolve().parents[1] / "doc_check.py"
spec = importlib.util.spec_from_file_location("doc_check", SCRIPT)
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


def tree_state(root):
    result = {}
    for path in root.rglob("*"):
        if path.is_symlink():
            result[str(path.relative_to(root))] = ("link", os.readlink(path))
        elif path.is_file():
            st = path.stat()
            result[str(path.relative_to(root))] = (
                st.st_size, st.st_mtime_ns, hashlib.sha256(path.read_bytes()).hexdigest())
    return result


def categories(report):
    return [item["category"] for item in report["findings"]]


class DocCheckTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="dm-doc-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.write_clean()

    def write(self, relative, text):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def write_clean(self):
        self.write("AGENTS.md", "# Agents\n\nSee [README](project_docs/README.md).\n")
        self.write("devlog.md",
                   "# DevLog\n\n| P | 详情 |\n|---|---|\n| P1 | [详情][p1] |\n\n"
                   "[p1]: project_docs/devlogs/a.md\n")
        self.write("project_docs/README.md", "# Readme\n")
        self.write("project_docs/devlogs/a.md", "# Node A\n")

    def test_read_only_and_clean_fixture(self):
        before = tree_state(self.root)
        report = checker.inspect(self.root)
        self.assertEqual(before, tree_state(self.root))
        self.assertEqual(report["exit_code"], 0, report["findings"])

    def test_broken_link_missing_handoff_and_dangling_reference(self):
        self.write("project_docs/README.md", "# Readme\n\n[dead](missing.md)\n")
        self.write("devlog.md",
                   "# DevLog\n\n| P | 详情 |\n|---|---|\n| P1 | [详情][p1] |\n| P2 | [详情][p2] |\n\n"
                   "[p1]: project_docs/devlogs/gone.md\n")
        report = checker.inspect(self.root)
        self.assertEqual(report["exit_code"], 1)
        self.assertIn("link", categories(report))
        self.assertIn("handoff", categories(report))
        self.assertIn("reference", categories(report))
        self.assertTrue(any("missing target for [p1]" in f["detail"] for f in report["findings"]))

    def test_external_anchor_inline_code_and_fence_ignored(self):
        self.write("project_docs/README.md",
                   "# Readme\n\n[ext](https://example.com/x.md)\n[mail](mailto:a@b.invalid)\n"
                   "[anchor](#section)\n\nSee `[inline](nope.md)` and `` `[tick](nope0.md)` ``.\n\n"
                   "```text\n[fake](nope.md)\n```\n\n~~~\n[also](nope2.md)\n~~~\n")
        report = checker.inspect(self.root)
        self.assertEqual(report["exit_code"], 0, report["findings"])

    def test_line_numbers_survive_fenced_blocks(self):
        self.write("project_docs/README.md",
                   "# Readme\n\n```text\nfiller line\n```\n\n[dead](missing.md)\n")
        report = checker.inspect(self.root)
        finding = next(f for f in report["findings"] if f["category"] == "link")
        self.assertEqual(finding["line"], 7)

    def test_link_escaping_workspace_is_error(self):
        self.write("project_docs/README.md", "# Readme\n\n[out](../../../outside.md)\n")
        report = checker.inspect(self.root)
        self.assertEqual(report["exit_code"], 2)
        self.assertTrue(any("escapes workspace" in f["detail"] for f in report["findings"]))

    def test_budget_byte_line_and_length(self):
        self.write("AGENTS.md", "# Agents\n\n" + ("x" * 9000) + "\n")
        self.write("devlog.md", "# DevLog\n" + "\n".join(f"line {i}" for i in range(45)) + "\n")
        report = checker.inspect(self.root)
        self.assertEqual(report["exit_code"], 1)
        self.assertTrue(any("bytes exceeds" in f["detail"] for f in report["findings"]))
        self.assertTrue(any("lines exceeds" in f["detail"] for f in report["findings"]))
        self.write("devlog.md", "# DevLog\n" + ("y" * 130) + "\n")
        report = checker.inspect(self.root)
        self.assertTrue(any("line length" in f["detail"] for f in report["findings"]))

    def test_orphan_handoff_reported(self):
        self.write("project_docs/devlogs/orphan.md", "# Orphan\n")
        report = checker.inspect(self.root)
        self.assertEqual(report["exit_code"], 1)
        self.assertTrue(any("orphan.md" in f["subject"] and f["category"] == "handoff"
                            for f in report["findings"]))

    def test_archive_and_submission_excluded(self):
        self.write("project_docs/archive/old.md", "[dead](nope.md)\n")
        self.write("project_docs/submission/official/guide.md", "[dead](../nope.md)\n")
        report = checker.inspect(self.root)
        self.assertEqual(report["exit_code"], 0, report["findings"])

    def test_cli_from_another_directory_and_invalid_root(self):
        result = subprocess.run([sys.executable, "-B", str(SCRIPT), "--root", str(self.root), "--json"],
                                cwd="/tmp", capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["workspace"], str(self.root))
        result = subprocess.run([sys.executable, "-B", str(SCRIPT), "--root", str(self.root / "missing"), "--json"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(json.loads(result.stdout)["exit_code"], 2)

    def test_missing_project_docs_is_incomplete(self):
        empty = self.root / "empty"
        empty.mkdir()
        with self.assertRaises(checker.CheckError):
            checker.inspect(empty)


if __name__ == "__main__":
    unittest.main(verbosity=2)
