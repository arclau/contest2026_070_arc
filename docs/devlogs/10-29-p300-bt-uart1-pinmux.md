# P300 — V1 蓝牙 E1：UART1 引脚归还板载 RTL8723

> 2026-10-29。目标：定位 V1 `ynm3000` 蓝牙 2026-09-15 起不可用（`bthci open timeout -110`）的原因，并做一次可回退的验证实验。板端未刷，结论待验。

## 起点：上游有实现，本地驱动比上游还新

`gitee open-vela/vendor_allwinnertech` 的 `dev` 分支确实没有蓝牙目录，但 **`dev-ai-contest-2026` / `trunk` 有完整实现**（此前查错分支）。逐文件对比：

| 文件 | 上游 | 本地 |
|---|---|---|
| `rtk_hci.c` 流控 | 无条件 `#define CONFIG_X4B_FACTEST` → 关 CRTSCTS + 开 PARENB（8E1） | 2026-08-10 改为 CRTSCTS + 8N1 |
| `rtk_hci_board.c` 复位 | low 220ms → high 220ms | high 后加 1s |
| `r528_boot.c` | `rtk8723FS_initialize(0)` 正常调用 | 被注释 |
| `r528_serial.c` UART1 | **PG6 TX / PG7 RX / PG8 RTS / PG9 CTS，mux2** | **PD21 TX / PD22 RX，mux4** |

驱动侧本地更完整，差异只在板级引脚。

## 根因假设（未确认，按纪律不升级为根因）

BT 驱动走 `/dev/uart1`，且依赖 PG8/PG9 硬件流控；2026-08-27 P122 为 Settings 的 UART 调试工具把 UART1 改到 PD21/PD22，PG 组不再复用到 UART1，芯片收不到 `sync_req` → 3s 超时 -110。时间线（8-27 改脚 → 9-15 报 -110 并关闭）吻合。

## 唯一实验变量

一次只验证"引脚"，其余保持不动：

- `chips/r528/r528_serial.c`：`r528_uart1config` 恢复 PG6/PG7/PG8/PG9 mux2（与上游一致）。
- `chips/r528/r528_boot.c`：放开 `rtk8723FS_initialize(0)`。
- `boards/r528/ynm3000/src/etc/init.d/rcS.nsh`：放开 `bluetoothd &`。
- **不动** `rtk_hci.c` 的 CRTSCTS/8N1，不动复位延时，不动 defconfig。

已知副作用：Settings 里的 UART 调试工具（`dm_uart_dbg.c:40` 也是 `/dev/uart1`）在 E1 下失效；该工具只在子页打开时 open，不影响开机。

## 构建与产物

- 编译：`./build.sh vendor/allwinnertech/boards/r528/ynm3000/configs/deskmate -j`，rc=0。
- `nm nuttx.elf` 出现 `rtk8723FS_initialize` / `bthci_open`，驱动已真正链接（此前因 init 被注释被链接器丢弃）。
- 坑：`rcS.nsh` 由 `rcS` 通过 `#include` 引入，Makefile 只把 `etc/init.d/rcS` 当依赖，**改 `rcS.nsh` 不会触发 etctmp 重建**。需 `touch src/etc/init.d/rcS` 再编，否则镜像里还是旧脚本。
- 打包：`pack_img.sh -c sun8iw20p1 -p rtos -b r528s3-ynm3000 …`，rc=0。
- 产物：`output/p300-bt-e1/ynm3000-bt-e1.img`，54,414,336 B，MD5 `35a9c85a30c01a42aaf26dacf1d6a9f1`；`vela.bin==nsh.fex`；镜像内含 `Starting bluetoothd`。
- 验证清单：`output/p300-bt-e1/verification.json`。

## UI 对接（同日追加）

`ui_bt.c` 的页面结构（状态卡 / 蓝牙开关 / 可被发现开关 / 我的设备 / 其他设备 / 配对弹窗）此前已存在，但接线有四处是"看起来能用"：

| 问题 | 原实现 | 现在 |
|---|---|---|
| 适配器状态造假 | `dm_net_bt_set` 直接写 `g_bt_state=ON`，enable 异步失败也显示已开 | 只认 `on_adapter_state_changed` 回调；`is_on` 每 5s 向 bluetoothd 回读一次 |
| 回调注册被覆盖 | `start_scan` 每轮重注册 `adapter_callbacks_t` 且只填 3 个字段，把状态/扫描模式回调清空 | 实例创建时一次性注册全部 5 个回调 |
| 可被发现开关 | 第三个参数是 bondable，不是超时，原代码传 `on?120:0`；开关初值写死 false，不回读 | 传 `BT_SCAN_MODE_CONNECTABLE_DISCOVERABLE` + `bondable=true`；新增 `dm_net_bt_is_discoverable()`，开关按 `on_scan_mode_changed` 回显，适配器关时强制关 |
| 打开开关后不扫描 | 只有进入页面时适配器已开才扫 | 轮询检测 ON 跳变即自动 `start_discovery(15s)`；发现过程中设备数变化就增量刷列表，不必等 15s |

