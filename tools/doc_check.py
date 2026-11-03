#!/usr/bin/env python3
"""Read-only documentation consistency check. No writes, git, network or build.

Finds broken relative links, over-budget index files and missing handoff
documents in the DesktopMate docs (root AGENTS.md / devlog.md and
project_docs/, excluding the historical archive). Reports only; never edits.
"""

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import sys
from urllib.parse import unquote, urlsplit

# Root docs plus the exported source tree. archive/ is historical and
# submission/ is third-party official material: both are outside the maintained
# doc set (see sync_docs.sh), so their dead links are not reported.
ROOT_DOCS = ("AGENTS.md", "devlog.md")
DOCS_DIR = "project_docs"
EXCLUDE_DIRS = ("archive", "submission")
SKIP_SCHEMES = ("http", "https", "mailto", "ftp", "tel", "data", "javascript")

# Budgets mirror AGENTS.md section 4 (single source of the rules); keep in sync.
BUDGETS = {
    "AGENTS.md": {"max_bytes": 8192},
    "devlog.md": {"max_lines": 40, "max_line_len": 120},
}
# Index whose reference definitions point at handoff/detail documents.
HANDOFF_INDEX = "devlog.md"
HANDOFF_DIR = "project_docs/devlogs"

REF_DEF_RE = re.compile(r"^\[([^\]\s]+)\]:\s*(\S+)", re.MULTILINE)
REF_USE_RE = re.compile(r"\]\[([^\]\s]+)\]")
INLINE_LINK_RE = re.compile(r"(!?)\[([^\]]*)\]\(([^)]+)\)")


class CheckError(Exception):
    pass


def strip_code_fences(text):
    """Blank fenced blocks and inline code spans, preserving length and newlines
    so absolute match positions (and thus line numbers) stay valid."""
    out = []
    fence = None
    for line in text.splitlines(keepends=True):
        content = line.rstrip("\n")
        newline = line[len(content):]
        stripped = content.lstrip()
        marker = stripped[:3]
        if fence is None and marker in ("```", "~~~"):
            fence = marker
            out.append(" " * len(content) + newline)
        elif fence is not None:
            if stripped.startswith(fence):
                fence = None
            out.append(" " * len(content) + newline)
        else:
            out.append(line)
    body = "".join(out)
    return re.sub(r"`[^`\n]*`", lambda match: " " * len(match.group()), body)


def resolve(root, doc, raw):
    """Return (absolute path or None, reason). Reason 'ok' means a local target."""
    raw = raw.strip()
    if raw.startswith("<") and raw.endswith(">"):
        raw = raw[1:-1].strip()
    parts = raw.split()
    if not parts:
        return None, "empty"
    raw = parts[0]
    if raw.startswith("#"):
        return None, "anchor-only"
    split = urlsplit(raw)
    if split.scheme and split.scheme.lower() not in ("", "file"):
        return None, "external"
    path_part = unquote(split.path)
    if not path_part:
        return None, "anchor-only"
    base = root if path_part.startswith("/") else doc.parent
    candidate = (base / path_part.lstrip("/")).resolve()
    try:
        candidate.relative_to(root)
    except ValueError:
        return None, "outside"
    return candidate, "ok"


def scan_docs(root):
    files = [root / name for name in ROOT_DOCS if (root / name).is_file()]
    docs_dir = root / DOCS_DIR
    if docs_dir.is_dir():
        for path in sorted(docs_dir.rglob("*.md")):
            relative = path.relative_to(docs_dir)
            if any(part in EXCLUDE_DIRS for part in relative.parts):
                continue
            files.append(path)
    return files


