#!/usr/bin/env python3
"""Provenance and privacy checks for CodeBuddy backfill."""

import json
import tempfile
import unittest
from pathlib import Path

from export_codebuddy_sessions import convert
from export_codex_rollouts import REDACTED


SESSION = "11111111-2222-3333-4444-555555555555"
TIME_MS = 1793577600000


def record(kind, **fields):
    return {"type": kind, "sessionId": SESSION, "timestamp": TIME_MS,
            "cwd": "/data/dm", **fields}


class ExportCodeBuddyTest(unittest.TestCase):
    def make_session(self, cwd="/data/dm"):
        secret = "sk-" + "A" * 24
        rows = [
            record("message", role="user", content=[{"type": "input_text", "text": "hello"}]),
            record("function_call", name="Bash", callId="tool-1", arguments=secret),
            record("function_call_result", name="Bash", callId="tool-1",
                   output={"type": "text", "text": secret}),
            record("message", role="assistant",
                   content=[{"type": "output_text", "text": "password=example123"}]),
        ]
        rows[0]["cwd"] = cwd
        directory = tempfile.TemporaryDirectory()
        path = Path(directory.name) / f"{SESSION}.jsonl"
        path.write_text("".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")
        return directory, path, secret

    def test_preserves_order_and_withholds_tool_payload(self):
        directory, path, secret = self.make_session()
        self.addCleanup(directory.cleanup)
        sid, events, raw_count, redacted = convert(path, "contest2026_070_arc", "arclau")
        self.assertEqual((sid, raw_count, redacted), (SESSION, 4, 1))
        self.assertEqual([e["role"] for e in events], ["user", "tool", "tool", "assistant"])
        self.assertEqual([e["seq"] for e in events], list(range(4)))
        self.assertEqual(events[-1]["text"], REDACTED)
        self.assertNotIn(secret, json.dumps(events))
        self.assertTrue(all(e["tool"] == "codebuddy" for e in events))

    def test_rejects_event_outside_workspace(self):
        directory, path, _ = self.make_session("/tmp/private")
        self.addCleanup(directory.cleanup)
        with self.assertRaisesRegex(ValueError, "outside workspace"):
            convert(path, "contest2026_070_arc", "arclau")


if __name__ == "__main__":
    unittest.main()
