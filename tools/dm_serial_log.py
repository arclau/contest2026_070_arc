#!/usr/bin/env python3
"""dm_serial_log.py - 板端串口日志框架（面向 AI agent）。

设计目标
--------
1. 常驻抓取：一次 start 之后一直采，板子重启/掉线/重连都不用重新开。
2. 永不覆盖：每个会话一个 .log + 一个 .meta.json，latest.log 只做软链。
3. agent 可读：固定前缀的行格式（ISO 时间 + RX/TX/MARK/BOOT/LINK），
   read/tail/summary 支持 --json，便于程序化取证据。
4. 免重复开机：同一次采集中自动识别多次启动并打 BOOT #n 分隔。

典型用法
--------
    dm_serial_log.py start --baud 1500000 --tag p275
    dm_serial_log.py status
    dm_serial_log.py mark "flash 95df86ae"
    dm_serial_log.py send "ls /dev" --wait 1.5
    dm_serial_log.py tail -n 80
    dm_serial_log.py read --from-boot 1 --grep "pwm_|BOE1440|I2C"
    dm_serial_log.py summary
    dm_serial_log.py wrap --tag flash -- bash /data/vela/pack.sh
    dm_serial_log.py stop

日志格式（每行一条，agent 直接按前缀切分）
------------------------------------------
    2026-10-20T00:37:12.345+00:00 RX   <设备输出，完整行>
    2026-10-20T00:37:12.345+00:00 RX~  <设备输出，无换行，空闲后强制落盘>
    2026-10-20T00:37:12.345+00:00 TX   <本机发送>
    2026-10-20T00:37:12.345+00:00 MARK <人工标记>
    2026-10-20T00:37:12.345+00:00 BOOT #1 <命中的启动特征>
    2026-10-20T00:37:12.345+00:00 LINK open|closed|error <说明>

只依赖 pyserial；无硬件时用 selftest（pty 环回）验证行为。
"""

import argparse
import errno
import json
import os
import pty
import re
import select
import signal
import socket
import subprocess
import sys
import time
from datetime import datetime, timezone

DEFAULT_PORT = "/dev/ttyUSB0"
DEFAULT_BAUD = 1500000
DEFAULT_LOGDIR = "/data/dm/logs/serial"
DEFAULT_RUNDIR = "/tmp/dm_serial"
DEFAULT_BOOT_RE = r"(?i)(U-Boot 20\d\d|NuttX|Starting kernel|Booting Linux|\[ND\]boot start)"
IDLE_FLUSH = 0.15          # 无换行的残行多久落盘
BOOT_COOLDOWN = 5.0        # 同一秒内的重复启动特征去抖


# ---------------------------------------------------------------- 基础工具

def now_iso():
    return datetime.now(timezone.utc).astimezone().isoformat(timespec="milliseconds")


def ensure_dir(path):
    os.makedirs(path, exist_ok=True)
    return path


def sock_path(port):
    return os.path.join(ensure_dir(DEFAULT_RUNDIR), os.path.basename(port) + ".sock")


def pid_path(port):
    return os.path.join(ensure_dir(DEFAULT_RUNDIR), os.path.basename(port) + ".pid")


def latest_link(logdir):
    return os.path.join(logdir, "latest.log")


def resolve_session(logdir, name):
    """把 latest / 前缀 / 文件名解析成实际 .log 路径。"""
    if not name or name == "latest":
        link = latest_link(logdir)
        if os.path.islink(link) or os.path.exists(link):
            return os.path.realpath(link)
        raise SystemExit("no session found in %s" % logdir)
    cand = name if os.path.isabs(name) else os.path.join(logdir, name)
    if os.path.exists(cand):
        return cand
    matches = sorted(
        p for p in os.listdir(logdir)
        if p.endswith(".log") and p.startswith(name)
    )
    if len(matches) == 1:
        return os.path.join(logdir, matches[0])
    if not matches:
        raise SystemExit("session not found: %s" % name)
    raise SystemExit("ambiguous session %s: %s" % (name, ", ".join(matches)))


