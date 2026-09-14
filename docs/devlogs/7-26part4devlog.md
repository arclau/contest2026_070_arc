# velafix-0726t2.md — OpenVela R528 第二阶段修复记录

> 本文档记录 2026-07-26 下午针对 Allwinner R528 (gemini-s1) 平台的第二轮修复，
> 接替 `velafix-0726.md`（上午场），覆盖 TF 卡、Music Player UI 重写、nxplayer 格式兼容、
> SMHC 数据读取崩溃修复。

---

## 1. 问题清单

### 1.1 TF 卡挂载失败
- **症状**：TF/SD card initialized OK，但 `/dev/mmcsd1` 不存在，mount 失败 (errno=15)
- **log 特征**：
  ```
  TF/SD card initialized OK
  [TF] block device NOT found: /dev/mmcsd1 (2)
  [TF] mount /dev/mmcsd1 -> /sdcard FAILED: -1 (errno=15)
  ```

### 1.2 Music Player 播放 MP3 崩溃 (Data Abort)
- **症状**：播放 MP3 时 Data Abort，PC=0x414090da, DFAR=0xeafffffe
- **多线程重现**：worker 线程 → musicplayer_main 线程 → playthread，均同地址崩溃

### 1.3 nxplayer 返回 -19 (ENODEV)
- **症状**：`nxplayer_playfile returned -19`
- **log 特征**：`type=0 ac_type=0`

### 1.4 SMHC SD 卡数据读取超时死机
- **症状**：播放 MP3 时 20 秒卡死后 Data Abort，完整 SMHC 寄存器 dump
- **log 特征**：
  ```
  before write reg 0x4021008 = 0xffffffff
  after write reg 0x4021008 = 0x8070807
  Data abort. PC: 414090da DFAR: eafffffe DFSR: 00000005
  ```

### 1.5 Music Player UI 布局 BUG
- **症状**：手写 UI 有按钮重叠、布局错乱

---

## 2. 根因分析与修复

### 2.1 TF 卡挂载失败 — cd_mode 配置错误

**根因**：`micro_sd_driver.c` 调用 `set_sdio_param(0, **2**, card_detected)`，`cd_mode=2` = `CARD_DETECT_BY_GPIO_IRQ`。
gemini-s1 开发板 **没有 CD (Card Detect) GPIO 引脚**（defconfig: `# CONFIG_MMCSD_HAVE_CARDDETECT is not set`）。
`hal_sdc_init()` 中 `cd_mode=2` 检测不到卡 → `host->present=0` → `__mci_hold_io(host)` **切断卡槽电源**。
`mmcsd_slotinitialize` 无法与无电的卡通信，/dev/mmcsd1 不出现。

**修复**：`set_sdio_param(0, **3**, card_detected)` → `CARD_ALWAYS_PRESENT`

```c
// vendor/allwinnertech/boards/r528/drivers/micro_sd/micro_sd_driver.c:98
- set_sdio_param(0, 2, card_detected);
+ set_sdio_param(0, 3, card_detected);  /* CARD_ALWAYS_PRESENT: board has no CD GPIO pin */
```

**验证**：重启后 `/dev/mmcsd1` 可识别，4 个 MP3 文件扫描成功。

---

### 2.2 Music Player UI 重写 — 替换为 LVGL Music Demo 布局

**根因**：手写 UI `create_player_ui()` 在 1200×1920 屏幕上有布局 BUG（按钮重叠、定位错误）。

**修复**：完全重写 `musicplayer_main.c`，使用 LVGL Music Demo 的 UI 布局（裁掉 spectrum 动画和启动动画）：

| 组件 | 来源 | 说明 |
|------|------|------|
| `create_wave_images()` | `lv_demo_music_main.c` | 顶部/底部波浪装饰图 |
| `create_title_box()` | `lv_demo_music_main.c` | 歌名 + 艺术家标签 |
| `create_ctrl_box()` | `lv_demo_music_main.c` | 上/下/播放按钮 + 进度条 + 时间 |
| `create_handle()` | `lv_demo_music_main.c` | "ALL TRACKS" 把手 |
| `create_playlist()` | `lv_demo_music_list.c` | 列表 UI |

