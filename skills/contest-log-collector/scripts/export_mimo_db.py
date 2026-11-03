#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Export MiMoCode sessions from mimocode.db into contest logs/.

MiMoCode is the team's CLI agent; contest schema tool enum maps it to "opencode".
Emits schema 1.0 events including role=tool (tool_name / tool_call_id / input / output).

Usage:
  python3 scripts/export_mimo_db.py --list
  python3 scripts/export_mimo_db.py --session <sid> [--repo <demo-repo>]
  python3 scripts/export_mimo_db.py --since 2026-10-14 [--repo <demo-repo>]
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sqlite3
import sys
from datetime import datetime, timezone
from pathlib import Path

SCHEMA_VERSION = "1.0"
TOOL = "opencode"  # official enum; MiMoCode == OpenCode-family CLI
COLLECTION_MODE = "cli"
MIMO_DB = Path.home() / ".local/share/mimocode/mimocode.db"
DEFAULT_REPO = Path("/data/dm/contest2026_070_arc")

REDACT_RULES = [
    (re.compile(r"nvapi-[A-Za-z0-9_-]{10,}"), "nvapi-***REDACTED***"),
    (re.compile(r"sk-[A-Za-z0-9_-]{20,}"), "sk-***REDACTED***"),
    (re.compile(r"ghp_[A-Za-z0-9]{36}"), "ghp_***REDACTED***"),
    (re.compile(r"github_pat_[A-Za-z0-9_]{20,}"), "github_pat_***REDACTED***"),
    (re.compile(r"AKIA[0-9A-Z]{16}"), "AKIA***REDACTED***"),
    (re.compile(r"Bearer\s+[A-Za-z0-9._\-+/=]+"), "Bearer ***REDACTED***"),
    (re.compile(r"AIza[0-9A-Za-z_-]{30,}"), "AIza***REDACTED***"),
    (re.compile(r"ss://[A-Za-z0-9+/=_\-]+"), "ss://***REDACTED***"),
    (re.compile(r"vmess://[A-Za-z0-9+/=_\-]+"), "vmess://***REDACTED***"),
    (re.compile(r"trojan://[A-Za-z0-9+/=_\-:@.]+"), "trojan://***REDACTED***"),
    (re.compile(r"vless://[A-Za-z0-9+/=_\-:@.?&=]+"), "vless://***REDACTED***"),
]


def iso_from_ms(ms: int | None) -> str:
    if not ms:
        ms = 0
    return (
        datetime.fromtimestamp(ms / 1000.0, tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3]
        + "Z"
    )


def redact(value, rules):
    if isinstance(value, str):
        n = 0
        out = value
        for pat, repl in rules:
            out, k = pat.subn(repl, out)
            n += k
        return out, n
    if isinstance(value, list):
        total = 0
        out = []
        for v in value:
            r, n = redact(v, rules)
            out.append(r)
            total += n
        return out, total
    if isinstance(value, dict):
        total = 0
        out = {}
        for k, v in value.items():
            r, n = redact(v, rules)
            out[k] = r
            total += n
        return out, total
    return value, 0


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
        # fall back to existing manifest in repo
        for cand in repo.glob("logs/*/manifest.json"):
            m = json.loads(cand.read_text(encoding="utf-8"))
            team_id = m.get("team_id") or team_id
            login = login or m.get("github_login") or login
    if not login:
        login = "arclau"
    return team_id, login


def connect(db_path: Path) -> sqlite3.Connection:
    con = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
    con.row_factory = sqlite3.Row
    return con


def list_sessions(con: sqlite3.Connection, directory: str | None = None) -> list[dict]:
    if directory:
        sql = """
        SELECT id, title, time_created, time_updated, directory
        FROM session
        WHERE (directory = ? OR directory LIKE ? || '/%')
          AND title NOT LIKE 'checkpoint-writer%'
          AND title NOT LIKE 'Auto %%'
        ORDER BY time_created
        """
        rows = con.execute(sql, (directory, directory)).fetchall()
    else:
        rows = con.execute(
            """
            SELECT id, title, time_created, time_updated, directory
            FROM session
            WHERE title NOT LIKE 'checkpoint-writer%'
              AND title NOT LIKE 'Auto %%'
            ORDER BY time_created
            """
        ).fetchall()
    return [
        {
            "session_id": r["id"],
            "title": r["title"] or "",
            "time_created": r["time_created"],
            "time_updated": r["time_updated"],
        }
        for r in rows
    ]


