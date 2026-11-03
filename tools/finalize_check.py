#!/usr/bin/env python3
"""Read-only finalization check. Reports code, docs and session-log status
separately and marks any missing item explicitly. No writes, network or build.
"""

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
import sys

TOOLS_DIR = Path(__file__).resolve().parent
WORKSPACE_CHECK = TOOLS_DIR / "workspace_check.py"
DOC_CHECK = TOOLS_DIR / "doc_check.py"
SYNC_DOCS = Path("/data/vela/sync_docs.sh")
LOGS_SUBDIR = "contest2026_070_arc/logs"
REQUIRED_EVENT_FIELDS = ("schema_version", "session_id", "team_id",
                         "github_login", "tool", "seq", "ts", "role")
SESSION_ENV = ("CODEBUDDY_SESSION_ID", "CLAUDE_SESSION_ID")
LEVELS = {"OK": 0, "WARN": 1, "MISSING": 1, "ERROR": 2}


class CheckError(Exception):
    pass


def section(status, detail, evidence=None):
    return {"status": status, "detail": detail, "evidence": evidence or {}}


def run_json(argv, timeout=120):
    """Run a sibling tool and parse its JSON stdout. Never raises for tool exit."""
    try:
        proc = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=timeout, text=True)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return False, f"{type(exc).__name__}"
    try:
        return True, json.loads(proc.stdout)
    except json.JSONDecodeError:
        return False, f"non-JSON output (exit {proc.returncode})"


def code_section(root):
    if not WORKSPACE_CHECK.is_file():
        return section("MISSING", f"{WORKSPACE_CHECK.name} not found")
    ok, data = run_json([sys.executable, "-B", str(WORKSPACE_CHECK), "--root", str(root), "--json"])
    if not ok:
        return section("ERROR", f"workspace_check could not run: {data}")
    repositories = data.get("repositories", [])
    dirty = sorted(r["path"] for r in repositories if r.get("changes"))
    findings = data.get("findings", [])
    exit_code = data.get("exit_code")
    detail = (f"workspace_check exit={exit_code}; dirty repos={len(dirty)}/{len(repositories)}; "
              f"findings={len(findings)}; build/pack/run NOT checked here")
    status = {0: "OK", 1: "WARN"}.get(exit_code, "ERROR")
    return section(status, detail,
                   {"exit_code": exit_code, "dirty_repos": dirty,
                    "finding_levels": sorted({f["level"] for f in findings})})


def docs_section(root):
    doc = {"available": False}
    if DOC_CHECK.is_file():
        ok, data = run_json([sys.executable, "-B", str(DOC_CHECK), "--root", str(root), "--json"])
        if ok:
            doc = {"available": True, "exit_code": data.get("exit_code"),
                   "findings": len(data.get("findings", []))}
        else:
            doc = {"available": False, "error": data}
    mirror = {"available": False}
    if SYNC_DOCS.is_file():
        try:
            proc = subprocess.run(["bash", str(SYNC_DOCS), "--check"],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  timeout=120, text=True)
            out = proc.stdout
            diff = [line.strip() for line in out.splitlines() if line.strip().startswith("DIFF")]
            mirror = {"available": True, "consistent": ("镜像一致" in out), "diff": diff}
        except (OSError, subprocess.TimeoutExpired) as exc:
            mirror = {"available": False, "error": type(exc).__name__}

    if not mirror["available"]:
        status = "MISSING"
        detail = f"docs mirror check unavailable ({SYNC_DOCS})"
    elif not mirror["consistent"]:
        status = "WARN"
        detail = "docs mirror differs: " + ", ".join(mirror["diff"] or ["unknown"])
    elif doc.get("available") and doc.get("exit_code", 0) != 0:
        status = "WARN"
        detail = f"docs mirror consistent; doc_check exit={doc['exit_code']} ({doc['findings']} findings)"
    elif not doc.get("available"):
        status = "WARN"
        detail = "docs mirror consistent; doc_check unavailable"
    else:
        status = "OK"
        detail = "docs mirror consistent; doc_check clean"
    return section(status, detail, {"doc_check": doc, "mirror": mirror})