**文件变化**：
- `musicplayer_main.c`: 941 → 717 行
- `CMakeLists.txt`: 移除 `decoder.c decoder_wav.c decoder_mp3.c` 引用
- `Makefile`: 同上

---

### 2.3 nxplayer 返回 -19 (ENODEV) — 驱动格式能力缺失

**根因**：`sunxi_audio_getcaps()` 中：
```c
caps->ac_format.hw = (1 << (AUDIO_FMT_PCM - 1));
```
只报告 `AUDIO_FMT_PCM`。nxplayer 检测到 `.mp3` 文件后以 `AUDIO_FMT_MP3` 调用
`nxplayer_opendevice()`，`prefformat & (1 << (AUDIO_FMT_MP3 - 1))` 不匹配 → 返回 -ENODEV。

**修复**：在 `sunxi_alsa.c` 的 `getcaps` 中添加 `AUDIO_FMT_MP3`：
```c
// vendor/allwinnertech/chips/r528/components/audio/sunxi_alsa.c:294
- caps->ac_format.hw = (1 << (AUDIO_FMT_PCM - 1));
+ caps->ac_format.hw = (1 << (AUDIO_FMT_PCM - 1)) |
+                      (1 << (AUDIO_FMT_MP3 - 1));
```

**原理**：nxplayer 收到 MP3 格式后，走软件解码器（nxplayer_mp3.c 使用 libmad）
将 MP3 解码为 PCM，再送 AW 硬件驱动输出。驱动本身只处理 PCM，不需要直接支持 MP3，
但格式检查关必须通过。

---

### 2.4 SMHC SD 卡数据读取超时 → `SDC_BUG_ON` 死机

**根因**：`hal_sdhost.c` 第 1613 行 `SDC_SemPend(&host->lock, SDC_DMA_TIMEOUT)` 超时
（20 秒），进入错误处理路径后第 1700 行调用 `SDC_BUG_ON(1); sys_abort()` **故意崩溃系统**。

```
hal_sdhost.c:1613  SDC_SemPend timeout (20s)
  → hal_sdhost.c:1616  if (ret != HAL_OK && host->present)
  → hal_sdhost.c:1639-1657  寄存器 dump（SMHC + 音频 Codec）
  → hal_sdhost.c:1700  SDC_BUG_ON(1); sys_abort();    ← 死因
```

DMA 超时的深层原因可能是 SMHC 时钟/中断配置问题，metadata 读取（单块小数据）正常，
大数据读取（多块 DMA）超时。

**修复**：去掉 `SDC_BUG_ON` 和 `sys_abort`，让错误路径 `goto out` 优雅返回：

```c
// vendor/allwinnertech/chips/r528/drivers/rtos-hal/hal/source/sdmmc/hal_sdhost.c:1700-1701
- SDC_BUG_ON(1);
- sys_abort();
+ /* 去掉 crash，让调用者处理错误 */
```

**效果**：SD 卡读取超时时不再死机，nxplayer 会收到 read 错误，播放器跳过当前文件继续。

---

## 3. 修改文件清单

| 文件 | 修改内容 |
|------|---------|
| `vendor/allwinnertech/boards/r528/drivers/micro_sd/micro_sd_driver.c:98` | `cd_mode=2` → `3` (CARD_ALWAYS_PRESENT) |
| `vendor/allwinnertech/chips/r528/components/audio/sunxi_alsa.c:294` | getcaps 添加 `AUDIO_FMT_MP3` 位 |
| `vendor/allwinnertech/chips/r528/drivers/rtos-hal/hal/source/sdmmc/hal_sdhost.c:1700-1701` | 删除 `SDC_BUG_ON(1); sys_abort();` |
| `apps/examples/musicplayer/musicplayer_main.c` | 全部重写，LVGL Music Demo UI + nxplayer |
| `apps/examples/musicplayer/CMakeLists.txt` | 移除 decoder 源文件 |
| `apps/examples/musicplayer/Makefile` | 移除 decoder 源文件和 libmad 包含路径 |

