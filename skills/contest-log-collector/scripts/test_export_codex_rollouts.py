#!/usr/bin/env python3
"""Privacy and provenance checks for Codex rollout backfill."""

import json
import tempfile
import unittest
from pathlib import Path

from export_codex_rollouts import REDACTED, convert


SESSION = "11111111-2222-3333-4444-555555555555"
TIME = "2026-11-02T00:00:00.000Z"


def record(kind, payload):
    return {"timestamp": TIME, "type": kind, "payload": payload}


class ExportCodexRolloutsTest(unittest.TestCase):
    def make_rollout(self, cwd="/data/dm"):
        secret = "sk-" + "A" * 24
        rows = [
            record("session_meta", {"id": SESSION, "cwd": cwd}),
            record("response_item", {"type": "message", "role": "user",
                                     "content": [{"type": "input_text", "text": "hello"}]}),
            record("response_item", {"type": "custom_tool_call", "name": "functions.exec",
                                     "call_id": "call-1", "input": secret}),
            record("response_item", {"type": "custom_tool_call_output", "call_id": "call-1",
                                     "output": [{"type": "text", "text": secret}]}),
            record("response_item", {"type": "message", "role": "assistant",
                                     "content": [{"type": "output_text", "text": "password=example123"}]}),
        ]
        directory = tempfile.TemporaryDirectory()
        path = Path(directory.name) / f"rollout-{SESSION}.jsonl"
        path.write_text("".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")
        return directory, path, secret

    def test_preserves_real_event_order_and_withholds_sensitive_payloads(self):
        directory, path, secret = self.make_rollout()
        self.addCleanup(directory.cleanup)
        session_id, events, raw_count, redacted = convert(path, "contest2026_070_arc", "arclau")
        self.assertEqual(session_id, SESSION)
        self.assertEqual(raw_count, 5)
        self.assertEqual([e["role"] for e in events], ["user", "tool", "tool", "assistant"])
        self.assertEqual([e["seq"] for e in events], list(range(4)))
        self.assertEqual(events[-1]["text"], REDACTED)
        self.assertEqual(redacted, 1)
        self.assertNotIn(secret, json.dumps(events))
        self.assertTrue(all(e["metadata"]["source_line"] >= 2 for e in events))

    def test_rejects_session_outside_workspace(self):
        directory, path, _ = self.make_rollout("/tmp/private")
        self.addCleanup(directory.cleanup)
        with self.assertRaisesRegex(ValueError, "outside workspace"):
            convert(path, "contest2026_070_arc", "arclau")


if __name__ == "__main__":
    unittest.main()
