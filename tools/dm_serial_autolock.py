#!/usr/bin/env python3
"""dm_serial_autolock.py - 等待新串口设备出现，自动判定波特率并交给 dm_serial_log.py 采集。

用途：不预先知道设备插在哪个 ttyUSB、也不确定波特率时，先挂一个"守株待兔"的监视器。
板子插上后自动识别，锁定后转交常驻采集器，日志格式与 dm_serial_log.py 完全一致。

典型用法
--------
    dm_serial_autolock.py --tag f101 --ignore /dev/ttyUSB0
    dm_serial_autolock.py --tag f101 --port /dev/ttyUSB1 --baud 115200
    dm_serial_autolock.py --selftest

判定规则
--------
对每个候选设备依次尝试 --bauds 列表，各采样 --probe 秒：
  有效字节 >= 20 且可打印字符占比 >= 0.85 视为命中，取占比最高者。
已被其他进程独占（TIOCEXCL）的设备直接跳过，不会抢走正在采集的口。
"""

import argparse
import glob
import os
import subprocess
import sys
import time

DEFAULT_BAUDS = "115200,1500000,921600,460800,57600"
TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dm_serial_log.py")


def list_devices():
    return sorted(glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*"))


def probe(port, baud, secs):
    """返回 (可打印占比, 字节数)；打不开返回 (-1, 0)。"""
    try:
        import serial
    except ImportError:
        sys.exit("pyserial 未安装：pip install pyserial")
    try:
        ser = serial.Serial(port, baud, timeout=0.05, exclusive=True)
    except Exception:
        return -1, 0
    buf = b""
    t0 = time.time()
    try:
        while time.time() - t0 < secs:
            buf += ser.read(8192)
    finally:
        ser.close()
    if not buf:
        return 0.0, 0
    ok = sum(1 for b in buf if 32 <= b < 127 or b in (9, 10, 13))
    return ok / len(buf), len(buf)


def pick_baud(port, bauds, secs):
    best = (0.0, 0, None)
    for b in bauds:
        ratio, n = probe(port, b, secs)
        if ratio < 0:
            return None, "busy"
        if n >= 20 and ratio >= 0.85 and ratio > best[0]:
            best = (ratio, n, b)
    return best[2], "ok"


def start_capture(port, baud, tag, logdir):
    cmd = [sys.executable, TOOL, "start", "--port", port, "--baud", str(baud),
           "--tag", tag, "--logdir", logdir]
    return subprocess.run(cmd, check=False).returncode


def selftest():
    """pty 环回：写入可打印文本，验证 probe 判定与波特率选择。"""
    import pty
    import threading

    master, slave = pty.openpty()
    name = os.ttyname(slave)

    def writer():
        for _ in range(3):
            os.write(master, b"U-Boot 2017.09\r\nStarting kernel ...\r\n")
            time.sleep(0.2)

    threading.Thread(target=writer, daemon=True).start()
    ratio, n = probe(name, 115200, 1.0)
    os.close(slave)
    os.close(master)
    assert n >= 20, "probe 未读到足够字节: n=%d" % n
    assert ratio >= 0.85, "probe 可打印占比过低: %.2f" % ratio
    print("selftest OK ratio=%.2f bytes=%d" % (ratio, n))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None, help="直接指定设备；缺省则等待新设备")
    ap.add_argument("--baud", type=int, default=None, help="指定波特率；缺省自动判定")
    ap.add_argument("--bauds", default=DEFAULT_BAUDS)
    ap.add_argument("--tag", default="autolock")
    ap.add_argument("--logdir", default="/data/dm/logs/serial")
    ap.add_argument("--ignore", action="append", default=[], help="忽略的设备，可多次")
    ap.add_argument("--probe", type=float, default=1.5, help="每个波特率采样秒数")
    ap.add_argument("--timeout", type=float, default=0, help="等待设备秒数，0=不限")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        selftest()
        return 0

    bauds = [int(x) for x in args.bauds.split(",") if x.strip()]
    ignore = set(args.ignore)
    seen = set(list_devices())
    busy_reported = set()
    deadline = time.time() + args.timeout if args.timeout else None

    print("watching; existing=%s ignore=%s" % (sorted(seen), sorted(ignore)), flush=True)
    while True:
        if args.port:
            cands = [args.port] if os.path.exists(args.port) else []
        else:
            cands = [d for d in list_devices() if d not in seen and d not in ignore]
        for port in cands:
            if port in ignore:
                continue
            baud, why = (args.baud, "forced") if args.baud else pick_baud(port, bauds, args.probe)
            if why == "busy":
                if port not in busy_reported:
                    print("%s busy (被独占，跳过)" % port, flush=True)
                    busy_reported.add(port)
                continue
            busy_reported.discard(port)
            if baud is None:
                print("%s 无可识别输出，继续等" % port, flush=True)
                continue
            print("lock %s @%d (%s)" % (port, baud, why), flush=True)
            rc = start_capture(port, baud, args.tag, args.logdir)
            print("dm_serial_log start rc=%d" % rc, flush=True)
            return rc
        if deadline and time.time() > deadline:
            print("timeout，未发现可用设备", flush=True)
            return 2
        current = set(list_devices())
        # 设备拔掉后要从 seen 移除，重新插上才算"新设备"
        seen = (seen & current) | (current - seen)
        time.sleep(0.5)


if __name__ == "__main__":
    sys.exit(main())