配套：`sim/dm_net_sim.c` 补 `dm_net_bt_is_discoverable` 与可发现状态存储，SIM 下开关可来回切换（设备仍为两条假数据）。

## 阶段 2 开工：A2DP 出声 + AVRCP 控歌（2026-10-29）

### 摸清的现状

- 框架层 `a2dp_sink_audio.c` 只把**编码帧**（SBC / AAC-LATM）交给 `audio_control_write()`，后者在 `CONFIG_AUDIOUTILS_TINYCOMPRESS` 未开时直接 `return -1`（`audio_control.c:350`）→ 手机放歌无声。
- 树里有现成软件 SBC 解码库：`external/libfluoride-sbc`（`CONFIG_LIB_FLUORIDE_SBC=n`，API 为 `OI_CODEC_SBC_DecoderReset/ConfigureRaw/DecodeFrame`）。
- 音频出口有两条：
  - ALSA `/dev/snd/pcmC0D0p`——XPlayer（libcedarx）在用，需要 ALSA ioctl 封装；
  - NuttX audio `/dev/audio/pcm0p`——`r528_boot.c:948` `audio_register("pcm0p", ...)` 注册；lowerhalf **没有 .write**（`sunxi_alsa.c:159` 的 ops 里只有 enqueuebuffer），必须走 `AUDIOIOC_CONFIGURE → START → ENQUEUEBUFFER`。
- 上游 gemini-s1 的 `nsh_minidisplay` defconfig 同样只开 A2DP sink、没开 tinycompress/AVRCP —— 这一块没有可抄的，要自己写。

### 方案（已按实际落地修正）

1. bluetoothd 内新增 `a2dp_sink_pcm.c`：SBC 软解 + PCM 环形缓冲 + 独立线程写 codec。
   **出口改用 aw-alsa-lib `snd_vela_pcm_*`（`hw:audiocodec`）**，与 luncher_dm 播放器同一条已验证通路；原计划的 `/dev/audio/pcm0p` 没有 .write、要自己拼 AUDIOIOC 序列，风险更大。
2. app 侧：A2DP 开始推流时暂停本地 XPlayer，避免两条音频通路抢 codec。
3. AVRCP：开 `CONFIG_BLUETOOTH_AVRCP_TARGET`，`luncher_dm` 注册回调，把 play/pause/next/prev 映射到现有 `music_*`。

### 落地（2026-10-29，编译通过，板端待验）

| 文件 | 改动 |
|---|---|
| `service/profiles/a2dp/sink/a2dp_sink_pcm.c/.h` | 新增。剥 3B LOAS + 1B 帧数 → `OI_CODEC_SBC_DecodeFrame` → 64KB 环形缓冲 → 线程 `snd_vela_pcm_writei`。非 SBC 帧计数丢弃。 |
| `service/profiles/a2dp/sink/a2dp_sink_audio.c` | tick 里 SBC 走 `a2dp_sink_pcm_feed()`（恒返回 len，不再触发 partial）；停止时 `a2dp_sink_pcm_stop()`。 |
| `frameworks/connectivity/bluetooth/Makefile` | sink 段加 `libfluoride-sbc/decoder/include`、`AW_HAL_DIR/include/hal`、`include/hal/aw-alsa-lib`、`include/osal` 四条头路径（均带 CONFIG 守卫）。 |
| `boards/r528/ynm3000/configs/deskmate/defconfig` | `CONFIG_LIB_FLUORIDE_SBC=y`、`CONFIG_BLUETOOTH_AVRCP_TARGET=y`。 |
| `apps/luncher_dm/dm_net.c/.h` | 注册 AVRCP target 面板回调（socket 线程只置待办码）+ A2DP sink 推流状态回调；新增 `dm_net_bt_avrcp_take()` / `dm_net_bt_a2dp_streaming()`。 |
| `apps/luncher_dm/deskmate_ui.c` | 新增常驻 200ms `dm_avrcp_poll_cb`：推流中暂停本地播放；否则把待办码映射到 `music_pause/resume/play/album_next`。 |

两个自查修正：① `a2dp_sink_pcm_stop()` 先 `g_run=false` + `pthread_join` 再 close 设备，避免 writer 卡在 writei 时被 close（UAF）；② 设备恒开 2ch —— 解码器固定 `pcmStride=2`，mono 流由库自己复制到第二声道（`decoder-sbc.c:179`）。

