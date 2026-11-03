# P327 — 开机提速（保守档）：Wi-Fi 启动脚本去掉固定等待

2026-10-31；V1 `ynm3000`。用户指定“保守优化，不要激进导致故障”，
范围选 A：只改启动脚本，不动 U-Boot。

## 基线（旧包冷启动实测）

同一份串口日志 `logs/serial/20261031-050733_p317-pet-read.log`，
主机 UTC 15:25:16 那次启动（板端时钟不可用）。

| 事件 | 时间 | 与上电关系 |
|---|---|---|
| BOOT0 / U-Boot | 15:25:16.011 | 基准 |
| partinfo（读 bootloader 分区前） | 15:25:18.935 | +2.92s |
| dst_addr / jump to rtos | 15:25:27.654 | 读分区耗时 8.72s |
| NuttX 日志开始 | 15:25:28.251 | +12.24s |
| lv_nuttx_uv_fb_init | 15:25:33.553 | +17.54s（桌面可操作代理指标） |
| Wi-Fi 脚本结束 / Boot nsh ok | 15:26:04.345 | +48.34s |

Wi-Fi 阶段逐步耗时（脚本 `set -x` 回显 + 驱动日志）：

| 步骤 | 起止 | 实际耗时 |
|---|---|---|
| wapi disconnect | 32.785 | 0 |
| sleep 1 | 32.785→33.862 | 1.00 |
| wapi reconnect | 33.862→38.932 | 5.07（阻塞到关联返回） |
| sleep 5 | 38.932→43.933 | 5.00 |
| wapi scan | 43.933→48.438 | 4.50（阻塞到扫描结果返回） |
| sleep 3 | 48.438→51.454 | 3.00 |
| wapi reconnect | 51.454→56.024 | 4.57（阻塞） |
| sleep 5 | 56.024→01.048 | 5.00 |
| renew（第一次 DHCP） | 01.048→01.174 | 0.13，成功 |
| sleep 3 | 01.174→04.176 | 3.00 |
| renew（第二次 DHCP） | 04.176→04.343 | 0.17，重复 |
| ntpc / show / done | 04.343→04.345 | 0 |

合计 31.56s；其中 reconnect/scan 本身是阻塞调用（16.14s），
固定 sleep 17s + 重复 DHCP 0.17s 属于纯等待。

## 改动（唯一文件）

`vendor/allwinnertech/boards/r528/ynm3000/src/etc/wifi/start_wifi.sh`

- 保留命令顺序（disconnect → reconnect → scan → reconnect → renew → ntpc）。
- 三处 reconnect/scan 之后的固定 sleep 5/3/5 改为 1s 稳定间隔。
- 第二次 `renew` 改为按第一次返回码重试：成功则不重发，
  失败最多再补两次（间隔 3s），覆盖关联晚于 reconnect 返回的情况。
- 注释行仍 < 63 字节（P325 的 NSH 64 字节行限制）。
- 未改 rcS.nsh、未改 Wi-Fi 驱动、未动 ai_agent 启动顺序（仍在 Wi-Fi 之后）。

差异：`output/p327-wifi-boot-trim/start_wifi.sh.before|after`。

## 预期收益（按旧包实测推算，尚未上板）

正常联网：1+5.07+1+4.50+1+4.57+1+0.13 ≈ 18.3s，
比旧脚本 31.56s 少约 13.3s；Boot nsh ok 预计 48.3s → 约 35s。
桌面可操作时间不受影响（Wi-Fi 脚本在 UI 启动之后、ai_agent 之前）。

失败场景不缩短：DHCP 三次都失败才放弃，与旧脚本“两次尝试”同量级，
且每次 renew 内部自带 DHCP 重试。

## 构建与产物

官方流程 `pack_deskmate.sh ynm3000`（编译 + 打包 + 校验）。
注意：增量编译不会重生成 romfs——`etctmp.c` 的依赖里 BOARD_DIR 展开为空，
只有目标缺失才会重跑配方；本轮删掉旧 `etctmp.c/etctmp.o/libboard.a` 强制重生成，
并解码 `etctmp.c` 确认含新脚本后才打包。

