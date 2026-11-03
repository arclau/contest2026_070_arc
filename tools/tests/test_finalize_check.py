#!/usr/bin/env python3
"""Behavior tests on isolated fixtures; never touch the real workspace."""

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
SCRIPT = Path(__file__).resolve().parents[1] / "finalize_check.py"
spec = importlib.util.spec_from_file_location("finalize_check", SCRIPT)
finalize = importlib.util.module_from_spec(spec)
spec.loader.exec_module(finalize)

SID = "test-session-0001"


def tree_state(root):
    result = {}
    for path in root.rglob("*"):
        if path.is_file():
            st = path.stat()
            result[str(path.relative_to(root))] = (
                st.st_size, st.st_mtime_ns, hashlib.sha256(path.read_bytes()).hexdigest())
    return result


def event(seq, **overrides):
    base = {"schema_version": "1.0", "session_id": SID, "team_id": "t",
            "github_login": "arclau", "tool": "opencode", "seq": seq,
            "ts": "2026-10-16T00:00:00Z", "role": "user"}
    base.update(overrides)
    return base


class FinalizeTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="dm-finalize-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "project_docs").mkdir()
        self.logs = self.root / finalize.LOGS_SUBDIR / "arclau"
        self.day = self.logs / "2026-10-16"
        self.day.mkdir(parents=True)
        self.stub_workspace = self.stub("workspace_stub.py",
                                        'import json\nprint(json.dumps({"exit_code": 0, "repositories": [], "findings": []}))\n')
        self.stub_doc = self.stub("doc_stub.py",
                                  'import json\nprint(json.dumps({"exit_code": 0, "findings": []}))\n')
        self.stub_sync = self.stub("sync_stub.sh", 'echo "== docs 镜像一致 =="\n')
        for name, value in (("WORKSPACE_CHECK", self.stub_workspace),
                            ("DOC_CHECK", self.stub_doc), ("SYNC_DOCS", self.stub_sync)):
            patcher = patch.object(finalize, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)

    def stub(self, name, body):
        path = self.root / name
        path.write_text(body)
        return path

    def write_log(self, events, file_name=f"opencode__{SID}.jsonl"):
        path = self.day / file_name
        path.write_text("\n".join(json.dumps(e) for e in events) + ("\n" if events else ""))
        return path

    def write_manifest(self, **overrides):
        entry = {"session_id": SID, "tool": "opencode", "event_count": 2, "health": "ok",
                 "file_path": f"logs/arclau/2026-10-16/opencode__{SID}.jsonl"}
        entry.update(overrides)
        (self.logs / "manifest.json").write_text(json.dumps({"sessions": [entry]}))

    def test_all_ok(self):
        self.write_log([event(1), event(2)])
        self.write_manifest()
        report = finalize.inspect(self.root, SID)
        self.assertEqual(report["exit_code"], 0, report["findings"])
        self.assertEqual([s["status"] for s in report["sections"].values()], ["OK", "OK", "OK"])

    def test_logs_missing_session(self):
        self.write_manifest(session_id="someone-else")
        report = finalize.inspect(self.root, SID)
        self.assertEqual(report["sections"]["logs"]["status"], "MISSING")
        self.assertEqual(report["exit_code"], 1)

    def test_logs_unregistered_file(self):
        self.write_log([event(1), event(2)])
        self.write_manifest(session_id="someone-else")
        report = finalize.inspect(self.root, SID)
        self.assertEqual(report["sections"]["logs"]["status"], "WARN")
        self.assertIn("not registered", report["sections"]["logs"]["detail"])

    def test_logs_unknown_session_id(self):
        self.write_manifest()
        report = finalize.inspect(self.root, None)
        self.assertEqual(report["sections"]["logs"]["status"], "MISSING")
        self.assertIn("session id unknown", report["sections"]["logs"]["detail"])

    def test_logs_zero_events(self):
        self.write_log([])
        self.write_manifest(event_count=0)
        report = finalize.inspect(self.root, SID)
        self.assertEqual(report["sections"]["logs"]["status"], "MISSING")
        self.assertIn("0 events", report["sections"]["logs"]["detail"])

    def test_logs_missing_field_and_bad_seq(self):
        self.write_log([event(1, role=None), event(1)])
        self.write_manifest()
        report = finalize.inspect(self.root, SID)
        detail = report["sections"]["logs"]["detail"]
        self.assertEqual(report["sections"]["logs"]["status"], "WARN")
        self.assertIn("seq", detail)

    def test_logs_manifest_count_mismatch(self):
        self.write_log([event(1), event(2)])
        self.write_manifest(event_count=99)
        report = finalize.inspect(self.root, SID)
        self.assertEqual(report["sections"]["logs"]["status"], "WARN")
        self.assertIn("event_count", report["sections"]["logs"]["detail"])

    def test_docs_mirror_diff(self):
        self.stub_sync.write_text('echo "  DIFF  specs"\n')
        report = finalize.inspect(self.root, SID)
        self.assertEqual(report["sections"]["docs"]["status"], "WARN")
        self.assertIn("specs", report["sections"]["docs"]["detail"])

    def test_docs_mirror_missing_script(self):
        with patch.object(finalize, "SYNC_DOCS", self.root / "nope.sh"):
            report = finalize.inspect(self.root, SID)
        self.assertEqual(report["sections"]["docs"]["status"], "MISSING")

    def test_code_warn_on_nonzero_exit(self):
        self.stub_workspace.write_text(
            'import json\nprint(json.dumps({"exit_code": 1, "repositories": '
            '[{"path": "nuttx", "changes": [{"status": " M", "path": "x.c"}]}], "findings": []}))\n')
        report = finalize.inspect(self.root, SID)
        self.assertEqual(report["sections"]["code"]["status"], "WARN")
        self.assertIn("nuttx", report["sections"]["code"]["evidence"]["dirty_repos"])

    def test_read_only(self):
        self.write_log([event(1), event(2)])
        self.write_manifest()
        before = tree_state(self.root)
        finalize.inspect(self.root, SID)
        self.assertEqual(before, tree_state(self.root))

    def test_cli_env_session_and_invalid_root(self):
        self.write_log([event(1), event(2)])
        self.write_manifest()
        env = {**os.environ, "CODEBUDDY_SESSION_ID": SID}
        result = subprocess.run(
            [sys.executable, "-B", str(SCRIPT), "--root", str(self.root), "--json",
             "--workspace-check", str(self.stub_workspace), "--doc-check", str(self.stub_doc),
             "--sync-docs", str(self.stub_sync)],
            cwd="/tmp", capture_output=True, text=True, env=env)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["session_id"], SID)
        result = subprocess.run([sys.executable, "-B", str(SCRIPT), "--root", str(self.root / "missing")],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