def load_sessions(logs_root):
    sessions = []
    for manifest in sorted(logs_root.glob("*/manifest.json")):
        try:
            data = json.loads(manifest.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        for entry in data.get("sessions", []):
            sessions.append(entry)
    return sessions


def validate_log(path, entry):
    """Return (status, detail, evidence) for a located session log."""
    evidence = {"file": str(path), "event_count_manifest": entry.get("event_count") if entry else None}
    try:
        lines = [line for line in path.read_text(encoding="utf-8", errors="replace").splitlines() if line.strip()]
    except OSError as exc:
        return "ERROR", f"session log unreadable ({type(exc).__name__})", evidence
    events = []
    for line in lines:
        try:
            events.append(json.loads(line))
        except json.JSONDecodeError:
            return "WARN", "session log has non-JSON lines", {**evidence, "lines": len(lines)}
    if not events:
        return "MISSING", "session log exists but has 0 events", {**evidence, "events": 0}
    missing = [f for f in REQUIRED_EVENT_FIELDS if f not in events[0]]
    seqs = [e.get("seq") for e in events if isinstance(e.get("seq"), int)]
    monotonic = all(b > a for a, b in zip(seqs, seqs[1:]))
    evidence.update(events=len(events), missing_fields=missing, seq_monotonic=monotonic,
                    tools=sorted({e.get("tool") for e in events if e.get("tool")}))
    if missing:
        return "WARN", f"events missing required fields: {', '.join(missing)}", evidence
    if not monotonic:
        return "WARN", "event seq is not strictly increasing", evidence
    if entry and entry.get("event_count") not in (None, len(events)):
        return "WARN", f"manifest event_count={entry['event_count']} != file events={len(events)}", evidence
    return "OK", f"session captured: {len(events)} events, tool={evidence['tools']}", evidence


def logs_section(root, session_id):
    logs_root = root / LOGS_SUBDIR
    if not logs_root.is_dir():
        return section("MISSING", f"{LOGS_SUBDIR} not found")
    sessions = load_sessions(logs_root)
    files = list(logs_root.rglob(f"*__{session_id}.jsonl")) if session_id else []
    if not session_id:
        return section("MISSING",
                       "session id unknown; pass --session or set CODEBUDDY_SESSION_ID/CLAUDE_SESSION_ID",
                       {"known_sessions": len(sessions)})
    entry = next((s for s in sessions if s.get("session_id") == session_id), None)
    if entry is None and not files:
        return section("MISSING", f"session {session_id} not found in logs/ manifest or files",
                       {"known_sessions": len(sessions)})
    if entry and entry.get("file_path"):
        path = root / LOGS_SUBDIR.split("/")[0] / entry["file_path"]
    else:
        path = files[0]
    if not path.is_file() and files:
        path = files[0]
    if not path.is_file():
        return section("WARN", "manifest lists session but log file is absent",
                       {"file": str(path), "session_id": session_id})
    status, detail, evidence = validate_log(path, entry)
    if entry is None and status == "OK":
        status = "WARN"
        detail = "session file present but not registered in manifest; " + detail
    return section(status, detail, {"session_id": session_id, **evidence})


def inspect(root, session_id):
    report = {"schema_version": 1, "observed_at": datetime.now(timezone.utc).isoformat(),
              "workspace": str(root), "session_id": session_id, "sections": {}, "findings": [],
              "limitations": [
                  "Read-only orchestration of sibling tools; it does not build, pack or run anything.",
                  "Code section proves environment/baseline identity only, never a build/run verdict.",
                  "Docs section delegates to sync_docs.sh --check and doc_check.py; it does not export.",
                  "Logs section reads logs/ manifest and one session file; a missing session is reported, not fixed.",
                  "Not an atomic snapshot; stop concurrent writers for a baseline.",
              ]}
    report["sections"]["code"] = code_section(root)
    report["sections"]["docs"] = docs_section(root)
    report["sections"]["logs"] = logs_section(root, session_id)
    worst = max(LEVELS[item["status"]] for item in report["sections"].values())
    report["exit_code"] = worst
    for name, item in report["sections"].items():
        if item["status"] != "OK":
            report["findings"].append({"section": name, "status": item["status"], "detail": item["detail"]})
    return report


def print_text(report):
    print(f"Finalize check: {report['workspace']} | session={report['session_id'] or 'unknown'}")
    for name in ("code", "docs", "logs"):
        item = report["sections"][name]
        print(f"  {name.upper():<5} {item['status']:<7} {item['detail']}")
    print(f"Check exit={report['exit_code']} (0=all ok, 1=needs attention/missing, 2=incomplete/error)")
    for limitation in report["limitations"]:
        print("  NOTE " + limitation)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, help="workspace root; defaults to script ancestor with project_docs")
    parser.add_argument("--session", help="session id to verify in logs/; defaults to CODEBUDDY_SESSION_ID/CLAUDE_SESSION_ID")
    parser.add_argument("--workspace-check", type=Path, help="override workspace_check.py path")
    parser.add_argument("--doc-check", type=Path, help="override doc_check.py path")
    parser.add_argument("--sync-docs", type=Path, help="override sync_docs.sh path")
    parser.add_argument("--json", action="store_true", help="machine-readable report on stdout")
    args = parser.parse_args()
    if args.workspace_check:
        global WORKSPACE_CHECK
        WORKSPACE_CHECK = args.workspace_check
    if args.doc_check:
        global DOC_CHECK
        DOC_CHECK = args.doc_check
    if args.sync_docs:
        global SYNC_DOCS
        SYNC_DOCS = args.sync_docs
    session_id = args.session or next((os.environ[k] for k in SESSION_ENV if os.environ.get(k)), None)
    try:
        root = args.root.resolve() if args.root else next(
            (p for p in Path(__file__).resolve().parents if (p / "project_docs").is_dir()), None)
        if root is None or not (root / "project_docs").is_dir():
            raise CheckError("workspace project_docs not found; use --root")
        report = inspect(root, session_id)
    except (OSError, CheckError) as exc:
        print(json.dumps({"schema_version": 1, "exit_code": 2,
                          "error": str(exc) if isinstance(exc, CheckError) else type(exc).__name__}))
        return 2
    if args.json:
        print(json.dumps(report, ensure_ascii=False, indent=2))
    else:
        print_text(report)
    return report["exit_code"]


if __name__ == "__main__":
    sys.exit(main())
