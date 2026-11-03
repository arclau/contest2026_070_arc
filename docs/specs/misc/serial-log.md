# 串口日志框架（面向 AI agent）

入口主源：`/data/dm/contest2026_070_arc/tools/dm_serial_log.py`。只依赖 `pyserial`，不构建、不写固件。

## 为什么有它

排障要看板端输出，但开机只有一次、串口会掉线、日志散在临时文件里。这个框架把串口做成**常驻、分段、可检索**的证据流：一次 `start` 之后板子重启、拔插、刷机都不用重新开，每次启动自动打 `BOOT #n` 分隔。

## 日常用法

```bash
T=/data/dm/contest2026_070_arc/tools/dm_serial_log.py

python3 $T start --baud 1500000 --tag p275     # 启动常驻采集，打印会话路径
python3 $T status                              # 链路/流量/启动次数
python3 $T mark "flash 95df86ae"               # 人工切一刀，便于 read --from-mark
python3 $T send "ls /dev" --wait 1.5           # 发命令并直接拿到回显
python3 $T tail -n 80
python3 $T summary                             # agent 摘要：面板/背光/触摸/DSI/故障
python3 $T read --from-boot 1 --grep "pwm_|BOE1440|\[I2C\]"
python3 $T stop
```

刷机/打包这类动作可以整段包起来，前后自动写 MARK：

```bash
python3 $T wrap --tag flash -- bash /data/dm/vendor/allwinnertech/tools/pack_deskmate.sh ynm3000
```

## 日志格式

每行固定前缀，agent 直接按空格切第 2 段：

```
2026-10-20T00:37:12.345+00:00 RX   设备输出（完整行）
2026-10-20T00:37:12.345+00:00 RX~  设备输出（无换行，空闲 150ms 强制落盘，保留 nsh> 这类提示符）
2026-10-20T00:37:12.345+00:00 TX   本机发送
2026-10-20T00:37:12.345+00:00 MARK 人工标记
2026-10-20T00:37:12.345+00:00 BOOT #1 <命中的启动特征>
2026-10-20T00:37:12.345+00:00 LINK open|closed|error <说明>
```

- 落盘目录 `/data/dm/logs/serial/`，会话名 `<时间>_<tag>.log`，**只追加不覆盖**。
- 同名 `.meta.json` 存端口、波特率、行数、字节数、启动次数、起止时间；`latest.log` 软链指向当前会话。
- 控制通道是 `/tmp/dm_serial/<port>.sock`（UNIX socket），不走串口，不会污染设备输入。

## agent 读取约定

| 需求 | 用法 |
|---|---|
| 这次开机做了什么 | `read --from-boot 1` |
| 只看某类行 | `read --grep <正则>`，`--json` 拿结构化结果 |
| 某次实验之后 | `mark` 打点 → `read --from-mark <文本>` |
| 一屏结论 | `summary`：panel / backlight / touch / dsi / fault 五组计数与样本 |
| 历史会话 | `list`，或 `--session <前缀>` |

`summary` 只做**行级关键字聚合**，用于快速定位，不代替逐行判读；判定根因仍以具体行为准。

## 自测与边界

```bash
python3 $T selftest    # pty 环回：分段、残行、BOOT 识别、send/read、meta、软链
```

- 不需要硬件；pty 环回验证的是**框架行为**，不证明板端真的在输出。
- 串口只读不改板子状态；`send` 会真的写设备，等价于人在终端敲命令。
- 拔线时守护进程每秒重试打开，日志记 `LINK error/closed/open`，不会退出。
- 日志含板端全量输出，导出/交付前按 AGENTS §六 扫凭据。