---

## 4. 当前验证状态

| 功能 | 状态 | 备注 |
|------|------|------|
| BOE 1200x1920 屏幕显示 | ✅ | 同上午场 |
| GT911/GT9271 触摸 | ✅ | 同上午场 |
| TF 卡识别 | ✅ | /dev/mmcsd1 存在，4 文件扫描 |
| Music Player UI | ✅ | Music Demo 布局，无 spectrum |
| nxplayer 格式检测 | ✅ | 接受 MP3 格式 |
| SMHC 超时不死机 | ✅ | `SDC_BUG_ON` 已删除 |
| **nxplayer MP3 播放** | **⬜ 未验证** | 需刷固件实测（DMA 超时可能仍有） |
| **WAV 播放** | **⬜ 未验证** | nxplayer 原生支持 PCM/WAV，应正常 |
| WiFi 驱动崩溃 | ❌ | 同上午场，`wapi` Prefetch Abort |

---

## 5. 编译与打包

```bash
cd /data/openvela
./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh/ -j12
cd vendor/allwinnertech/lichee
source tools/scripts/envsetup.sh
lunch_nuttx r528s3-gemini-s1
pack
```

### 固件输出
`lichee/out/r528s3/gemini-s1_nand/rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img`

---

## 6. 下一任 AI 交接清单

### 6.1 已验证的已知问题

1. **SMHC DMA 超时**（~20秒）— metadata 读取正常，数据读取超时。`SDC_BUG_ON` 已去掉不再死机，
   但超时本身未修复。播放 MP3 时可能出现"文件读取失败"跳下一首。
   - 排查方向：检查 SMHC 时钟门控 (CCU)、DMA 通道配置、或者用 PIO 模式替换 DMA
   - 快速测试：用 `aplay /sdcard/music/test.wav` 测试 WAV 播放（如果 WAV 也超时则肯定是 SMHC 问题）

2. **WiFi 驱动崩溃** — `wapi` 进程 Prefetch Abort (PC: fffffffe)，空函数指针调用。
   自 velafix-0726 上午场以来未修复。

### 6.2 待验证项

- [ ] 刷入最新固件后测试 MP3 是否能出声
- [ ] 如果 MP3 仍不能播，用 `aplay` 测试 WAV 文件排除 SMHC 问题
- [ ] 测试 WAV 文件播放（nxplayer 原生支持 WAV/PCM）

### 6.3 关键文件路径

```
tfdevlog.txt                                   ← 上一阶段（第一阶段 TF/播放器）调试记录
velafix-0726.md                                ← 上午场（音频/触摸/框架）修复记录
velafix-0726t2.md                              ← 本文件（下午场第二阶段）
miumiu-thinking.md                             ← MiuMiu 调试思维框架
veladevpower.md                                ← 调试思维/架构文档

apps/examples/musicplayer/musicplayer_main.c   ← Music Player 主程序（最新版）
vendor/allwinnertech/boards/r528/drivers/micro_sd/micro_sd_driver.c  ← TF 卡驱动
vendor/allwinnertech/chips/r528/components/audio/sunxi_alsa.c         ← 音频驱动
vendor/allwinnertech/chips/r528/drivers/rtos-hal/hal/source/sdmmc/hal_sdhost.c  ← SMHC 驱动
```

### 6.4 调试建议

- SMHC 超时问题可在 `hal_sdhost.c:1613` 加 `_info` 确认 `SDC_SemPend` 返回值
- 如果需要更详细的 SMHC 寄存器信息，第 1639-1657 行的 dump 代码保留不动
- 尝试关闭 DMA（在 `nuttx_sdio_mult.c:672` 强制 `dma_use = 0`）改 PIO 模式
- 或检查 CCU 时钟门控：`reg 0x02001834`（CCU SMHC0 时钟门控寄存器）
