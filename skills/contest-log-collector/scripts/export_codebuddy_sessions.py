#!/usr/bin/env python3
"""Export /data/dm CodeBuddy sessions into the local contest repository.

It preserves user and assistant messages plus tool event order. Tool
arguments/results are withheld; sensitive message blocks are replaced.
Raw CodeBuddy JSONL stays on this host. This tool never commits or pushes.
"""

from __future__ import annotations

import argparse
import json
import os
import re
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path

from export_codex_rollouts import (
    SENSITIVE,
    TEAM_REPO,
    atomic_json,
    atomic_jsonl,
    clean_text,
    within_workspace,
)

SOURCE = Path.home() / ".codebuddy" / "projects" / "data-dm"
SESSION_ID = re.compile(r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")


def iso_from_ms(value: object) -> str:
    if not isinstance(value, int) or not (1_700_000_000_000 < value < 2_000_000_000_000):
        raise ValueError("invalid timestamp")
    return datetime.fromtimestamp(value / 1000, timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def source_lines(path: Path):
    with path.open(encoding="utf-8") as handle:
        yield from enumerate(handle, 1)


def convert(path: Path, team_id: str, login: str) -> tuple[str, list[dict], int, int]:
    session_id = path.stem
    if not SESSION_ID.fullmatch(session_id):
        raise ValueError("invalid session id")
    events: list[dict] = []
    calls: dict[str, tuple[str, str]] = {}
    raw_count = 0
    redacted_blocks = 0
    observed_cwd = False

    for line_no, line in source_lines(path):
        raw_count += 1
        raw = json.loads(line)
        seen_id = raw.get("sessionId")
        if seen_id and seen_id != session_id:
            raise ValueError("session id mismatch")
        cwd = raw.get("cwd")
        if cwd:
            observed_cwd = True
            if not within_workspace(cwd):
                raise ValueError("event cwd outside workspace")
        kind = raw.get("type")
        if kind not in ("message", "function_call", "function_call_result"):
            continue
        ts = iso_from_ms(raw.get("timestamp"))

        def emit(role: str, **fields: object) -> None:
            events.append({
                "schema_version": "1.0",
                "session_id": session_id,
                "team_id": team_id,
                "github_login": login,
                "tool": "codebuddy",
                "seq": len(events),
                "ts": ts,
                "cwd": "/data/dm",
                "role": role,
                "metadata": {"source_line": line_no, "source_type": kind},
                **fields,
            })

        if kind == "message" and raw.get("role") in ("user", "assistant"):
            role = raw["role"]
            for block in raw.get("content") or []:
                if not isinstance(block, dict) or block.get("type") not in ("input_text", "output_text"):
                    continue
                content = block.get("text")
                if not isinstance(content, str) or not content:
                    continue
                safe_text, hits = clean_text(content)
                emit(role, text=safe_text)
                if hits:
                    events[-1]["metadata"]["message_redacted"] = True
                    events[-1]["redacted_count"] = hits
                    redacted_blocks += 1
        elif kind == "function_call":
            call_id = raw.get("callId")
            name = raw.get("name")
            if not isinstance(call_id, str) or not call_id:
                raise ValueError("tool call missing callId")
            if not isinstance(name, str) or not name:
                raise ValueError("tool call missing name")
            safe_name, name_hits = clean_text(name)
            if name_hits:
                safe_name = "<redacted tool name>"
            safe_id = f"call_{len(calls) + 1}"
            calls[call_id] = (safe_name, safe_id)
            emit("tool", tool_name=safe_name, tool_call_id=safe_id, input=None, output=None)
            events[-1]["metadata"]["tool_payload_withheld"] = True
        elif kind == "function_call_result":
            call_id = raw.get("callId")
            if not isinstance(call_id, str) or not call_id:
                raise ValueError("tool result missing callId")
            name, safe_id = calls.get(call_id, ("<result>", f"result_{line_no}"))
            emit("tool", tool_name=name, tool_call_id=safe_id, input=None, output=None)
            events[-1]["metadata"]["tool_payload_withheld"] = True
            events[-1]["metadata"]["tool_result"] = True

    if not observed_cwd:
        raise ValueError("session has no cwd evidence")
    roles = Counter(event["role"] for event in events)
    if not roles["user"] or not roles["assistant"]:
        raise ValueError("missing user/assistant messages")
    if any(pattern.search(event.get(field, ""))
           for event in events for field in ("text", "tool_name") for pattern in SENSITIVE):
        raise ValueError("sensitive content remained after redaction")
    return session_id, events, raw_count, redacted_blocks


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dest", type=Path, default=TEAM_REPO, help="local contest repository")
    parser.add_argument("--session", help="only this CodeBuddy session ID")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--refresh", action="store_true")
    args = parser.parse_args()
    destination = args.dest.resolve()
    if destination != TEAM_REPO:
        parser.error("destination must be the local contest repository")
    os.umask(0o077)
    manifest_path = destination / "logs" / "arclau" / "manifest.json"
    if manifest_path.exists():
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    else:
        manifest = {
            "schema_version": "1.0",
            "team_id": "contest2026_070_arc",
            "github_login": "arclau",
            "generator": "codebuddy-local-backfill@1",
            "sessions": [],
        }
    team_id = manifest["team_id"]
    login = manifest["github_login"]
    known = {entry["session_id"]: entry for entry in manifest.get("sessions", [])}
    totals = Counter()
    for source in sorted(SOURCE.glob("*.jsonl")):
        if args.session and source.stem != args.session:
            continue
        if source.stem in known and not args.refresh:
            totals["already_present"] += 1
            continue
        try:
            session_id, events, raw_count, redacted_blocks = convert(source, team_id, login)
        except (OSError, UnicodeError, ValueError, json.JSONDecodeError) as exc:
            totals[f"skipped_{str(exc).split(':')[0]}"] += 1
            continue
        rel = f"logs/{login}/{events[0]['ts'][:10]}/codebuddy__{session_id}.jsonl"
        target = destination / rel
        if target.exists() and session_id not in known:
            totals["orphan_conflict"] += 1
            continue
        entry = {
            "session_id": session_id,
            "tool": "codebuddy",
            "started_at": events[0]["ts"],
            "last_event_at": events[-1]["ts"],
            "event_count": len(events),
            "raw_event_count": raw_count,
            "file_path": rel,
            "collection_mode": "cli",
            "health": "degraded",
            "source_format": "codebuddy_jsonl",
            "privacy_mode": "tool_payload_withheld_sensitive_messages_redacted",
            "redacted_count_total": redacted_blocks,
        }
        if not args.dry_run:
            atomic_jsonl(target, events)
            if session_id in known:
                known[session_id].update(entry)
            else:
                manifest["sessions"].append(entry)
                known[session_id] = entry
        totals["exported"] += 1
        totals["events"] += len(events)
        totals["redacted_message_blocks"] += redacted_blocks
    if not args.dry_run and totals["exported"]:
        manifest["updated_at"] = datetime.now(timezone.utc).isoformat()
        atomic_json(manifest_path, manifest)
    print(json.dumps({"dry_run": args.dry_run, "totals": dict(totals)}, ensure_ascii=False))
    return 0 if totals["exported"] or not args.session else 1


if __name__ == "__main__":
    raise SystemExit(main())