证据：`./build.sh …/ynm3000/configs/deskmate -j$(nproc)` rc=0，日志无 `error:`；
`nm nuttx.elf` 可见 `a2dp_sink_pcm_feed` / `OI_CODEC_SBC_DecodeFrame` / `snd_vela_pcm_open` / `bt_avrcp_target_register_callbacks` / `dm_net_bt_avrcp_take`。
`.config`：`CONFIG_LIB_FLUORIDE_SBC=y`、`CONFIG_BLUETOOTH_AVRCP_TARGET=y`、zblue 侧 `CONFIG_BT_A2DP_SINK=y` / `CONFIG_BT_AVRCP_TARGET=y`。

打包：官方 `pack_img.sh -b r528s3-ynm3000 …` rc=0；产物 `output/p300-bt-a2dp/ynm3000-bt-a2dp.img`，54,444,032 B，MD5 `c3face87f76c22e79e28d9fd466f11f1`；`vela.bin == nsh.fex`（`a092e20d…`）；镜像内含 `Starting bluetoothd` / `pcm ready` / `a2dp_snk_pcm` 字样。清单：`output/p300-bt-a2dp/verification.json`。

待验（板端，宿主当前无 `/dev/ttyUSB*`，板未接）：手机连上后能出声；手机播放/暂停/切歌能控制本机；推流时本地曲库自动暂停。

## 链路检查（2026-10-29，不刷机）

按 `nuttx-driver-development` 的审查流程逐跳核对 BT→PCM→codec，并在宿主用真实 SBC 码流跑真实 `a2dp_sink_pcm.c`（stub 掉 ALSA，把 PCM 落盘比对）。

| # | 环节 | 检查方式 | 结果 |
|---|---|---|---|
| 1 | AVDTP 收包 → repackage → 队列 → tick | `sal_a2dp_interface.c:1034` / `a2dp_sink_audio.c:169` | PASS |
| 2 | 负载格式：3B LOAS + 1B 帧数 + 0x9C | ffmpeg 真码流 689 帧全部消费，0 丢弃 | PASS |
| 3 | SBC 解码正确性 | 与 ffmpeg 解码参考逐样本相关 1.0000，RMS/峰值一致 | PASS |
| 4 | mono / 48k / 一包多帧 | 344 帧→176128B、375 帧→192000B、3 帧/包 125 包 | PASS |
| 5 | 非法/截断负载 | 丢弃+计数，不崩，后续帧继续解 | PASS |
| 6 | hw:audiocodec 卡存在 | `card_default.c:50` 注册 `audiocodec` | PASS |
| 7 | 采样率/声道/格式 | `sun8iw20-codec.c:1581` 播放 8k–192k、1–2ch、S16 | PASS |
| 8 | 缓冲上限 | `sunxi-pcm.c:58` buffer 128KB/period 64KB，本实现 32KB/4KB | PASS |
| 9 | **设备独占** | `snd_pcm.c:1476` `ref_count != 0 → -EBUSY` | **FAIL→已修** |
| 10 | 环形缓冲丢帧对齐 | 原按字节丢 → 左右声道互换 | **FAIL→已修** |
| 11 | 打开失败重试 | 原每帧重试刷日志 | WARN→已修 |
| 12 | AVRCP play/pause 映射 | 原 play 与 pause 都映射成 toggle | **FAIL→已修** |
| 13 | AVRCP TG 链路 | SDP `sal_avrcp_interface.c:1947`、启动 `avrcp_target_service.c:519`、socket `bt_socket_avrcp_target.c:119` | PASS |
| 14 | 回调线程安全 | socket 线程只写待办码，LVGL timer 消费 | PASS |
| 15 | 前提：适配器不自动使能 | 无 AUTO_ENABLE 配置，须先在 Settings 开蓝牙才会注册回调 | 记录为前提 |

### 本轮修掉的三个真问题

1. **本地播放让路无效**：暂停只是 `XPlayerPause`，substream 仍被占，`ksnd_pcm_open` 返回 -EBUSY，手机推流永远打不开设备。改为 `music_audio_force_stop()`（XPlayerStop→PlayerClear→关 sink），并用 `yielded` 锁存避免每 200ms 反复停。
2. **环形缓冲左右声道互换**：丢弃最旧数据时按字节推进 `g_tail`，非 4 字节整数倍就永久错位。改为整帧丢弃/整帧读取。
3. **AVRCP 播放/暂停反转**：手机会分别发 PLAY 与 PAUSE，原实现两者都映射成 toggle。改为独立命令码 `DM_AVRCP_CMD_PLAY/PAUSE/NEXT/PREV`。

### 宿主复现方法（可重跑）

