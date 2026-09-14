#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""One-off AtomCode session extractor for the openvela AI contest logs/.

AtomCode is NOT an officially supported tool of contest-log-collector, so this
script does not add an adapter or touch the official schema/enums. It only
normalizes the raw AtomCode session store

    ~/.atomcode/sessions/<project>/<session-id>.jsonl

into the contest event schema (schema_version 1.0) and writes:

    <repo>/logs/<github_login>/<YYYY-MM-DD>/atomcode__<session-id>.jsonl
    <repo>/logs/<github_login>/manifest.json

Each AtomCode line is one turn: {user, reasoning, assistant, tools[], usage}.
We map it to events: user -> thinking -> assistant -> one event per tool.

Usage:
    python3 skills/contest-log-collector/scripts/backfill_atomcode.py \
        [--repo <demo-repo>] [--output <dir>] [--atomcode-home ~/.atomcode]

By default it writes into --output (or <repo>/logs) so it can be staged.
"""
from __future__ import annotations

import argparse
import json
import os
import re
from datetime import datetime, timezone
from pathlib import Path

SCHEMA_VERSION = "1.0"
TOOL = "atomcode"
DEFAULT_REDACT_RULES = [
    (re.compile(r"sk-[A-Za-z0-9_-]{20,}"), "sk-***REDACTED***"),
    (re.compile(r"ghp_[A-Za-z0-9]{36}"), "ghp_***REDACTED***"),
    (re.compile(r"Bearer\s+[A-Za-z0-9._\-+/=]+"), "Bearer ***REDACTED***"),
]


def iso_from_ms(ms: int) -> str:
    return datetime.fromtimestamp(ms / 1000.0, tz=timezone.utc).strftime(
        "%Y-%m-%dT%H:%M:%S.%f"
    )[:-3] + "Z"


def load_env(repo: Path) -> tuple[str, str]:
    team_id = os.environ.get("TEAM_ID", "")
    login = os.environ.get("GITHUB_LOGIN", "")
    env_file = Path.home() / ".claude" / "contest-collector.env"
    if env_file.is_file():
        for line in env_file.read_text(encoding="utf-8").splitlines():
            if line.startswith("TEAM_ID=") and not team_id:
                team_id = line.split("=", 1)[1].strip()
            elif line.startswith("GITHUB_LOGIN=") and not login:
                login = line.split("=", 1)[1].strip()
    if not team_id:
        raise SystemExit("TEAM_ID not set (env or ~/.claude/contest-collector.env)")
    if not login:
        raise SystemExit("GITHUB_LOGIN not set (env or ~/.claude/contest-collector.env)")
    return team_id, login


def redact(value, rules):
    if isinstance(value, str):
        for pattern, repl in rules:
            value = pattern.sub(repl, value)
        return value
    if isinstance(value, list):
        return [redact(v, rules) for v in value]
    if isinstance(value, dict):
        return {k: redact(v, rules) for k, v in value.items()}
    return value


def parse_args_field(raw):
    if raw is None:
        return None
    if isinstance(raw, (dict, list)):
        return raw
    if isinstance(raw, str):
        s = raw.strip()
        if s[:1] in "{[":
            try:
                return json.loads(s)
            except json.JSONDecodeError:
                return raw
    return raw


def events_for_session(session_id: str, turns: list[dict], team_id: str,
                       login: str, rules) -> tuple[list[dict], str, str]:
    events: list[dict] = []
    seq = 0
    started = None
    last_ts = None

    for turn in turns:
        ts = turn.get("iso")
        if not ts:
            ms = turn.get("ts") or turn.get("started_at")
            ts = iso_from_ms(int(ms)) if ms else None
        if not ts:
            continue
        started = started or ts
        last_ts = ts

        cwd = turn.get("working_dir")
        usage = turn.get("usage") or {}
        base = {"ts": ts, "cwd": cwd} if cwd else {"ts": ts}

        def emit(payload: dict) -> None:
            nonlocal seq
            out = {
                "schema_version": SCHEMA_VERSION,
                "session_id": session_id,
                "team_id": team_id,
                "github_login": login,
                "tool": TOOL,
                "seq": seq,
                **base,
                **payload,
            }
            events.append(redact(out, rules))
            seq += 1

        user = turn.get("user")
        if isinstance(user, str) and user.strip():
            emit({"role": "user", "text": user})

        reasoning = turn.get("reasoning")
        if isinstance(reasoning, str) and reasoning.strip():
            emit({"role": "assistant", "thinking": reasoning})

        assistant = turn.get("assistant")
        if isinstance(assistant, str) and assistant.strip():
            payload = {"role": "assistant", "text": assistant}
            if isinstance(usage.get("prompt"), int):
                payload["tokens_in"] = usage["prompt"]
            if isinstance(usage.get("completion"), int):
                payload["tokens_out"] = usage["completion"]
            emit(payload)

        turn_id = turn.get("turn_id", 0)
        for i, tool in enumerate(turn.get("tools") or []):
            if not isinstance(tool, dict):
                continue
            emit({
                "role": "tool",
                "tool_name": tool.get("name") or "unknown",
                "tool_call_id": f"{session_id}-t{turn_id}-{i}",
                "input": parse_args_field(tool.get("args")),
                "output": tool.get("result"),
            })

    return events, started, last_ts


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default="/data/dm/contest2026_070_arc")
    ap.add_argument("--output", default=None,
                    help="output logs root (default: <repo>/logs)")
    ap.add_argument("--atomcode-home", default=str(Path.home() / ".atomcode"))
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    repo = Path(args.repo).resolve()
    out_root = Path(args.output).resolve() if args.output else repo / "logs"
    home = Path(args.atomcode_home).expanduser()

    team_id, login = load_env(repo)
    member = out_root / login
    rules = DEFAULT_REDACT_RULES

    manifest_path = member / "manifest.json"
    manifest = {
        "schema_version": SCHEMA_VERSION,
        "team_id": team_id,
        "github_login": login,
        "generator": "contest-log-collector/backfill_atomcode (unofficial)",
        "sessions": [],
    }
    if manifest_path.exists():
        try:
            existing = json.loads(manifest_path.read_text(encoding="utf-8"))
            if existing.get("github_login") == login:
                manifest = existing
                manifest.setdefault("sessions", [])
        except json.JSONDecodeError:
            pass

    known = {s.get("session_id") for s in manifest["sessions"]}
    session_files = sorted(home.glob("sessions/*/*.jsonl"))
    added = 0
    total_events = 0
    for path in session_files:
        session_id = path.stem
        if session_id in known:
            continue

        # 仅保留与本作品（openvela / R528）相关的会话；
        # 剔除与之无关的其它项目会话（armbian / rk3576 / wifitest / ...）
        work_dir = ""
        meta_probe = path.with_suffix(".meta")
        if meta_probe.exists():
            try:
                work_dir = json.loads(
                    meta_probe.read_text(encoding="utf-8")
                ).get("working_dir") or ""
            except json.JSONDecodeError:
                work_dir = ""
        if not (work_dir.startswith("/data/dm")
                or work_dir.startswith("/data/openvela")
                or work_dir.startswith("/data/vela")):
            continue

        turns = []
        for line in path.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                turns.append(json.loads(line))
            except json.JSONDecodeError:
                continue
        if not turns:
            continue

        events, started, last_ts = events_for_session(
            session_id, turns, team_id, login, rules
        )
        if not events:
            continue

        date_dir = datetime.fromisoformat(
            started.replace("Z", "+00:00")
        ).strftime("%Y-%m-%d")
        rel = f"logs/{login}/{date_dir}/{TOOL}__{session_id}.jsonl"
        if not args.dry_run:
            dest = member / date_dir / f"{TOOL}__{session_id}.jsonl"
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_text(
                "\n".join(json.dumps(e, ensure_ascii=False) for e in events) + "\n",
                encoding="utf-8",
            )

        meta_path = path.with_suffix(".meta")
        title = None
        if meta_path.exists():
            try:
                title = json.loads(meta_path.read_text(encoding="utf-8")).get("name")
            except json.JSONDecodeError:
                title = None
        entry = {
            "session_id": session_id,
            "tool": TOOL,
            "started_at": started,
            "last_event_at": last_ts,
            "event_count": len(events),
            "file_path": rel,
            "collection_mode": "cli",
            "health": "ok",
        }
        if title:
            entry["title"] = title
        manifest["sessions"].append(entry)
        added += 1
        total_events += len(events)

    manifest["updated_at"] = datetime.now(timezone.utc).isoformat()
    if not args.dry_run:
        member.mkdir(parents=True, exist_ok=True)
        manifest_path.write_text(
            json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )

    print(f"atomcode sessions found : {len(session_files)}")
    print(f"new sessions written    : {added}")
    print(f"events written          : {total_events}")
    print(f"manifest sessions total : {len(manifest['sessions'])}")
    print(f"output                  : {member}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