def extract_events(
    con: sqlite3.Connection, session_id: str, team_id: str, github_login: str
) -> tuple[list[dict], int]:
    events: list[dict] = []
    seq = 0
    redacted_total = 0

    msgs = con.execute(
        """
        SELECT id, time_created, data FROM message
        WHERE session_id = ? ORDER BY time_created, rowid
        """,
        (session_id,),
    ).fetchall()

    for msg in msgs:
        try:
            meta = json.loads(msg["data"] or "{}")
        except json.JSONDecodeError:
            meta = {}
        role = meta.get("role") or "assistant"
        base_ts = iso_from_ms(msg["time_created"])

        parts = con.execute(
            "SELECT data FROM part WHERE message_id = ? ORDER BY rowid", (msg["id"],)
        ).fetchall()

        thinking_buf: list[str] = []
        text_emitted = False

        for (raw,) in parts:
            try:
                part = json.loads(raw or "{}")
            except json.JSONDecodeError:
                continue
            ptype = part.get("type")

            if ptype == "reasoning":
                t = part.get("text") or part.get("reasoning") or ""
                if isinstance(t, str) and t.strip():
                    thinking_buf.append(t)
                continue

            if ptype == "step-start" or ptype == "step-finish":
                continue

            if ptype == "text":
                if part.get("synthetic"):
                    continue
                text = part.get("text") or ""
                text, n = redact(text, REDACT_RULES)
                redacted_total += n
                ev = {
                    "schema_version": SCHEMA_VERSION,
                    "session_id": session_id,
                    "team_id": team_id,
                    "github_login": github_login,
                    "tool": TOOL,
                    "ts": base_ts,
                    "role": role if role in ("user", "assistant", "system") else "assistant",
                    "seq": seq,
                    "text": text,
                }
                if thinking_buf and not text_emitted:
                    th = "\n".join(thinking_buf)
                    th, n = redact(th, REDACT_RULES)
                    redacted_total += n
                    ev["thinking"] = th
                    thinking_buf = []
                events.append(ev)
                seq += 1
                text_emitted = True
                continue

            if ptype == "tool":
                state = part.get("state") or {}
                inp = state.get("input")
                out = state.get("output")
                if out is None:
                    md = state.get("metadata") or {}
                    out = md.get("output")
                inp, n1 = redact(inp, REDACT_RULES)
                out, n2 = redact(out, REDACT_RULES)
                redacted_total += n1 + n2
                ev = {
                    "schema_version": SCHEMA_VERSION,
                    "session_id": session_id,
                    "team_id": team_id,
                    "github_login": github_login,
                    "tool": TOOL,
                    "ts": base_ts,
                    "role": "tool",
                    "seq": seq,
                    "tool_name": part.get("tool") or "unknown",
                    "tool_call_id": part.get("callID") or f"call_{seq}",
                    "input": inp,
                    "output": out,
                }
                st_status = state.get("status")
                if st_status:
                    ev["metadata"] = {"status": st_status}
                events.append(ev)
                seq += 1

        # leftover reasoning without text: attach as assistant thinking-only event
        if thinking_buf and not text_emitted and role == "assistant":
            th = "\n".join(thinking_buf)
            th, n = redact(th, REDACT_RULES)
            redacted_total += n
            events.append(
                {
                    "schema_version": SCHEMA_VERSION,
                    "session_id": session_id,
                    "team_id": team_id,
                    "github_login": github_login,
                    "tool": TOOL,
                    "ts": base_ts,
                    "role": "assistant",
                    "seq": seq,
                    "text": "",
                    "thinking": th,
                }
            )
            seq += 1

    return events, redacted_total