def read_meta(logpath):
    meta = logpath[:-4] + ".meta.json"
    try:
        with open(meta, "r", encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return {}


def write_meta(logpath, data):
    meta = logpath[:-4] + ".meta.json"
    tmp = meta + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(data, fh, ensure_ascii=False, indent=1, sort_keys=True)
    os.replace(tmp, meta)


def tail_lines(path, count):
    """从文件尾读最后 count 行，避免整份载入。"""
    with open(path, "rb") as fh:
        fh.seek(0, os.SEEK_END)
        size = fh.tell()
        block = 8192
        data = b""
        while size > 0 and data.count(b"\n") <= count:
            step = min(block, size)
            size -= step
            fh.seek(size)
            data = fh.read(step) + data
    text = data.decode("utf-8", "replace")
    lines = text.splitlines()
    return lines[-count:] if count else lines


# ---------------------------------------------------------------- 客户端

class Client(object):
    def __init__(self, port):
        self.path = sock_path(port)

    def request(self, payload, timeout=10.0):
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(timeout)
        try:
            sock.connect(self.path)
            sock.sendall((json.dumps(payload) + "\n").encode("utf-8"))
            buf = b""
            while not buf.endswith(b"\n"):
                chunk = sock.recv(65536)
                if not chunk:
                    break
                buf += chunk
            return json.loads(buf.decode("utf-8") or "{}")
        finally:
            sock.close()

    def alive(self):
        return os.path.exists(self.path)


# ---------------------------------------------------------------- 采集守护进程

class Daemon(object):
    def __init__(self, port, baud, logdir, tag, boot_re, exclusive=True):
        self.port = port
        self.baud = baud
        self.logdir = ensure_dir(logdir)
        self.tag = tag or "serial"
        self.boot_re = re.compile(boot_re) if boot_re else None
        self.exclusive = exclusive

        stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
        self.logpath = os.path.join(self.logdir, "%s_%s.log" % (stamp, self.tag))
        self.fh = open(self.logpath, "a", encoding="utf-8", buffering=1)

        self.ser = None
        self.pending = b""
        self.pending_ts = 0.0
        self.rx_bytes = 0
        self.tx_bytes = 0
        self.lines = 0
        self.boots = 0
        self.last_boot = 0.0
        self.started = time.time()
        self.conn_state = "closed"
        self.meta_dirty = True

        self.capture = None          # 供 send 用的临时捕获
        self.capture_until = 0.0
        self.capture_idle = 0.0

    # ---- 输出

    def emit(self, kind, text):
        line = "%s %-4s %s" % (now_iso(), kind, text)
        self.fh.write(line + "\n")
        self.lines += 1
        self.meta_dirty = True

    def emit_boot(self, hit):
        self.boots += 1
        self.emit("BOOT", "#%d %s" % (self.boots, hit.strip()[:60]))

    # ---- 串口

    def open_port(self):
        try:
            import serial
        except ImportError:
            self.emit("LINK", "error pyserial missing")
            return False
        try:
            self.ser = serial.Serial(
                self.port, self.baud, timeout=0,
                bytesize=8, parity="N", stopbits=1,
                rtscts=False, dsrdtr=False,
                exclusive=self.exclusive,
            )
        except Exception as exc:                       # noqa: BLE001 - 端口可能被占用/未插
            if self.conn_state != "error":
                self.emit("LINK", "error %s" % str(exc).replace("\n", " "))
                self.conn_state = "error"
            self.ser = None
            return False
        self.conn_state = "open"
        self.emit("LINK", "open %s @%d" % (self.port, self.baud))
        return True

    def close_port(self, why):
        if self.ser:
            try:
                self.ser.close()
            except Exception:                          # noqa: BLE001
                pass
            self.ser = None
        if self.conn_state != "closed":
            self.emit("LINK", "closed %s" % why)
            self.conn_state = "closed"

    def feed(self, chunk):
        if not chunk:
            return
        self.rx_bytes += len(chunk)
        self.pending += chunk
        self.pending_ts = time.time()
        if self.capture is not None:
            self.capture.append(chunk)
        self.drain_lines()

    def drain_lines(self, force=False):
        while True:
            idx = self.pending.find(b"\n")
            if idx < 0:
                break
            raw, self.pending = self.pending[:idx], self.pending[idx + 1:]
            self.emit_rx(raw)
        if self.pending and (force or time.time() - self.pending_ts > IDLE_FLUSH):
            raw, self.pending = self.pending, b""
            self.emit_rx(raw, partial=True)

    def emit_rx(self, raw, partial=False):
        text = raw.decode("utf-8", "replace").rstrip("\r")
        self.emit("RX~" if partial else "RX", text)
        if not partial and self.boot_re and self.boot_re.search(text):
            if time.time() - self.last_boot > BOOT_COOLDOWN:
                self.last_boot = time.time()
                self.emit_boot(text)

    # ---- 控制命令

    def handle(self, req):
        cmd = req.get("cmd")
        if cmd == "status":
            return self.status()
        if cmd == "mark":
            self.emit("MARK", str(req.get("text", "")))
            return {"ok": True}
        if cmd == "send":
            return self.do_send(req)
        if cmd == "stop":
            self.emit("LINK", "stop requested")
            return {"ok": True, "stopping": True}
        return {"ok": False, "error": "unknown cmd %r" % cmd}

    def status(self):
        return {
            "ok": True,
            "running": True,
            "port": self.port,
            "baud": self.baud,
            "session": self.logpath,
            "connected": self.ser is not None,
            "rx_bytes": self.rx_bytes,
            "tx_bytes": self.tx_bytes,
            "lines": self.lines,
            "boots": self.boots,
            "uptime_s": round(time.time() - self.started, 1),
        }

    def do_send(self, req):
        data = str(req.get("data", ""))
        eol = req.get("eol", "\r\n")
        wait = float(req.get("wait", 1.5))
        max_wait = float(req.get("max_wait", max(5.0, wait * 4)))
        if not self.ser:
            return {"ok": False, "error": "serial not open"}
        self.emit("TX", data)
        payload = (data + eol).encode("utf-8")
        try:
            self.ser.write(payload)
            self.ser.flush()
        except Exception as exc:                       # noqa: BLE001
            return {"ok": False, "error": "write failed: %s" % exc}
        self.tx_bytes += len(payload)

        self.capture = []
        self.capture_idle = time.time()
        deadline = time.time() + max_wait
        while time.time() < deadline:
            ready, _, _ = select.select([self.ser], [], [], 0.05)
            if ready:
                chunk = self.ser.read(4096)
                if chunk:
                    self.feed(chunk)
                    self.capture_idle = time.time()
            if time.time() - self.capture_idle >= wait:
                break
        text = b"".join(self.capture).decode("utf-8", "replace")
        self.capture = None
        return {"ok": True, "response": text}

    # ---- 主循环

    def serve(self):
        self.emit("LINK", "session start pid=%d" % os.getpid())
        self.emit("LINK", "logfile %s" % self.logpath)
        self.meta_dirty = True

        spath = sock_path(self.port)
        try:
            os.unlink(spath)
        except OSError:
            pass
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(spath)
        listener.listen(8)
        listener.setblocking(False)

        with open(pid_path(self.port), "w", encoding="utf-8") as fh:
            fh.write(str(os.getpid()))

        stop = {"flag": False}

        def on_signal(signum, frame):                  # noqa: ARG001
            stop["flag"] = True

        signal.signal(signal.SIGTERM, on_signal)
        signal.signal(signal.SIGINT, on_signal)

        last_meta = 0.0
        while not stop["flag"]:
            if self.ser is None:
                self.open_port()
                if self.ser is None:
                    time.sleep(1.0)
                    continue

            watch = [listener, self.ser]
            try:
                ready, _, _ = select.select(watch, [], [], 0.1)
            except (OSError, ValueError):
                self.close_port("select failed")
                continue

            for item in ready:
                if item is listener:
                    try:
                        conn, _ = listener.accept()
                    except OSError:
                        continue
                    with conn:
                        conn.settimeout(5.0)
                        try:
                            buf = b""
                            while not buf.endswith(b"\n"):
                                part = conn.recv(4096)
                                if not part:
                                    break
                                buf += part
                            resp = self.handle(json.loads(buf.decode("utf-8") or "{}"))
                        except Exception as exc:       # noqa: BLE001
                            resp = {"ok": False, "error": str(exc)}
                        try:
                            conn.sendall((json.dumps(resp) + "\n").encode("utf-8"))
                        except OSError:
                            pass
                    if resp.get("stopping"):
                        stop["flag"] = True
                else:
                    try:
                        chunk = self.ser.read(4096)
                    except Exception as exc:           # noqa: BLE001
                        self.close_port("read failed: %s" % exc)
                        continue
                    if chunk:
                        self.feed(chunk)
                    else:
                        # 端口消失（拔线）时 read 返回空
                        if not self.ser.is_open:
                            self.close_port("device gone")

            self.drain_lines()
            now = time.time()
            if self.meta_dirty and now - last_meta > 2.0:
                self.flush_meta()
                last_meta = now

        self.drain_lines(force=True)
        self.flush_meta(final=True)
        self.close_port("shutdown")
        try:
            listener.close()
            os.unlink(spath)
        except OSError:
            pass
        try:
            os.unlink(pid_path(self.port))
        except OSError:
            pass
        self.emit("LINK", "session end")
        self.fh.close()

    def flush_meta(self, final=False):
        data = {
            "port": self.port,
            "baud": self.baud,
            "tag": self.tag,
            "logfile": self.logpath,
            "started": datetime.fromtimestamp(self.started).astimezone().isoformat(),
            "rx_bytes": self.rx_bytes,
            "tx_bytes": self.tx_bytes,
            "lines": self.lines,
            "boots": self.boots,
            "pid": os.getpid(),
            "running": not final,
        }
        if final:
            data["ended"] = now_iso()
            data["uptime_s"] = round(time.time() - self.started, 1)
        write_meta(self.logpath, data)
        try:
            link = latest_link(self.logdir)
            tmp = link + ".tmp"
            if os.path.lexists(tmp):
                os.unlink(tmp)
            os.symlink(os.path.basename(self.logpath), tmp)
            os.replace(tmp, link)
        except OSError:
            pass
        self.meta_dirty = False


# ---------------------------------------------------------------- 子命令

def cmd_start(args):
    if Client(args.port).alive():
        status = Client(args.port).request({"cmd": "status"})
        print("already running: %s" % status.get("session"))
        return 0
    logdir = ensure_dir(args.logdir)
    daemon = Daemon(args.port, args.baud, logdir, args.tag, args.boot_regex,
                    exclusive=not args.no_exclusive)
    # 子进程里重建，避免父进程持有文件句柄
    logpath = daemon.logpath
    daemon.fh.close()
    argv = [sys.executable, os.path.abspath(__file__), "_daemon",
            "--port", args.port, "--baud", str(args.baud),
            "--logdir", logdir, "--tag", args.tag,
            "--logfile", logpath]
    if args.boot_regex:
        argv += ["--boot-regex", args.boot_regex]
    if args.no_exclusive:
        argv += ["--no-exclusive"]
    subprocess.Popen(argv, start_new_session=True,
                     stdin=subprocess.DEVNULL,
                     stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL)
    deadline = time.time() + 5.0
    while time.time() < deadline:
        if Client(args.port).alive():
            print(logpath)
            return 0
        time.sleep(0.1)
    print("failed to start daemon (socket %s not ready)" % sock_path(args.port),
          file=sys.stderr)
    return 1


def cmd_daemon(args):
    daemon = Daemon(args.port, args.baud, args.logdir, args.tag, args.boot_regex,
                    exclusive=not args.no_exclusive)
    if args.logfile and args.logfile != daemon.logpath:
        daemon.fh.close()
        daemon.logpath = args.logfile
        daemon.fh = open(daemon.logpath, "a", encoding="utf-8", buffering=1)
    daemon.serve()
    return 0


def cmd_stop(args):
    client = Client(args.port)
    if not client.alive():
        print("not running")
        return 0
    resp = client.request({"cmd": "stop"})
    deadline = time.time() + 5.0
    while time.time() < deadline and client.alive():
        time.sleep(0.1)
    print("stopped" if not client.alive() else "stop timeout", file=sys.stdout)
    return 0 if not client.alive() else 1


def cmd_status(args):
    client = Client(args.port)
    if not client.alive():
        out = {"ok": True, "running": False, "port": args.port}
    else:
        out = client.request({"cmd": "status"})
    if args.json:
        print(json.dumps(out, ensure_ascii=False, indent=1))
    else:
        if not out.get("running"):
            print("not running (%s)" % args.port)
        else:
            print("session   %s" % out["session"])
            print("link      %s  %s @%d" % ("up" if out["connected"] else "down",
                                           out["port"], out["baud"]))
            print("traffic   rx=%d tx=%d  lines=%d boots=%d  uptime=%ss"
                  % (out["rx_bytes"], out["tx_bytes"], out["lines"],
                     out["boots"], out["uptime_s"]))
    return 0


def cmd_mark(args):
    client = Client(args.port)
    if not client.alive():
        print("not running", file=sys.stderr)
        return 1
    client.request({"cmd": "mark", "text": " ".join(args.text)})
    return 0


def cmd_send(args):
    client = Client(args.port)
    if not client.alive():
        print("not running", file=sys.stderr)
        return 1
    eol = "" if args.no_newline else ("\n" if args.lf else "\r\n")
    resp = client.request({
        "cmd": "send",
        "data": " ".join(args.text),
        "eol": eol,
        "wait": args.wait,
        "max_wait": args.max_wait,
    }, timeout=args.max_wait + 10)
    if not resp.get("ok"):
        print("send failed: %s" % resp.get("error"), file=sys.stderr)
        return 1
    sys.stdout.write(resp.get("response", ""))
    if args.json:
        print(json.dumps(resp, ensure_ascii=False, indent=1))
    return 0


def cmd_tail(args):
    path = resolve_session(args.logdir, args.session)
    for line in tail_lines(path, args.n):
        print(line)
    return 0


def _iter_lines(path, from_line=0):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for idx, line in enumerate(fh):
            if idx >= from_line:
                yield idx, line.rstrip("\n")


def cmd_read(args):
    path = resolve_session(args.logdir, args.session)
    lines = list(_iter_lines(path))
    start = 0
    if args.from_mark:
        for idx, line in lines:
            if " MARK " in line and args.from_mark in line:
                start = idx + 1
    if args.from_boot:
        seen = 0
        for idx, line in lines:
            if " BOOT " in line:
                seen += 1
                if seen == args.from_boot:
                    start = idx
                    break
    pattern = re.compile(args.grep) if args.grep else None
    out = []
    for idx, line in lines[start:]:
        if pattern and not pattern.search(line):
            continue
        out.append(line)
        if args.limit and len(out) >= args.limit:
            break
    if args.json:
        print(json.dumps({"session": path, "count": len(out), "lines": out},
                         ensure_ascii=False, indent=1))
    else:
        for line in out:
            print(line)
    return 0


KEYS = [
    ("panel", r"BOE1440|BOE_1440x720|power_on|panel_init"),
    ("backlight", r"pwm_|set_bright|bright_curve|bl_en"),
    ("touch", r"HX83102|GT9271|\[I2C\]|twi"),
    ("dsi", r"de_dsi|DSI|tcon"),
    ("fault", r"(?i)panic|assert|hardfault|segfault|timeout|error|fail"),
]


def cmd_summary(args):
    path = resolve_session(args.logdir, args.session)
    lines = list(_iter_lines(path))
    meta = read_meta(path)

    boots = []
    for idx, line in lines:
        if " BOOT " in line:
            boots.append((idx, line))

    report = {
        "session": path,
        "meta": meta,
        "total_lines": len(lines),
        "boots": len(boots),
        "first": lines[0][1] if lines else None,
        "last": lines[-1][1] if lines else None,
        "groups": {},
    }
    for name, pattern in KEYS:
        rx = re.compile(pattern)
        hits = [line for _, line in lines if rx.search(line)]
        report["groups"][name] = {
            "count": len(hits),
            "samples": hits[:args.samples],
        }

    if args.json:
        print(json.dumps(report, ensure_ascii=False, indent=1))
        return 0

    print("session  %s" % path)
    if meta:
        print("link     %s @%s  running=%s boots=%s"
              % (meta.get("port"), meta.get("baud"), meta.get("running"),
                 meta.get("boots")))
    print("lines    %d" % len(lines))
    if boots:
        print("boots    %d" % len(boots))
        for idx, line in boots:
            print("  %s" % line)
    for name, _ in KEYS:
        info = report["groups"][name]
        print("%-9s %d hit(s)" % (name, info["count"]))
        for sample in info["samples"]:
            print("   | %s" % sample)
    return 0


def cmd_list(args):
    logdir = args.logdir
    if not os.path.isdir(logdir):
        print("no logs in %s" % logdir)
        return 0
    rows = []
    for name in sorted(os.listdir(logdir)):
        if not name.endswith(".log"):
            continue
        path = os.path.join(logdir, name)
        meta = read_meta(path)
        rows.append({
            "session": name,
            "size": os.path.getsize(path),
            "mtime": datetime.fromtimestamp(os.path.getmtime(path)).isoformat(
                timespec="seconds"),
            "boots": meta.get("boots"),
            "running": meta.get("running"),
            "baud": meta.get("baud"),
        })
    if args.json:
        print(json.dumps(rows, ensure_ascii=False, indent=1))
    else:
        for row in rows:
            print("%-40s %8d B  boots=%-3s %s%s"
                  % (row["session"], row["size"], row["boots"], row["mtime"],
                     "  [live]" if row["running"] else ""))
    return 0


def cmd_wrap(args):
    """在采集运行期间执行一条外部命令（刷机/打包），前后自动打 MARK。"""
    client = Client(args.port)
    started_here = False
    if not client.alive():
        rc = cmd_start(argparse.Namespace(
            port=args.port, baud=args.baud, logdir=args.logdir,
            tag=args.tag or "wrap", boot_regex=None, no_exclusive=False))
        if rc:
            return rc
        started_here = True
    label = args.tag or "cmd"
    client.request({"cmd": "mark", "text": "%s BEGIN %s" % (label, " ".join(args.command))})
    t0 = time.time()
    rc = subprocess.call(args.command)
    client.request({"cmd": "mark",
                    "text": "%s END rc=%d %.1fs" % (label, rc, time.time() - t0)})
    if started_here and not args.keep:
        cmd_stop(argparse.Namespace(port=args.port))
    return rc


# ---------------------------------------------------------------- 自测

def cmd_selftest(args):
    """用 pty 环回验证：分段、残行落盘、BOOT 识别、send/read、meta。"""
    import tempfile

    failures = []

    def check(name, cond, detail=""):
        if cond:
            print("PASS %s" % name)
        else:
            print("FAIL %s %s" % (name, detail))
            failures.append(name)

    with tempfile.TemporaryDirectory(prefix="dm-serial-") as tmp:
        master, slave = pty.openpty()
        port = os.ttyname(slave)
        logdir = os.path.join(tmp, "logs")
        rc = cmd_start(argparse.Namespace(
            port=port, baud=DEFAULT_BAUD, logdir=logdir, tag="selftest",
            boot_regex=DEFAULT_BOOT_RE, no_exclusive=True))
        check("daemon start", rc == 0)
        client = Client(port)
        check("socket alive", client.alive())

        os.write(master, b"U-Boot 2024.01 (Oct 19 2026)\r\n")
        os.write(master, b"BOE1440: power_on start\r\n")
        time.sleep(0.4)
        os.write(master, b"nsh> ")           # 无换行，应走 RX~
        time.sleep(0.6)

        session = resolve_session(logdir, "latest")
        text = open(session, encoding="utf-8").read()
        check("boot detected", "BOOT #1" in text, text[-200:])
        check("rx line", "BOE1440: power_on start" in text)
        check("partial flushed", "RX~  nsh> " in text, text[-200:])

        client.request({"cmd": "mark", "text": "probe-1"})
        resp = client.request({"cmd": "send", "data": "ls /dev", "eol": "\r\n",
                               "wait": 0.3, "max_wait": 2.0})
        echoed = os.read(master, 64)
        check("tx on wire", b"ls /dev\r\n" in echoed, repr(echoed))
        check("send ok", resp.get("ok") is True, str(resp))
        os.write(master, b"/dev/console\r\n")
        time.sleep(0.3)

        rc = cmd_stop(argparse.Namespace(port=port))
        check("daemon stop", rc == 0 and not client.alive())
        meta = read_meta(session)
        check("meta written", meta.get("running") is False and meta.get("lines", 0) > 3,
              str(meta))
        check("latest link", os.path.islink(latest_link(logdir)))

        out = subprocess.run(
            [sys.executable, os.path.abspath(__file__), "read",
             "--logdir", logdir, "--from-mark", "probe-1", "--json"],
            capture_output=True, text=True)
        payload = json.loads(out.stdout or "{}")
        check("read --from-mark", payload.get("count", 0) >= 1, out.stdout[:200])

        out = subprocess.run(
            [sys.executable, os.path.abspath(__file__), "summary",
             "--logdir", logdir, "--json"],
            capture_output=True, text=True)
        summary = json.loads(out.stdout or "{}")
        check("summary boots", summary.get("boots") == 1, out.stdout[:200])
        check("summary panel hits", summary.get("groups", {}).get("panel", {}).get("count", 0) >= 1)

        os.close(master)
        os.close(slave)

    if failures:
        print("\n%d check(s) failed: %s" % (len(failures), ", ".join(failures)))
        return 1
    print("\nall checks passed")
    return 0


# ---------------------------------------------------------------- 入口

def build_parser():
    p = argparse.ArgumentParser(
        description="板端串口日志框架（常驻抓取 / 分段归档 / agent 可读）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("典型用法")[1] if "典型用法" in __doc__ else None)
    p.add_argument("--logdir", default=DEFAULT_LOGDIR, help="日志目录")
    sub = p.add_subparsers(dest="cmd", required=True)

    def add_common(sp):
        sp.add_argument("--port", default=DEFAULT_PORT)
        sp.add_argument("--logdir", default=DEFAULT_LOGDIR)

    sp = sub.add_parser("start", help="启动常驻采集")
    add_common(sp)
    sp.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    sp.add_argument("--tag", default=None, help="会话标签，进文件名")
    sp.add_argument("--boot-regex", default=DEFAULT_BOOT_RE)
    sp.add_argument("--no-exclusive", action="store_true")
    sp.set_defaults(func=cmd_start)

    sp = sub.add_parser("_daemon", help=argparse.SUPPRESS)
    add_common(sp)
    sp.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    sp.add_argument("--tag", default=None)
    sp.add_argument("--logfile", default=None)
    sp.add_argument("--boot-regex", default=DEFAULT_BOOT_RE)
    sp.add_argument("--no-exclusive", action="store_true")
    sp.set_defaults(func=cmd_daemon)

    sp = sub.add_parser("stop", help="停止采集")
    add_common(sp)
    sp.set_defaults(func=cmd_stop)

    sp = sub.add_parser("status", help="查看采集状态")
    add_common(sp)
    sp.add_argument("--json", action="store_true")
    sp.set_defaults(func=cmd_status)

    sp = sub.add_parser("mark", help="写入一条 MARK")
    add_common(sp)
    sp.add_argument("text", nargs="+")
    sp.set_defaults(func=cmd_mark)

    sp = sub.add_parser("send", help="向设备发一条命令并取回响应")
    add_common(sp)
    sp.add_argument("text", nargs="+")
    sp.add_argument("--wait", type=float, default=1.5, help="静默多久算结束")
    sp.add_argument("--max-wait", type=float, default=6.0)
    sp.add_argument("--lf", action="store_true", help="只发 \\n")
    sp.add_argument("--no-newline", action="store_true")
    sp.add_argument("--json", action="store_true")
    sp.set_defaults(func=cmd_send)

    sp = sub.add_parser("tail", help="看最后 N 行")
    add_common(sp)
    sp.add_argument("-n", type=int, default=50)
    sp.add_argument("--session", default="latest")
    sp.set_defaults(func=cmd_tail)

    sp = sub.add_parser("read", help="按标记/启动号/正则读取")
    add_common(sp)
    sp.add_argument("--session", default="latest")
    sp.add_argument("--from-mark", default=None)
    sp.add_argument("--from-boot", type=int, default=None)
    sp.add_argument("--grep", default=None)
    sp.add_argument("--limit", type=int, default=0)
    sp.add_argument("--json", action="store_true")
    sp.set_defaults(func=cmd_read)

    sp = sub.add_parser("summary", help="给 agent 的摘要")
    add_common(sp)
    sp.add_argument("--session", default="latest")
    sp.add_argument("--samples", type=int, default=3)
    sp.add_argument("--json", action="store_true")
    sp.set_defaults(func=cmd_summary)

    sp = sub.add_parser("list", help="列出所有会话")
    sp.add_argument("--logdir", default=DEFAULT_LOGDIR)
    sp.add_argument("--json", action="store_true")
    sp.set_defaults(func=cmd_list)

    sp = sub.add_parser("wrap", help="采集期间执行外部命令（刷机/打包）")
    add_common(sp)
    sp.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    sp.add_argument("--tag", default=None)
    sp.add_argument("--keep", action="store_true", help="结束后不停止采集")
    sp.add_argument("command", nargs=argparse.REMAINDER)
    sp.set_defaults(func=cmd_wrap)

    sp = sub.add_parser("selftest", help="pty 环回自测（不需要硬件）")
    sp.set_defaults(func=cmd_selftest)
    return p


def main(argv):
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.cmd == "wrap" and args.command and args.command[0] == "--":
        args.command = args.command[1:]
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