def inspect(root):
    report = {"schema_version": 1, "observed_at": datetime.now(timezone.utc).isoformat(),
              "workspace": str(root), "documents": [], "findings": [],
              "limitations": [
                  "Static text check only; not a build, render or link-preview verdict.",
                  "Only relative Markdown links and reference definitions in root AGENTS.md/devlog.md and project_docs/ (archive excluded).",
                  "External URLs, anchors and code spans/blocks are not followed or validated.",
                  "Read-only and not an atomic snapshot; stop concurrent writers for a baseline.",
              ]}
    if not (root / DOCS_DIR).is_dir():
        raise CheckError("project_docs directory not found; use --root")

    def finding(level, category, subject, line, detail):
        report["findings"].append({"level": level, "category": category,
                                   "subject": subject, "line": line, "detail": detail})

    files = scan_docs(root)
    report["documents"] = [path.relative_to(root).as_posix() for path in files]

    for doc in files:
        rel = doc.relative_to(root).as_posix()
        try:
            text = doc.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            finding("error", "read", rel, None, type(exc).__name__)
            continue
        body = strip_code_fences(text)

        def line_of(position):
            return text.count("\n", 0, position) + 1

        for match in INLINE_LINK_RE.finditer(body):
            if match.group(1) == "!":
                continue  # image, not a doc link
            target = match.group(3)
            resolved, reason = resolve(root, doc, target)
            if reason == "outside":
                finding("error", "link", rel, line_of(match.start()),
                        f"link escapes workspace -> {target}")
            elif reason == "ok" and not resolved.exists():
                finding("warning", "link", rel, line_of(match.start()),
                        f"broken link -> {target}")

        defined = {}
        for match in REF_DEF_RE.finditer(body):
            key, target = match.group(1), match.group(2)
            defined[key] = target
            resolved, reason = resolve(root, doc, target)
            category = "handoff" if rel == HANDOFF_INDEX else "link"
            if reason == "outside":
                finding("error", category, rel, line_of(match.start()),
                        f"reference escapes workspace -> {target}")
            elif reason == "ok" and not resolved.exists():
                finding("warning", category, rel, line_of(match.start()),
                        f"missing target for [{key}] -> {target}")
        for match in REF_USE_RE.finditer(body):
            key = match.group(1)
            if key not in defined:
                finding("warning", "reference", rel, line_of(match.start()),
                        f"dangling reference [{key}] has no definition")

    for rel, budget in BUDGETS.items():
        path = root / rel
        if not path.is_file():
            finding("error", "budget", rel, None, "budgeted file missing")
            continue
        data = path.read_bytes()
        if "max_bytes" in budget and len(data) > budget["max_bytes"]:
            finding("warning", "budget", rel, None,
                    f"{len(data)} bytes exceeds {budget['max_bytes']}")
        if "max_lines" in budget or "max_line_len" in budget:
            lines = data.decode("utf-8", errors="replace").splitlines()
            if "max_lines" in budget and len(lines) > budget["max_lines"]:
                finding("warning", "budget", rel, None,
                        f"{len(lines)} lines exceeds {budget['max_lines']}")
            if "max_line_len" in budget:
                for number, line in enumerate(lines, 1):
                    if len(line) > budget["max_line_len"]:
                        finding("warning", "budget", rel, number,
                                f"line length {len(line)} exceeds {budget['max_line_len']}")

    index = root / HANDOFF_INDEX
    handoff_dir = root / HANDOFF_DIR
    if index.is_file() and handoff_dir.is_dir():
        body = strip_code_fences(index.read_text(encoding="utf-8", errors="replace"))
        referenced = set()
        for match in REF_DEF_RE.finditer(body):
            referenced.add(resolve(root, index, match.group(2))[0])
        for match in INLINE_LINK_RE.finditer(body):
            referenced.add(resolve(root, index, match.group(3))[0])
        for path in sorted(handoff_dir.rglob("*.md")):
            if path.resolve() not in referenced:
                finding("warning", "handoff", path.relative_to(root).as_posix(), None,
                        f"not referenced from {HANDOFF_INDEX}")

    levels = {item["level"] for item in report["findings"]}
    report["exit_code"] = 2 if "error" in levels else 1 if "warning" in levels else 0
    return report


def print_text(report):
    print(f"Workspace: {report['workspace']} | documents={len(report['documents'])}")
    for item in report["findings"]:
        location = f"{item['subject']}:{item['line']}" if item["line"] else item["subject"]
        print(f"  {item['level'].upper()} [{item['category']}] {location}: {item['detail']}")
    if not report["findings"]:
        print("  OK no broken links, budget or handoff findings")
    print(f"Check exit={report['exit_code']} (0=no findings, 1=needs attention, 2=incomplete/error)")
    for limitation in report["limitations"]:
        print("  NOTE " + limitation)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, help="workspace containing project_docs; defaults to script ancestor")
    parser.add_argument("--json", action="store_true", help="machine-readable report on stdout")
    args = parser.parse_args()
    try:
        root = args.root.resolve() if args.root else next(
            (p for p in Path(__file__).resolve().parents if (p / DOCS_DIR).is_dir()), None)
        if root is None or not (root / DOCS_DIR).is_dir():
            raise CheckError("workspace project_docs not found; use --root")
        report = inspect(root)
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
