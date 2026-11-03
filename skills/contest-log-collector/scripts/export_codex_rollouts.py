#!/usr/bin/env python3
"""Backfill Codex rollout sessions belonging to /data/dm.

The official Claude-style hook cannot parse Codex response_item records.
This exporter preserves user/assistant text and tool event order from the
rollout. Tool arguments and results are withheld because they can contain
credentials or unrelated private files. Sensitive message blocks are replaced
as a whole. Each exported event carries its original rollout line number.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import tempfile
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path

WORKSPACE = Path("/data/dm")
ROLLOUTS = Path.home() / ".codex" / "sessions"
TEAM_REPO = WORKSPACE / "contest2026_070_arc"
REDACTED = "[REDACTED: sensitive content in original message]"

# Fail closed for message blocks that may hold credentials or personal data.
# Tool input/output is always withheld, including when no pattern matches.
SENSITIVE = [
    re.compile(r"(?:nvapi-|sk-|ghp_|github_pat_|AIza|AKIA|LTAI|AKLT)[A-Za-z0-9_-]{10,}", re.I),
    re.compile(r"Bearer\s+[A-Za-z0-9._+/=-]{12,}", re.I),
    re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----"),
    re.compile(r"\b(?:vmess|vless|trojan|ss)://\S{8,}", re.I),
    re.compile(r"https?://\S+", re.I),
    re.compile(r"(?<!\d)(?:\d{1,3}\.){3}\d{1,3}(?!\d)"),
    re.compile(r"[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}"),
    re.compile(r"(?<!\d)1[3-9]\d{9}(?!\d)"),
    re.compile(r"\d{8,}"),
    re.compile(r"(?:password|passwd|psk|api[_-]?key|secret|token|ssid|订阅|密码|密钥|凭据|手机号|内网)", re.I),
]


def within_workspace(cwd: str) -> bool:
    if not cwd:
        return False
    try:
        return Path(cwd).resolve().is_relative_to(WORKSPACE)
    except (OSError, ValueError):
        return False


def clean_text(value: str) -> tuple[str, int]:
    count = sum(bool(pattern.search(value)) for pattern in SENSITIVE)
    if len(value) > 8192:
        count += 1
    if count:
        return REDACTED, count
    return value, 0


def iso_timestamp(value: str | None) -> str:
    if not isinstance(value, str):
        raise ValueError("missing timestamp")
    datetime.fromisoformat(value.replace("Z", "+00:00"))
    return value


def source_lines(path: Path):
    with path.open(encoding="utf-8") as handle:
        yield from enumerate(handle, 1)


def convert(path: Path, team_id: str, login: str) -> tuple[str, list[dict], int, int]:
    session_id = ""
    cwd = ""
    events: list[dict] = []
    calls: dict[str, tuple[str, str]] = {}
    raw_count = 0
    redacted_blocks = 0
    for line_no, line in source_lines(path):
        raw_count += 1
        raw = json.loads(line)
        payload = raw.get("payload") or {}
        kind = raw.get("type")
        if kind == "session_meta":
            session_id = payload.get("id") or ""
            cwd = payload.get("cwd") or ""
            continue
        if kind == "turn_context":
            turn_cwd = payload.get("cwd")
            if turn_cwd and not within_workspace(turn_cwd):
                raise ValueError("turn cwd outside workspace")
        if kind != "response_item":
            continue
        item_type = payload.get("type")
        ts = iso_timestamp(raw.get("timestamp"))

        def emit(role: str, **fields: object) -> None:
            events.append({
                "schema_version": "1.0",
                "session_id": session_id,
                "team_id": team_id,
                "github_login": login,
                "tool": "codex",
                "seq": len(events),
                "ts": ts,
                "cwd": cwd,
                "role": role,
                "metadata": {"source_line": line_no, "source_type": item_type},
                **fields,
            })

        if item_type == "message" and payload.get("role") in ("user", "assistant"):
            role = payload["role"]
            for block in payload.get("content") or []:
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
        elif item_type == "custom_tool_call":
            call_id = payload.get("call_id")
            name = payload.get("name")
            if not isinstance(call_id, str) or not call_id:
                raise ValueError("tool call missing call_id")
            if not isinstance(name, str) or not name:
                raise ValueError("tool call missing name")
            safe_name, name_hits = clean_text(name)
            if name_hits:
                safe_name = "<redacted tool name>"
            safe_id = f"call_{len(calls) + 1}"
            calls[call_id] = (safe_name, safe_id)
            emit("tool", tool_name=safe_name, tool_call_id=safe_id, input=None, output=None)
            events[-1]["metadata"]["tool_payload_withheld"] = True
        elif item_type == "custom_tool_call_output":
            call_id = payload.get("call_id")
            if not isinstance(call_id, str) or not call_id:
                raise ValueError("tool output missing call_id")
            name, safe_id = calls.get(call_id, ("<result>", f"result_{line_no}"))
            emit("tool", tool_name=name, tool_call_id=safe_id, input=None, output=None)
            events[-1]["metadata"]["tool_payload_withheld"] = True
            events[-1]["metadata"]["tool_result"] = True

    if not session_id or session_id not in path.name:
        raise ValueError("session id mismatch")
    if not re.fullmatch(r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}", session_id):
        raise ValueError("invalid session id")
    if not within_workspace(cwd):
        raise ValueError("session cwd outside workspace")
    if not events:
        raise ValueError("no exportable events")
    roles = Counter(event["role"] for event in events)
    if not all(roles.get(role) for role in ("user", "assistant", "tool")):
        raise ValueError("missing user/assistant/tool role")
    if any(pattern.search(event.get(field, ""))
           for event in events for field in ("text", "tool_name") for pattern in SENSITIVE):
        raise ValueError("sensitive content remained after redaction")
    return session_id, events, raw_count, redacted_blocks


def atomic_json(path: Path, data: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", dir=path.parent, delete=False) as handle:
        temp = Path(handle.name)
        json.dump(data, handle, ensure_ascii=False, indent=2)
        handle.write("\n")
    os.replace(temp, path)


def atomic_jsonl(path: Path, events: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", dir=path.parent, delete=False) as handle:
        temp = Path(handle.name)
        for event in events:
            handle.write(json.dumps(event, ensure_ascii=False) + "\n")
    os.replace(temp, path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--session", help="only this Codex session ID")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--refresh", action="store_true", help="replace a previously exported target session")
    args = parser.parse_args()
    manifest_path = TEAM_REPO / "logs" / "arclau" / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    team_id = manifest["team_id"]
    login = manifest["github_login"]
    known = {entry["session_id"]: entry for entry in manifest.get("sessions", [])}
    totals = Counter()
    for source in sorted(ROLLOUTS.rglob("*.jsonl")):
        if args.session and args.session not in source.name:
            continue
        try:
            with source.open(encoding="utf-8") as handle:
                first = json.loads(handle.readline())
            meta = first.get("payload") or {}
        except (OSError, UnicodeError, json.JSONDecodeError):
            totals["skipped_unreadable_meta"] += 1
            continue
        if first.get("type") != "session_meta" or not within_workspace(meta.get("cwd") or ""):
            totals["other_workspace"] += 1
            continue
        source_id = meta.get("id")
        if source_id in known and not (args.refresh and args.session == source_id):
            totals["already_present"] += 1
            continue
        try:
            session_id, events, raw_count, redacted_blocks = convert(source, team_id, login)
        except (OSError, UnicodeError, ValueError, json.JSONDecodeError) as exc:
            totals[f"skipped_{str(exc).split(':')[0]}"] += 1
            continue
        if session_id in known and not (args.refresh and args.session == session_id):
            totals["already_present"] += 1
            continue
        date = events[0]["ts"][:10]
        rel = f"logs/{login}/{date}/codex__{session_id}.jsonl"
        destination = TEAM_REPO / rel
        if destination.exists() and session_id not in known:
            totals["orphan_conflict"] += 1
            continue
        entry = {
            "session_id": session_id,
            "tool": "codex",
            "started_at": events[0]["ts"],
            "last_event_at": events[-1]["ts"],
            "event_count": len(events),
            "raw_event_count": raw_count,
            "file_path": rel,
            "collection_mode": "cli",
            "health": "degraded",
            "source_format": "codex_response_item",
            "privacy_mode": "tool_payload_withheld_sensitive_messages_redacted",
            "redacted_count_total": redacted_blocks,
        }
        if not args.dry_run:
            atomic_jsonl(destination, events)
            if session_id in known:
                known[session_id].update(entry)
            else:
                manifest.setdefault("sessions", []).append(entry)
                known[session_id] = entry
        totals["exported"] += 1
        totals["events"] += len(events)
        totals["redacted_message_blocks"] += redacted_blocks
    if not args.dry_run and totals["exported"]:
        manifest["updated_at"] = datetime.now(timezone.utc).isoformat()
        atomic_json(manifest_path, manifest)
    print(json.dumps({"dry_run": args.dry_run, "session": args.session, "totals": dict(totals)}, ensure_ascii=False))
    return 0 if totals["exported"] or not args.session else 1


if __name__ == "__main__":
    raise SystemExit(main())