- 新包 `output/ynm3000/ynm3000.img` MD5=`593435b1d89378b67774d62f41a39889`
- `vela.bin` == `nsh.fex` = `0af74fbd908004ac913e9dd70eaf98ed`
- 校验 PASS：配置 / 入口 / 启动 / 121 资源 / 整包
- 旧包备份 `output/ynm3000/ynm3000-p325.img`（MD5 12d02c0a…）

## 未做与待验

- 未刷机。串口在 15:50 掉线（`could not open port /dev/ttyUSB0`），
  无法先做在线 A/B，也无法复验新脚本。
- 待验：冷启动后 Wi-Fi 一次连上、IP 有效、ai_agent 报 Network connected、
  以及 Boot nsh ok 的实际时刻。
- 桌面时间 17.5s 的瓶颈在 U-Boot：默认 env 的
  `bootcmd=sunxi_flash read 41400000 bootloader;boot_rtos 41400000`
  不带长度，把 50MB bootloader 分区整块读进 0x41400000，实测 8.72s ≈ 6MB/s；
  nsh.fex 只有 8.4MB。加长度参数（如 16MB）预计省 5.9s，但需重编 u-boot，
  失败要靠重刷恢复，本轮按用户选择不做。
- LED 上电闪灯测试 0.4s（`luncher_dm.c` 两处 usleep 200ms）本轮不动。

## 交接（2026-10-31 收尾，次日刷机）

- 串口 `/dev/ttyUSB0` 15:50 起不存在，板子离线；新包未刷、未做在线 A/B。
- 待刷：`output/ynm3000/ynm3000.img`（`593435b1d89378b67774d62f41a39889`）。
  旧包留底 `output/ynm3000/ynm3000-p325.img`。
- 刷机后用断电重上电冷启动（`reboot` 已知可能挂死），按下列顺序取证据：
  1. `dm_serial_log.py read --from-boot 1 --grep 'wapi|renew|wifi startup done|Boot nsh ok'`
     对时：确认 disconnect→done 由 31.56s 降到约 18s；
  2. `wapi show wlan0` 有 IP、ai_agent 打印 `Network connected`；
  3. `lv_nuttx_uv_fb_init` 与 `Boot nsh ok` 两个时刻（桌面时间本轮预期不变，约 17.5s）。
- 若 Wi-Fi 未连上：先看 renew 是否三次失败，再回退 `output/p327-wifi-boot-trim/start_wifi.sh.before`
  比对；不要顺手加大 sleep 了事，先确认是关联慢还是 DHCP 慢。
- 未做：U-Boot 读取长度（预计 -5.9s，需重编 u-boot，待用户另行拍板）、LED 0.4s。

## 2026-10-31 23:38 UTC：烧录后自动启动观察

串口采集器在 23:37:32 重新接入，随后记录到 USB 烧录、NAND 擦写、
`FEX_CMD_fes_verify_status` 返回 0，以及烧录后的自动启动。
该次启动的脚本回显含四处 1 秒等待和按返回码跳过的 DHCP 重试分支，
与 P327 新脚本一致；本机无法从串口独立证明烧录镜像的完整哈希。

| 主机 UTC 事件 | 时刻 | 距 BOOT0 |
|---|---|---|
| BOOT0 | 23:38:38.756 | 0 |
| `wapi disconnect` | 23:38:52.374 | +13.618s |
| `lv_nuttx_uv_fb_init` | 23:38:53.129 | +14.373s，仍只是桌面事件循环代理指标 |
| `wifi startup done` | 23:39:06.626 | +27.870s，脚本耗时约 14.252s |
| `Boot nsh ok` | 23:39:06.627 | +27.871s |
| `ai_agent Network connected` | 23:39:06.675 | +27.919s；联网成功 |

旧包同一主机采集的冷启动脚本耗时 31.56s、`Boot nsh ok` +48.336s。
本次烧录后自动启动的脚本少约 17.3s，整段启动少约 20.5s；
两次网络调用耗时和启动条件不完全相同，不把整段差值全归因于脚本修改。
还需断电重上电冷启动复测，并现场确认桌面可操作；当前只验证了日志代理指标。
