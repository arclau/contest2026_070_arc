#!/usr/bin/env python3
"""Read-only DesktopMate inventory. No build, hooks, network or automatic repair."""

import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys

CORE_REPOS = (
    "vendor/allwinnertech", "nuttx", "apps", "vendor/openvela",
    "prebuilts/emulator/tools", "contest2026_070_arc",
)
CONFIG_KEYS = (
    "CONFIG_ARCH_ARM", "CONFIG_ARCH_ARM64", "CONFIG_ARCH_CHIP_GOLDFISH_ARM64",
    "CONFIG_LUNCHER_DM_APP", "CONFIG_LUNCHER_DM_SIM", "CONFIG_INIT_ENTRYPOINT",
    "CONFIG_MM_KASAN", "CONFIG_LV_COLOR_DEPTH", "CONFIG_LV_USE_NUTTX_LIBUV",
    "CONFIG_LUNCHER_DM_APP_STACKSIZE",
)
TARGETS = {
    "sim": {
        "source": "vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap/defconfig",
        "config": "cmake_out/vela_goldfish-arm64-v8a-ap/.config",
        "elf": "cmake_out/vela_goldfish-arm64-v8a-ap/nuttx",
        "expected": {"CONFIG_ARCH_ARM64": "y", "CONFIG_ARCH_CHIP_GOLDFISH_ARM64": "y",
                     "CONFIG_LUNCHER_DM_APP": "y", "CONFIG_LUNCHER_DM_SIM": "y",
                     "CONFIG_INIT_ENTRYPOINT": '"luncher_dm_main"'},
        "elf_identity": (64, 183),
    },
    "board": {
        "source": "vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/deskmate/defconfig",
        "config": "nuttx/.config", "elf": "nuttx/nuttx",
        "expected": {"CONFIG_ARCH_ARM": "y", "CONFIG_LUNCHER_DM_APP": "y",
                     "CONFIG_LUNCHER_DM_SIM": "n"},
        "elf_identity": (32, 40),
    },
}


class CheckError(Exception):
    pass