源码与脚本：`output/p300-bt-a2dp/host-test/`。编译真实 `a2dp_sink_pcm.c` + libfluoride-sbc 解码器，stub 掉 `pcm.h`，把 PCM 落到文件后与 `ffmpeg -f sbc` 的解码结果比对。

### 残留风险（板端才暴露）

- `a2dp_sink_pcm_stop()` 在 BT 服务循环里 `pthread_join` 写线程，最长等一次 `writei`（≈180ms），停止瞬间可能推迟其它 BT 消息。
- 手机实际协商的 SBC 参数（joint stereo / bitpool）未在真机验证，宿主只覆盖了 ffmpeg 生成的码流。
- `pcm-audio` 的 `audio_analyzer.py` 因宿主缺 scipy 未跑；改用等价的 numpy 指标（RMS/峰值/声道相关/零值）。

## 外部实现对照（2026-10-29，未刷机）

为确认"别人是否用了不同驱动逻辑"，抽查参赛队与 fork（GitHub `open-vela/contest2026_*`、`vendor_allwinnertech` fork），只抓单文件不拉整库：

| 对象 | 结果 |
|---|---|
| yanxingyu17 `fix/bt-instance-include-path-20260701`、Rustypudding `gemini-s1/kid-buddy-upstream` 的 `rtk_hci.c` / `rtk_hci_board.c` / `r528_boot.c` | 两份之间逐字节相同；与本地差异仅为我们 2026-08-10 的实验（8N1+CRTSCTS 注释、复位 1s 延时），其余逐行一致 |
| gemini-s1 `nsh` defconfig vs ynm3000 deskmate | BT 相关项仅多 `CONFIG_BLUETOOTH_AVRCP_TARGET`；`CONFIG_UART1_BAUD=115200`、设备名默认值一致 |
| `r528_serial.c` UART1 | 参考实现同为 PG6/PG7/PG8/PG9、mux2 |
| 037（FlowDesk）、060（音乐播放器） | 均为 Gemini-S1 + 原生驱动、未改 vendor；060 README 宣称 A2DP 可用（未复现）。037 的 AI 日志里 `rtk8723FS_initialize(0)` 被**注释掉**，注释原文 "Bluetooth disabled — combo chip BT_DIS pin interferes with WiFi SDIO probe"；其自带引脚表把 BT_RESETN 记成 PG17、并承认 PG18/PD21/PD22 是猜测 |
| 组合芯片 GPIO 分工 | 本项目 2026-08-10 会话（用户口述原理图）：**BT_RESETN = PG18**、AP_WAKE_BT = PG17、BT_WAKE_AP = PD16、PB10 = SYS_LED、PG12–15 = BT_PCM。与 `drv_gpio.c` 的 `/dev/gpio2`=PG18 一致；037 的 PG17 说法与之矛盾，不采信 |
| 官方 `nsh_minidisplay` 配置 | BT 相关符号与本地一致（本地多 AVRCP 与 `DEBUG_WIRELESS_*`）；UART1 均为 PG6–9 mux2、115200。差异仅：官方 `UART1_RXDMA/TXDMA=y`、RXBUF 4096；**`r528_serial.c` 无任何 DMA 代码，这两个符号对 `/dev/uart1` 无效** |

结论：不存在"别人的另一套驱动逻辑"，也没有任何队伍公开验证过这块板 BT 可用（037 直接关闭 BT，060 仅 README 声称）。本机 8-10 的全部失败点都在 H5 sync（`filep_h5 open ret:-110`），日志中从未出现 sync 成功。硬件与官方 Gemini-S1 相同，故软件已无参考差异可抄；剩余只能靠板端观测（PG18 复位电平、模块 BT 段 UART_TX/PG7 在复位后 3s 内有无回帧）。板子当前离线（无 `/dev/ttyUSB*`），E1 镜像（`output/p300-bt-e1/`）尚未上板——下一步是刷 E1 包取开机早期日志，确认 sync 是否仍失败，再决定测针。

## 判据（板端）

通过：串口出现 `For normal BT, enable hw flow control` 之后的 `bt download fw takes` / `after hci_read_local_addr`，无 `-110`，`top` 无 bt_recv 占满 CPU1，Settings 蓝牙页能看到适配器状态。

不通过：仍是 `hci_check_local_ver ret:-110` → 下一步 E2（单独关 CRTSCTS，验证 ynm3000 是否没接 PG8/PG9）；E2 仍失败 → E3 查 `/dev/gpio2`（PG18 复位脚，`drv_gpio.c:107`）与模块供电。

## 回退

三个文件均为局部改动，逐条还原即可；回退镜像 `output/p299-rollback-20261029/ynm3000-rollback.img`（P299 回滚包）可直接刷回无蓝牙状态。
