#!/usr/bin/env python3
"""voice_trace.py — 把串口日志里的 TURN 打点还原成每轮语音的时间轴。

配合 dm_serial_log.py 常驻采集使用。板端 voice_channel.c / agent_loop.c
在 P341 之后每轮输出：

    [voice] TURN begin(ptt_up) xrun0=3 short0=1
    [voice] TURN asr_start +812ms
    [voice] TURN asr_done +2841ms
    [voice] TURN llm_req#0 +2843ms
    [voice] TURN llm_done#0 +9120ms
    [voice] TURN tts_start +9124ms
    [voice] TURN tts_first_chunk +10180ms
    [voice] TURN tts_first_write +10182ms
    [voice] TURN end(speak_done) +21400ms xrun=0 short=0

所有 +Nms 都相对"松手"（ptt_up），所以这里只做减法，不做时钟换算。

用法：
    python3 voice_trace.py                     # 读 logs/serial/latest.log
    python3 voice_trace.py -f 某个.log
    python3 voice_trace.py -n 5                # 只看最近 5 轮
    python3 voice_trace.py --json
"""

import argparse
import json
import os
import re
import sys

DEFAULT_LOG = "/data/dm/logs/serial/latest.log"

# 2026-11-01T15:39:21.385+00:00 RX   [voice] TURN ...
RE_LINE = re.compile(r"^(?P<ts>\S+)\s+RX~?\s+(?P<body>.*)$")
RE_TURN = re.compile(r"\[voice\]\s+TURN\s+(?P<rest>.+)$")
RE_BEGIN = re.compile(r"begin\((?P<why>[^)]*)\)\s+xrun0=(?P<x>\d+)\s+short0=(?P<s>\d+)")
RE_END = re.compile(r"end\((?P<why>[^)]*)\)\s+\+(?P<ms>\d+)ms\s+xrun=(?P<x>\d+)\s+short=(?P<s>\d+)")
RE_MARK = re.compile(r"(?P<tag>\S+)\s+\+(?P<ms>\d+)ms\s*$")


def parse(path):
    turns = []
    cur = None
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for raw in f:
            m = RE_LINE.match(raw.rstrip("\r\n"))
            if not m:
                continue
            t = RE_TURN.search(m.group("body"))
            if not t:
                continue
            rest = t.group("rest").strip()
            mb = RE_BEGIN.search(rest)
            if mb:
                cur = {
                    "ts": m.group("ts"),
                    "why": mb.group("why"),
                    "xrun0": int(mb.group("x")),
                    "short0": int(mb.group("s")),
                    "marks": [],
                    "end_ms": None,
                    "xrun": None,
                    "short": None,
                }
                turns.append(cur)
                continue
            me = RE_END.search(rest)
            if me and cur is not None:
                cur["end_ms"] = int(me.group("ms"))
                cur["end_why"] = me.group("why")
                cur["xrun"] = int(me.group("x"))
                cur["short"] = int(me.group("s"))
                cur = None
                continue
            mm = RE_MARK.search(rest)
            if mm and cur is not None:
                cur["marks"].append((mm.group("tag"), int(mm.group("ms"))))
    return turns


def stage(cur, tag):
    """取某个打点的相对时间；同 tag 多次出现取第一次。"""
    for t, ms in cur["marks"]:
        if t == tag:
            return ms
    return None


def delta(cur, a, b):
    x, y = stage(cur, a), stage(cur, b)
    if x is None or y is None:
        return None
    return y - x


def show_turn(i, cur):
    print(f"\n#{i}  松手 {cur['ts']}")
    prev = 0
    for tag, ms in cur["marks"]:
        print(f"    +{ms:>7}ms  (+{ms - prev:>6})  {tag}")
        prev = ms
    if cur["end_ms"] is not None:
        print(f"    +{cur['end_ms']:>7}ms  (+{cur['end_ms'] - prev:>6})  "
              f"end({cur.get('end_why', '')})  "
              f"xrun={cur['xrun']} short={cur['short']}")
    else:
        print("    （没有 end —— 本轮中断或日志截断）")

    rows = [
        ("ASR", "asr_start", "asr_done"),
        ("LLM", "llm_req#0", "llm_done#0"),
        ("TTS首包", "tts_start", "tts_first_chunk"),
        ("首音", "ptt", "tts_first_write"),
    ]
    parts = []
    for name, a, b in rows:
        if a == "ptt":
            v = stage(cur, b)
        else:
            v = delta(cur, a, b)
        if v is not None:
            parts.append(f"{name}={v}ms")
    if parts:
        print("    分段：" + "  ".join(parts))


def median(xs):
    xs = sorted(xs)
    if not xs:
        return None
    n = len(xs)
    return xs[n // 2] if n % 2 else (xs[n // 2 - 1] + xs[n // 2]) // 2


def summarize(turns):
    done = [t for t in turns if t["end_ms"] is not None]
    firsts = [stage(t, "tts_first_write") for t in done]
    firsts = [v for v in firsts if v is not None]
    asrs = [delta(t, "asr_start", "asr_done") for t in done]
    asrs = [v for v in asrs if v is not None]
    llms = [delta(t, "llm_req#0", "llm_done#0") for t in done]
    llms = [v for v in llms if v is not None]

    print("\n=== 汇总 ===")
    print(f"回合数      {len(turns)}（完整 {len(done)}）")
    for name, xs in (("ASR", asrs), ("LLM", llms), ("首音", firsts)):
        if xs:
            print(f"{name:<10} 中位 {median(xs)}ms  最小 {min(xs)}ms  最大 {max(xs)}ms  n={len(xs)}")
    tot_x = sum(t["xrun"] or 0 for t in done)
    tot_s = sum(t["short"] or 0 for t in done)
    print(f"xrun 合计   {tot_x}")
    print(f"短写合计    {tot_s}")
    if tot_s:
        print("  → 短写不为 0：writei 返回帧数少于请求，剩余样本被丢弃（断音候选原因）")
    if tot_x:
        print("  → xrun 不为 0：DMA 缓冲被抽干，说明供数中断（网络停顿或线程被阻塞）")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-f", "--file", default=DEFAULT_LOG)
    ap.add_argument("-n", "--last", type=int, default=0, help="只看最近 N 轮")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.file):
        sys.exit(f"找不到日志：{args.file}")

    turns = parse(args.file)
    if not turns:
        sys.exit("日志里没有 TURN 打点（板端是否为 P341 之后的固件？）")

    if args.last:
        turns = turns[-args.last:]

    if args.json:
        print(json.dumps(turns, ensure_ascii=False, indent=2))
        return

    for i, cur in enumerate(turns, 1):
        show_turn(i, cur)
    summarize(turns)


if __name__ == "__main__":
    main()