def git(directory, *args, allow_failure=False):
    # Disable optional index refresh, fsmonitor hooks, lazy fetch and inherited
    # repository overrides. Never print Git stderr (may contain private config).
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    env.update(GIT_OPTIONAL_LOCKS="0", GIT_NO_LAZY_FETCH="1", LC_ALL="C")
    try:
        result = subprocess.run(
            ["git", "--no-optional-locks", "-c", "core.fsmonitor=false",
             "-c", "core.untrackedCache=false", "-C", str(directory), *args],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, timeout=20,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise CheckError(f"git unavailable/timeout ({type(exc).__name__})") from exc
    if result.returncode and not allow_failure:
        raise CheckError(f"git command failed (exit {result.returncode})")
    return result


def inside(root, path):
    resolved = path.resolve()
    if not resolved.is_relative_to(root):
        raise CheckError("path resolves outside workspace")
    return resolved


def label(root, path):
    return str(path.relative_to(root))


def file_identity(root, relative, elf=False):
    path = root / relative
    result = {"path": relative}
    try:
        resolved = inside(root, path)
        if not resolved.exists():
            return {**result, "state": "missing"}
        before = resolved.stat()
        if not stat.S_ISREG(before.st_mode):
            raise CheckError("not a regular file")
        digest = hashlib.sha256()
        with resolved.open("rb") as stream:
            header = stream.read(64)
            digest.update(header)
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
        after = resolved.stat()
        if (before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (
                after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns):
            raise CheckError("file changed during inspection; retry after writer stops")
        result.update(state="observed", size=after.st_size, mtime_ns=after.st_mtime_ns,
                      sha256=digest.hexdigest(), resolved_path=label(root, resolved))
        if elf:
            if len(header) >= 20 and header[:4] == b"\x7fELF" and header[4] in (1, 2) and header[5] in (1, 2):
                result["elf"] = {"bits": 32 if header[4] == 1 else 64,
                                 "machine": int.from_bytes(header[18:20], "little" if header[5] == 1 else "big")}
            else:
                result["elf"] = None
        return result
    except (OSError, CheckError) as exc:
        return {**result, "state": "error", "error": type(exc).__name__ if isinstance(exc, OSError) else str(exc)}


def config_info(root, relative):
    result = file_identity(root, relative)
    if result["state"] != "observed":
        return result
    try:
        data = inside(root, root / relative).read_bytes()
        if hashlib.sha256(data).hexdigest() != result["sha256"]:
            raise CheckError("config changed during inspection")
        values = {}
        duplicates = []
        for line in data.decode("utf-8", errors="replace").splitlines():
            match = re.fullmatch(r"(CONFIG_\w+)=(.*)|# (CONFIG_\w+) is not set", line)
            if not match:
                continue
            key, value = (match[1], match[2]) if match[1] else (match[3], "n")
            if key not in CONFIG_KEYS:
                continue
            if key in values:
                duplicates.append(key)
            # Display only fixed non-secret config values; no arbitrary strings.
            if value not in ("y", "n", '"luncher_dm_main"', '"nsh_main"') and not re.fullmatch(r"\d{1,10}", value):
                value = "<other>"
            values[key] = value
        result.update(values=values, duplicate_keys=sorted(set(duplicates)))
    except (OSError, CheckError) as exc:
        result.update(state="error", error=type(exc).__name__ if isinstance(exc, OSError) else str(exc))
    return result


def parse_status(data):
    fields = data.decode("utf-8", errors="replace").split("\0")
    changes = []
    index = 0
    while index < len(fields) and fields[index]:
        entry = fields[index]
        change = {"status": entry[:2], "path": entry[3:]}
        index += 1
        if "R" in change["status"] or "C" in change["status"]:
            change["from"] = fields[index]
            index += 1
        changes.append(change)
    return changes


def repo_info(root, relative):
    result = {"path": relative}
    try:
        path = inside(root, root / relative)
        if not path.is_dir():
            return {**result, "state": "missing"}
        top = Path(os.fsdecode(git(path, "rev-parse", "--show-toplevel").stdout).strip()).resolve()
        if top != path:
            raise CheckError("expected independent repository; Git resolved a parent")
        head = git(path, "rev-parse", "--verify", "HEAD", allow_failure=True)
        changes = parse_status(git(path, "status", "--porcelain=v1", "-z", "--untracked-files=normal", "--ignore-submodules=none").stdout)
        result.update(state="observed", head=head.stdout.decode().strip() if head.returncode == 0 else None,
                      dirty=bool(changes), changes=changes)
    except (CheckError, OSError) as exc:
        result.update(state="error", error=str(exc) if isinstance(exc, CheckError) else type(exc).__name__)
    return result


def inspect(root, target="sim", all_repos=False, paths=()):
    report = {"schema_version": 1, "observed_at": datetime.now(timezone.utc).isoformat(),
              "workspace": str(root), "scope": "manifest+core" if all_repos else "core+requested_paths",
              "target": target, "repositories": [], "path_owners": [], "targets": {}, "findings": [],
              "limitations": ["Observation only; not a build/run/pack/check_res verdict.",
                              "Config and ELF hashes are not a proof that they came from the same build.",
                              "Git status entries do not hash dirty contents; untracked directories are collapsed.",
                              "No runtime, mirrors, credential contents or session-log completeness checks.",
                              "Inspection is not an atomic snapshot; stop concurrent writers for a baseline."]}

    def finding(level, subject, detail):
        report["findings"].append({"level": level, "subject": subject, "detail": detail})

    repos = set(CORE_REPOS)
    if all_repos:
        try:
            manifest = inside(root, root / ".repo/project.list")
            for line in manifest.read_text().splitlines():
                if line.strip():
                    repos.add(label(root, inside(root, root / line.strip())))
        except (OSError, CheckError) as exc:
            finding("error", ".repo/project.list", f"cannot read full inventory ({type(exc).__name__})")
    for relative in paths:
        try:
            path = inside(root, root / relative)
            if not path.exists():
                raise CheckError("requested path missing")
            owner = git(path if path.is_dir() else path.parent, "rev-parse", "--show-toplevel", allow_failure=True)
            if owner.returncode:
                report["path_owners"].append({"path": relative, "repository": None})
                finding("warning", relative, "no Git owner resolved (unversioned or inaccessible)")
            else:
                top = inside(root, Path(os.fsdecode(owner.stdout).strip()))
                name = label(root, top)
                repos.add(name)
                report["path_owners"].append({"path": relative, "repository": name})
        except (CheckError, OSError) as exc:
            finding("error", relative, str(exc) if isinstance(exc, CheckError) else type(exc).__name__)
    with ThreadPoolExecutor(max_workers=4) as pool:
        report["repositories"] = list(pool.map(lambda name: repo_info(root, name), sorted(repos)))
    for repo in report["repositories"]:
        if repo["state"] != "observed":
            finding("error" if repo["state"] == "error" else "warning", repo["path"], repo.get("error", "repository missing"))
        elif repo["head"] is None:
            finding("warning", repo["path"], "HEAD unavailable; repository may have no commits")
    for name in TARGETS if target == "all" else (target,):
        spec = TARGETS[name]
        info = {"source_config": config_info(root, spec["source"]),
                "generated_config": config_info(root, spec["config"]),
                "artifact": file_identity(root, spec["elf"], elf=True),
                "build_provenance": "unknown", "runtime_validation": "not_checked"}
        report["targets"][name] = info
        for kind in ("source_config", "generated_config", "artifact"):
            item = info[kind]
            if item["state"] != "observed":
                finding("error" if item["state"] == "error" else "warning", item["path"], item.get("error", "missing"))
            for key in item.get("duplicate_keys", []):
                finding("warning", item["path"], f"duplicate selected key: {key}")
        config, artifact = info["generated_config"], info["artifact"]
        if config["state"] == "observed":
            for key, expected in spec["expected"].items():
                actual = config["values"].get(key)
                if actual != expected:
                    finding("warning", config["path"], f"{key}: observed {actual or 'unknown'}, expected {expected}")
            source = info["source_config"]
            if source["state"] == "observed":
                for key, value in source["values"].items():
                    if key in config["values"] and config["values"][key] != value:
                        finding("warning", name, f"source/generated selected key differs: {key}")
        if artifact["state"] == "observed":
            identity = artifact["elf"] or {}
            if (identity.get("bits"), identity.get("machine")) != spec["elf_identity"]:
                finding("warning", artifact["path"], "ELF header architecture differs from target or is invalid")
            for kind in ("source_config", "generated_config"):
                item = info[kind]
                if item["state"] == "observed" and item["mtime_ns"] > artifact["mtime_ns"]:
                    finding("warning", artifact["path"], f"{kind} newer than ELF; rebuild identity needs verification (mtime is only a hint)")
    if (root / "nuttx/include/nuttx/config.h").exists() and (root / "cmake_out/vela_goldfish-arm64-v8a-ap/include/nuttx/config.h").exists():
        finding("warning", "config headers", "both source-tree and CMake headers exist; inspect actual include order before claiming contamination")
    levels = {f["level"] for f in report["findings"]}
    report["exit_code"] = 2 if "error" in levels else 1 if "warning" in levels else 0
    return report


def print_text(report):
    print(f"Workspace: {report['workspace']} | scope={report['scope']} | target={report['target']}")
    for repo in report["repositories"]:
        if repo["state"] != "observed":
            print(f"  {repo['state'].upper()} {repo['path']}")
            continue
        changes = repo["changes"]
        print(f"  {'DIRTY' if changes else 'CLEAN'} {repo['path']} HEAD={repo['head'] or 'unknown'} entries={len(changes)}")
        for change in changes[:12]:
            print(f"    {change['status']} {json.dumps(change['path'], ensure_ascii=False)}")
        if len(changes) > 12:
            print("    ... use --json for all status entries")
    for owner in report["path_owners"]:
        print(f"  OWNER {owner['path']} -> {owner['repository'] or 'unknown'}")
    for name, info in report["targets"].items():
        print(f"Target {name}: provenance=UNKNOWN runtime=NOT_CHECKED")
        for kind in ("source_config", "generated_config", "artifact"):
            item = info[kind]
            print(f"  {kind}: {item['path']} {item['state']} sha256={item.get('sha256', '-')}")
            if "values" in item:
                print("    " + json.dumps(item["values"], ensure_ascii=False, sort_keys=True))
            if "elf" in item:
                print("    ELF " + json.dumps(item["elf"]))
    for item in report["findings"]:
        print(f"  {item['level'].upper()} {item['subject']}: {item['detail']}")
    print(f"Inspection exit={report['exit_code']} (0=no static findings, 1=needs attention, 2=incomplete/error)")
    for limitation in report["limitations"]:
        print("  NOTE " + limitation)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, help="workspace with .repo; defaults to script ancestor")
    parser.add_argument("--target", choices=("sim", "board", "all"), default="sim")
    parser.add_argument("--all-repos", action="store_true", help="include .repo/project.list, not just six core repositories")
    parser.add_argument("--path", action="append", default=[], help="additional file/directory to resolve, relative to workspace")
    parser.add_argument("--json", action="store_true", help="machine-readable report on stdout")
    args = parser.parse_args()
    try:
        root = args.root.resolve() if args.root else next((p for p in Path(__file__).resolve().parents if (p / ".repo").is_dir()), None)
        if root is None or not (root / ".repo").is_dir():
            raise CheckError("workspace .repo directory not found; use --root")
        report = inspect(root, args.target, args.all_repos, args.path)
    except (OSError, CheckError) as exc:
        print(json.dumps({"schema_version": 1, "exit_code": 2, "error": str(exc) if isinstance(exc, CheckError) else type(exc).__name__}))
        return 2
    if args.json:
        print(json.dumps(report, ensure_ascii=False, indent=2))
    else:
        print_text(report)
    return report["exit_code"]


if __name__ == "__main__":
    sys.exit(main())
