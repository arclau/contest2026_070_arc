#!/usr/bin/env python3
"""Behavior tests on isolated Git workspaces; never build or touch SDK state."""

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
SCRIPT = Path(__file__).resolve().parents[1] / "workspace_check.py"
spec = importlib.util.spec_from_file_location("workspace_check", SCRIPT)
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


def run_git(path, *args):
    return subprocess.check_output(
        ["git", "-C", str(path), "-c", "user.name=Fixture", "-c",
         "user.email=fixture@example.invalid", *args], stderr=subprocess.DEVNULL,
    )


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


class WorkspaceTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="dm-check-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / ".repo").mkdir()
        (self.root / ".repo/project.list").write_text("\n".join(checker.CORE_REPOS))
        for name in checker.CORE_REPOS:
            self.new_repo(name)
        self.make_target("sim")

    def new_repo(self, name):
        path = self.root / name
        path.mkdir(parents=True, exist_ok=True)
        run_git(path, "init", "-q")
        run_git(path, "commit", "-q", "--allow-empty", "-m", "fixture")
        return path

    def make_target(self, name, overrides=None):
        target = checker.TARGETS[name]
        values = {**target["expected"], **(overrides or {})}
        content = "\n".join(f"# {k} is not set" if v == "n" else f"{k}={v}" for k, v in values.items()) + "\n"
        for key in ("source", "config"):
            path = self.root / target[key]
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
            os.utime(path, (1000, 1000))
        header = bytearray(64)
        header[:4] = b"\x7fELF"
        header[4] = 2 if target["elf_identity"][0] == 64 else 1
        header[5] = 1
        header[18:20] = target["elf_identity"][1].to_bytes(2, "little")
        elf = self.root / target["elf"]
        elf.write_bytes(header)
        os.utime(elf, (2000, 2000))

    def test_read_only_with_staged_rename_dirty_and_untracked(self):
        repo = self.root / "apps"
        old = repo / "old\nname.c"
        old.write_text("one\n")
        run_git(repo, "add", "--", old.name)
        run_git(repo, "commit", "-qm", "add")
        run_git(repo, "mv", "--", old.name, "new file.c")
        (repo / "untracked.txt").write_text("user data")
        before = tree_state(self.root)
        report = checker.inspect(self.root)
        self.assertEqual(before, tree_state(self.root))
        self.assertEqual(report["exit_code"], 0)  # dirty is information, not failure
        changes = next(r for r in report["repositories"] if r["path"] == "apps")["changes"]
        rename = next(c for c in changes if c["status"] == "R ")
        self.assertEqual(rename["from"], "old\nname.c")
        self.assertEqual(rename["path"], "new file.c")

    def test_manifest_and_symlink_owner(self):
        nested = self.new_repo("apps/extra")
        (nested / "file.c").write_text("fixture")
        (self.root / "alias.c").symlink_to(nested / "file.c")
        with (self.root / ".repo/project.list").open("a") as stream:
            stream.write("\napps/extra\n")
        report = checker.inspect(self.root, all_repos=True, paths=["alias.c"])
        self.assertIn("apps/extra", [r["path"] for r in report["repositories"]])
        self.assertEqual(report["path_owners"][0]["repository"], "apps/extra")

    def test_missing_artifact_and_wrong_arch_are_not_success(self):
        elf = self.root / checker.TARGETS["sim"]["elf"]
        elf.unlink()
        self.assertEqual(checker.inspect(self.root)["exit_code"], 1)
        elf.write_bytes(b"not an ELF")
        report = checker.inspect(self.root)
        self.assertEqual(report["exit_code"], 1)
        self.assertTrue(any("architecture" in f["detail"] for f in report["findings"]))

    def test_config_newer_than_elf_and_no_provenance_claim(self):
        config = self.root / checker.TARGETS["sim"]["config"]
        os.utime(config, (3000, 3000))
        report = checker.inspect(self.root)
        self.assertEqual(report["exit_code"], 1)
        self.assertEqual(report["targets"]["sim"]["build_provenance"], "unknown")
        self.assertTrue(any("newer than ELF" in f["detail"] for f in report["findings"]))

    def test_board_sim_enabled_and_config_values_not_leaked(self):
        self.make_target("board", {"CONFIG_LUNCHER_DM_SIM": "y"})
        config = self.root / checker.TARGETS["board"]["config"]
        with config.open("a") as stream:
            stream.write('CONFIG_WIFI_PASSWORD="PRIVATE_SENTINEL"\nCONFIG_INIT_ENTRYPOINT="PRIVATE_SENTINEL"\n')
        report = checker.inspect(self.root, target="board")
        self.assertEqual(report["exit_code"], 1)
        self.assertNotIn("PRIVATE_SENTINEL", json.dumps(report))
        self.assertTrue(any("CONFIG_LUNCHER_DM_SIM" in f["detail"] for f in report["findings"]))

    def test_outside_symlink_rejected(self):
        (self.root / "outside").symlink_to(self.root.parent)
        report = checker.inspect(self.root, paths=["outside"])
        self.assertEqual(report["exit_code"], 2)
        self.assertTrue(any("outside workspace" in f["detail"] for f in report["findings"]))

    def test_missing_manifest_repository_cannot_fall_back_to_parent(self):
        (self.root / "apps/not-a-repo").mkdir()
        with (self.root / ".repo/project.list").open("a") as stream:
            stream.write("\napps/not-a-repo\n")
        report = checker.inspect(self.root, all_repos=True)
        item = next(r for r in report["repositories"] if r["path"] == "apps/not-a-repo")
        self.assertEqual(item["state"], "error")

    def test_git_timeout_is_error_not_clean(self):
        with patch.object(checker.subprocess, "run", side_effect=subprocess.TimeoutExpired("git", 20)):
            item = checker.repo_info(self.root, "apps")
        self.assertEqual(item["state"], "error")
        self.assertNotIn("dirty", item)

    def test_inherited_git_dir_and_fsmonitor_do_not_redirect_or_execute(self):
        repo = self.root / "apps"
        hook = self.root / "unexpected-hook"
        hook.write_text('#!/bin/sh\ntouch "' + str(self.root / "HOOK_EXECUTED") + '"\n')
        hook.chmod(0o755)
        run_git(repo, "config", "core.fsmonitor", str(hook))
        with patch.dict(os.environ, {"GIT_DIR": "/nonexistent", "GIT_WORK_TREE": "/nonexistent"}):
            report = checker.inspect(self.root)
        self.assertEqual(report["exit_code"], 0)
        self.assertFalse((self.root / "HOOK_EXECUTED").exists())

    def test_cli_from_another_directory_and_invalid_root(self):
        result = subprocess.run([sys.executable, "-B", str(SCRIPT), "--root", str(self.root), "--json"],
                                cwd="/tmp", capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["workspace"], str(self.root))
        result = subprocess.run([sys.executable, "-B", str(SCRIPT), "--root", str(self.root / "missing"), "--json"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(json.loads(result.stdout)["exit_code"], 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