def write_session(
    repo: Path,
    github_login: str,
    session_id: str,
    events: list[dict],
) -> Path:
    if not events:
        raise SystemExit(f"no events extracted for {session_id}")
    ts0 = events[0]["ts"]
    date = ts0[:10]
    out_dir = repo / "logs" / github_login / date
    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / f"{TOOL}__{session_id}.jsonl"
    with out_path.open("w", encoding="utf-8") as f:
        for ev in events:
            f.write(json.dumps(ev, ensure_ascii=False) + "\n")
    return out_path


def upsert_manifest(
    repo: Path,
    github_login: str,
    team_id: str,
    session_id: str,
    events: list[dict],
    file_path: Path,
    model: str | None = None,
    redacted_total: int = 0,
) -> None:
    man_path = repo / "logs" / github_login / "manifest.json"
    if man_path.is_file():
        man = json.loads(man_path.read_text(encoding="utf-8"))
    else:
        man = {
            "schema_version": SCHEMA_VERSION,
            "team_id": team_id,
            "github_login": github_login,
            "sessions": [],
        }
    man["generator"] = "mimo-db-export@1.1.0"
    man["updated_at"] = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"

    rel = str(file_path.relative_to(repo))
    entry = {
        "session_id": session_id,
        "tool": TOOL,
        "started_at": events[0]["ts"],
        "last_event_at": events[-1]["ts"],
        "event_count": len(events),
        "file_path": rel,
        "collection_mode": COLLECTION_MODE,
        "health": "ok",
        "redacted_count_total": redacted_total,
    }
    if model:
        entry["model"] = model

    sessions = man.setdefault("sessions", [])
    for i, s in enumerate(sessions):
        if s.get("session_id") == session_id:
            sessions[i] = {**s, **entry}
            break
    else:
        sessions.append(entry)

    man_path.write_text(json.dumps(man, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    p = argparse.ArgumentParser(description="Export MiMoCode sessions to contest logs/")
    p.add_argument("--db", default=str(MIMO_DB))
    p.add_argument("--repo", default=str(DEFAULT_REPO))
    p.add_argument("--list", action="store_true")
    p.add_argument("--session", action="append", default=[])
    p.add_argument("--since", help="YYYY-MM-DD, export sessions with time_created >= date")
    p.add_argument("--all", action="store_true")
    p.add_argument(
        "--directory",
        help="restrict to sessions whose cwd is this dir (e.g. /data/vela); default all",
    )
    args = p.parse_args()

    db = Path(args.db)
    repo = Path(args.repo)
    if not db.is_file():
        raise SystemExit(f"mimo db not found: {db}")
    if not repo.is_dir():
        raise SystemExit(f"repo not found: {repo}")

    con = connect(db)
    team_id, github_login = load_env(repo)
    sessions = list_sessions(con, args.directory)

    if args.list:
        print(f"{'session_id':<32} {'started':<22} {'title'}")
        for s in sessions:
            print(
                f"{s['session_id'][:30]:<32} "
                f"{iso_from_ms(s['time_created'])[:19]:<22} {s['title'][:50]}"
            )
        return 0

    wanted: list[dict] = []
    if args.session:
        ids = set(args.session)
        wanted = [s for s in sessions if s["session_id"] in ids]
        missing = ids - {s["session_id"] for s in wanted}
        if missing:
            raise SystemExit(f"session not found in db: {missing}")
    elif args.since:
        cutoff = int(
            datetime.fromisoformat(args.since + "T00:00:00+00:00").timestamp() * 1000
        )
        wanted = [s for s in sessions if s["time_created"] >= cutoff]
    elif args.all:
        wanted = sessions
    else:
        raise SystemExit("specify --list / --session / --since / --all")

    for s in wanted:
        events, redacted = extract_events(con, s["session_id"], team_id, github_login)
        if not events:
            print(f"SKIP {s['session_id']} (no events)")
            continue
        path = write_session(repo, github_login, s["session_id"], events)
        upsert_manifest(repo, github_login, team_id, s["session_id"], events, path,
                        redacted_total=redacted)
        roles = {}
        for e in events:
            roles[e["role"]] = roles.get(e["role"], 0) + 1
        print(
            f"OK   {s['session_id']}  events={len(events)} roles={roles} "
            f"redacted={redacted} -> {path.relative_to(repo)}"
        )

    return 0


if __name__ == "__main__":
    sys.exit(main())
