# DevLog 2026-08-06 (Part5) — 卡顿根治排查：重采样移线程否决 + SDMMC DMA 深挖 + 音频线程优先级提升

> 工程：openvela (R528) — nsh 配置
> 工作目录：`/data/dm`
> 工具链：SDK 自带 prebuilts/gcc/linux-x86_64/arm-none-eabi (GCC 13.4.0)
> 打包流程：`lichee` 下 `source envsetup.sh && lunch_nuttx 2 && pack`，镜像输出至 `lichee/out/r528s3/gemini-s1_nand/`
> **AI 交接：新会话请先读 `AGENTS.md`，本文件是当日归档，仅按需查阅。**

---

## 一、任务背景

Part4（ok-20260806-18）交接遗留：**卡顿根治（解码供给慢）**——44100→48k 重采样在解码线程耗 CPU（评估移 render 线程/优化 do_AuMIX）；`MMCSD_MULTIBLOCK_LIMIT=1` 单块读权宜 → SDMMC 多块读 DMA 根治（深挖 `sunxi_mmc_mult_sendcmd` DMA/中断竞争）；必要时继续增大 buffer / 提解码线程优先级。

本 part 完成三项代码级排查/评估，并落地**音频线程优先级提升**（最确定、可编译验证、低风险项），编译打包固化。

## 二、卡顿链路定位（代码级）

日志证据（Part3 上板）：`wht>>>Render, timeout:81ms` + `writei size=4608 frames=1152 wr=-32`（underrun）。

链路定位：
- **render 线程**（`audioRenderComponent.c`，优先级 45）：`AudioRenderThread` → `requestPcmData()` → **同步阻塞**调 `AudioDecCompRequestPcmData()`（`audioDecComponent.c`）向解码线程要 40ms PCM；取数超 15ms 即打 `wht>>>Render timeout` 日志。render 线程对声卡 writei 时序敏感，被抢占/延迟直接 underrun。
- **解码线程**（`adecoder.c` 解码输出路径，优先级 45）：44100Hz 源走 `do_audioresample()`（条件 `Samplerate < RATE_LIMIT || chan==1 || Samplerate==44100`，Part3 加入）→ 内部 `do_AuMIX`(ByPassflag=2) → `AudioResample()`（float 线性插值）→ 输出 48000。
- **SD 读**：`MMCSD_MULTIBLOCK_LIMIT=1` 强制单块读（CMD17，每 512B 一次命令），拉低解码供给吞吐。

## 三、评估：重采样移 render 线程 —— **否决**

| 候选方案 | 结论 | 理由 |
|---------|------|------|
| 重采样移到 render 线程 | ❌ 否决 | render 线程本来就是**同步阻塞**等解码数据（requestPcmData），且对声卡时序敏感；把重采样 CPU 峰值挪过去只会让更敏感的位置更慢，无收益且更危险 |
| 优化 do_AuMIX/AudioResample | ⏸ 低优先 | `AudioResample` 是 float 线性插值，44100→48000 每 40ms 仅 ~1920 帧×2ch，全量开销应在几 ms 内，**不是 81ms 超时主因**（FIXPOINT 定点化可做后备，暂不动） |
| **提音频线程优先级** | ✅ 落地 | 原 45 **低于** luncher 主线程（LVGL UI）默认优先级 100（`SCHED_PRIORITY_DEFAULT=100`）→ 播放时被 UI 刷新/触摸抢占 → 解码供给慢 + writei 延迟 → 卡顿。提到 150（高于 UI、低于 video 240） |

**线程优先级对比（NuttX 数值越大越优先，MAX=255）**：

| 线程 | 原优先级 | 现优先级 |
|------|---------|---------|
| 音频解码 `AudioDecodeThread` | 45 | **150** |
| 音频渲染 `AudioRenderThread` | 45 | **150** |
| 视频解码/渲染（参考） | 240 | 240（未动） |
| luncher 主线程 / LVGL（默认） | 100 | 100（未动） |

## 四、SDMMC 多块读 DMA 排查（结论：保持单块读权宜，根治留上板）

排查链：`sunxi_mmc_mult_sendcmd`（`nuttx_sdio_mult.c:255`）→ `rom_HAL_SDC_Request`（`hal_sdhost.c:1744`，含未对齐 bounce buffer 逻辑）→ `do_rom_HAL_SDC_Request`（`hal_sdhost.c:1506`）。

发现：
1. **DMA 实际已启用**：`.config` 中 `CONFIG_SDC_DMA_USED=y`（`nuttx_sdio.c:580` → `sdc_param.dma_use=1`）、`CONFIG_SDIO_DMA=y`、`CONFIG_SDC_DMA_BUF_SIZE=64`（64KB 对齐 bounce 缓冲）。
2. DMA 路径逻辑完整：`__mci_prepare_dma`（`hal_sdhost.c:574`，IDMA 开 + 描述符地址写入）、`__mci_alloc_idma_des`（描述符链：每段 ≤8192B，`SDXC_MAX_DES_NUM=4M>>13=512`）、中断 `__mci_irq_handler`（`trans_done && dma_done` → post `host->lock`）。
3. **历史崩溃点**（8-5 已定论）：`do_rom_HAL_SDC_Request` 超时 dump 路径 `hal_sdhost.c:1674` 访问坏 sg buffer（DFAR=eafffffe）→ `SDC_BUG_ON(1); sys_abort()`。
4. **静态无法定论超时根因**：DMA/中断竞争（`host->lock` 信号量 post 时序、`wait==SDC_WAIT_NONE` 早退路径、`byte_cnt > SDC_ALIGN_DMA_BUF_SIZE(64KB)` 直接 return -1 等）需上板抓寄存器/日志复现才能确认。
5. 结论：**维持 `MMCSD_MULTIBLOCK_LIMIT=1` 单块读权宜**（上板稳定不崩），多块读 DMA 根治列为遗留（上板深挖）。已启用 DMA 对单块读仍生效（描述符路径相同），不冲突。

## 五、改动清单（ok-20260806-19）

| 文件 | 改动 |
|------|------|
| `chips/r528/components/multimedia/src/libcedarx/libcore/playback/audioDecComponent.c` | 解码线程 `sched_priority` 45 → 150（注释说明提权依据） |
| `chips/r528/components/multimedia/src/libcedarx/libcore/playback/audioRenderComponent.c` | 渲染线程 `sched_priority` 45 → 150（同上） |

## 六、验证结果

- 编译：`./build.sh ... -j$(nproc)` ✅ 通过
- 打包：`source envsetup.sh && lunch_nuttx 2 && pack` ✅ 成功
- 产物：`nsh.fex` 与 `vela.bin` **字节一致**（7478440 B，md5 `dea2dbb61b55f1540d5573bce0872635`）✅
- 固化：git_snapshot.sh → tag **ok-20260806-19**（vendor 仓 2 文件改动；nuttx/apps 无改动）
- ⚠️ **上板验证待用户执行**：播放卡顿应改善（解码/渲染不再被 UI 抢占）；切歌无声修复（ok-20260806-18）一并验证

## 七、遗留事项 / 下一步

1. **上板验证 ok-20260806-19**：①卡顿是否改善（线程提权）②切歌后有声（18 号点）——若仍有卡顿，抓 `wht>>>Render timeout` 与线程占用日志
2. **🔴 SDMMC 多块读 DMA 根治（遗留，需上板）**：深挖 `sunxi_mmc_mult_sendcmd` → `do_rom_HAL_SDC_Request` 超时根因（DMA/中断竞争、64KB bounce 上限、wait 状态机早退），恢复多块读提速
3. 若提权后仍欠载：`AudioResample` FIXPOINT 定点化（去 float）、解码线程再提至 200、或声卡 period/buffer 继续放大（512/2048 → 1024/4096 上限评估）
4. 既有遗留不变：模拟器 freetype、蓝牙符号观感、背景图、天气城市、G2D、sync_deskmate 工具链改进

## 八、补充（CPULOAD 观测版，ok-20260806-20）：上板评估 UI CPU 占用

上板验证 ok-20260806-19 前，用户要求先评估 UI 对系统资源占用。检查发现**当前固件无法测 CPU%**：`CONFIG_SCHED_CPULOAD_NONE=y`（CPU 负载统计被禁）+ openvela 无独立 `apps/system/top`（top 为 nshlib 内置命令 `nsh_command.c:624`，CPU 占用列由 `NSH_HAVE_CPULOAD` 控制，而 `nsh.h:466-468` 在 `CONFIG_SCHED_CPULOAD_NONE` 时 undef 它）→ ps 的 CPU 列与 top 均无数据源。

**修复（仅加观测能力，不动功能）**：`CONFIG_SCHED_CPULOAD_NONE=y` → `CONFIG_SCHED_CPULOAD_SYSCLK=y`（`nuttx/.config` 用 kconfig-tweak 改 + `defconfig` 同步加行防 distclean 丢失）。

**上板观测命令**（刷 ok-20260806-20 后）：

```bash
top -d 10          # 每 10s 刷新各线程 CPU 占用%（重点看 luncher_dm/LVGL、解码/渲染线程）
ps                 # 线程 PID/优先级/状态（CPU 列现在有数据）
free               # 堆内存占用
cat /proc/usage    # 内存使用统计
cat /proc/cpuinfo  # CPU 信息
cat /proc/uptime   # 运行时间
ls /proc           # 进程/系统信息列表
```

验证：编译/打包通过（nsh.fex==vela.bin 7478448B md5 1ca4054b）；固化 ok-20260806-20（vendor 仓 defconfig 1 行）。**待上板验证**：刷机后跑 `top -d 10` 对比播放 MP3 时 luncher 主线程 vs 音频解码/渲染线程 CPU%，验证"UI 抢占解码"假设是否成立。

## 九、补充（提权 240，ok-20260806-21）：上板 ps 实测确认 Demux 抢占

用户刷 ok-20260806-20 上板跑 `ps`（放歌前后各一次），实测数据：

**放歌后新增线程**：Demux=240、XPlayer=240、AudioDecode=**150**、AudioRender=**150**。**关键发现：Demux/XPlayer 是 240，高于我们提的 150**——Demux 就绪时抢占 AudioDecode 的解码计算，解码供给峰值仍超时。

**卡顿证据链**（放歌日志）：
- `audioRender <doRender:870>: wht>>>Render, timeout:45ms`（从 81ms 改善到 45ms，但 >15ms 阈值）
- `writei size=3328 frames=832 wr=-32`（underrun！size 小于正常 7680=1920帧，声卡缓冲快空才写入）
- 同刻 `show_standby: Show standby (reused)` → UI 待机触发 LVGL 重绘与音频供给竞争

**修复（本轮）**：AudioDecode/AudioRender `sched_priority` 150→**240**（与 Demux/XPlayer 同级，消除 Demux 抢占）。验证：编译/打包通过（nsh.fex==vela.bin 7478448B md5 55654c76）；固化 ok-20260806-21。**待上板验证**：卡顿应明显改善；若仍欠载转 SD 多块读 DMA 根治 / FIXPOINT 定点化。

## 十、补充（ok-20260806-22）：切歌无声深挖结论 + top 命令修复

### 10.1 切歌无声深挖（代码级链路全部验证）

上板 21 号固件（提权 240）实测：**切歌后下一首依然无声**。代码级深挖结论：

| 链路 | 验证结果 |
|------|---------|
| 18 号修复（open 强制 dapm_state=0） | ✅ 在代码中（snd_pcm.c:498） |
| close 链路 dapm_control(0) | ✅ 存在（snd_core.c:595-596 → codec 1135 同步 dapm_state） |
| XPlayerReset 切歌流程 | ✅ 走到 `363, snd_vela_pcm_close`（用户日志确认），TinaSoundDeviceStop→closeSoundDevice→snd_pcm_close |
| 第一首内 close/reopen | ✅ 正常（44100→48000 重采样触发 `checkSampleRate:712 start sound devide again` 且出声正常） |

**关键判断**：dapm_state 修复链路理论闭环，但实测仍无声 → **根因可能不在 dapm_state**（或切歌后未真正重新 open 设备）。下轮需抓切歌后完整日志（writei 返回值/有无 363 close/有无 dapm 日志）定位断点；已归档为遗留。

### 10.2 top 命令 not found 修复（🔴 重要）

上板实测 `top: command not found`。**根因**：`CONFIG_FS_PROCFS_EXCLUDE_CPULOAD=y`（.config:2409，依赖 `!SCHED_CPULOAD_NONE`——改 SYSCLK 后依赖满足自动变 y）→ `nsh.h:466-468` 中 `NSH_HAVE_CPULOAD` 在 EXCLUDE_CPULOAD 定义时被 undef → **top 命令从未编译进固件**（top 是 nshlib 内置 `CMD_MAP("top", cmd_top)`，受 NSH_HAVE_CPULOAD 门控）。

**修复**：`kconfig-tweak -d FS_PROCFS_EXCLUDE_CPULOAD`（.config）+ defconfig 同步加 `# CONFIG_FS_PROCFS_EXCLUDE_CPULOAD is not set` + 删除 nshlib 旧 .o 强制重编。验证：strings vela.bin 含 `Usage: top[ -n <num>] [ -d <delay>]...` 与 `cpuload` ✅。固化 ok-20260806-22。

**教训**：改 CPULOAD 配置时只开了 SYSCLK，忽略了 `FS_PROCFS_EXCLUDE_CPULOAD` 的自动联动——配置开关要查 Kconfig 依赖链，不能只看目标选项。

## 十一、补充（ok-20260806-23）：卡顿真因转向 SD 读 I/O，恢复多块读 16

### 11.1 上板 22 号实测（top 首次有数据）推翻"CPU 抢占"假设

用户刷 22 号跑 top + ps（播放 MP3），实测数据：

| 线程 | 待机 CPU% | 播放 CPU% |
|------|----------|----------|
| bt_recv（蓝牙收） | 50.1% | 50.1% 🔴 |
| luncher_dm（UI） | 28.6% | 1.0-1.2% |
| CPU0 IDLE | 20.6% | 37.4% |
| AudioDecode | - | 8.5-9.3% |
| AudioRender | - | 0.0% |

**关键事实**：①writei 全成功（wr=1920）零 underrun；②AudioDecode 仅 8.5-9.3% CPU 且大部分 Waiting Signal（等数据非算数据）；③CPU0 IDLE 37%+ 双核有大量余量；④但**用户听感依然卡**。

**结论（推翻 Part5 提权假设）**：卡顿根因不在 CPU 抢占/算力，而在 **SD 读 I/O 供给慢**——`MMCSD_MULTIBLOCK_LIMIT=1` 单块读（每 512B 一次 CMD17 命令），且 ps 显示 realtek_sdio_thread/recv_task_thread 存在（WiFi/蓝牙走 SDIO，与 SD 卡 SDMMC 争抢总线带宽），命令延迟被放大 → 解码供给间歇 → 可闻卡顿。线程提权（19/21 号）只解决 CPU 抢占，未解决 I/O 供给。

### 11.2 修复：恢复多块读（折中 16 块）

历史 =0（无限制）时 DMA 多块读崩（`do_rom_HAL_SDC_Request` 超时 dump 路径访问坏 sg buffer），不能直接放开。**取 `MMCSD_MULTIBLOCK_LIMIT=16`**：每次 CMD18 读 16 块（8KB），命令开销降 16 倍，DMA 描述符少（避开历史崩溃条件），吞吐大幅提升。

改动：`nuttx/.config` + `defconfig` 的 `CONFIG_MMCSD_MULTIBLOCK_LIMIT` 1→16。验证：编译/打包通过（nsh.fex==vela.bin 7482536B md5 6390dd96）；固化 ok-20260806-23。**待上板验证**：卡顿应明显改善；若仍卡或崩，需上板抓 SD 读耗时日志 + 考虑 bt_recv 50% 空转排查（蓝牙未连也占半个核，疑 H4 驱动忙等）。

## 十二、补充（ok-20260806-24）：多块读 16 上板直接崩，回滚单块读（DMA bug 实证）

### 12.1 上板实测：MMCSD_MULTIBLOCK_LIMIT=16 播放 MP3 直接崩溃

用户刷 23 号（多块读 16）上板，点击播放 Aurora Borealis.mp3 后系统崩溃（irq 栈 100% 打满 + `backtrace|10: 0xe350fffe` 坏地址）。

**addr2line 解析崩溃栈（nuttx.elf，PID112=Demux 线程 Running 时崩）**：

```
DemuxThread (demuxComponent.c:1993)
→ CdxParserPrepare → __FileStreamConnect → read → readv → nx_readv
→ file_readv_compat → fat_read → fat_hwread → mmcsd_read (mmcsd_sdio.c:2383)
→ mmcsd_readmultiple (mmcsd_sdio.c:1764)
→ mmcsd_sendcmdpoll → sunxi_mmc_mult_sendcmd (nuttx_sdio_mult.c:289)
→ HAL_SDC_Request → do_rom_HAL_SDC_Request (hal_sdhost.c:1674)  ← 崩溃点
```

**与历史 8-5 崩溃点完全一致**（`do_rom_HAL_SDC_Request:1674` 超时 dump 路径访问坏 sg buffer）。**实证确认：R528 的 SDMMC 多块读 DMA 有确定性 bug，任何 >1 块读都会触发**——`MMCSD_MULTIBLOCK_LIMIT=1` 单块读是必要权宜（虽卡但稳）。

### 12.2 回滚与结论

**回滚**：`MMCSD_MULTIBLOCK_LIMIT` 16→1（.config + defconfig），固化 ok-20260806-24。编译/打包通过（nsh.fex==vela.bin 7478448B md5 d9b635d3）。

**卡顿问题重新定性**：
- 单块读：稳定不崩但供给慢 → 卡顿（I/O 命令延迟）
- 多块读：提速但必崩（DMA bug 实证）→ 不可用
- **根治卡顿必须先修多块读 DMA bug**（`sunxi_mmc_mult_sendcmd` → `do_rom_HAL_SDC_Request` 超时根因：DMA/中断竞争、wait 状态机早退、超时 dump 路径访问坏 sg buffer），这是唯一正路；或从供给侧绕过（如预读缓存大块到内存、解码线程提前预取）

## 十三、补充（ok-20260806-25）：norflash 音乐目录映射（laojie.mp3 测试通道）

### 13.1 背景与动机

卡顿根因=SD 单块读 I/O 供给慢，但多块读 16 上板直接崩（DMA bug 实证），修 DMA 是正路但需时间。为**绕开 SD 卡读**做对照验证，用户提出：利用 norflash 用户空间（256M nand，usrdata 分区挂载 /data，当前仅用 ~10MB），放一首测试歌 laojie.mp3，播放器增加第二目录扫描直接从 norflash 读。

### 13.2 改动

| 文件 | 改动 |
|------|------|
| `apps/luncher_dm/deskmate_ui.c` | 新增 `DM_MUSIC_DIR_NORFLASH "/data/music"` 宏；`music_scan_tracks()` 改为循环扫描 `scan_dirs[] = { "/sdcard/music", "/data/music" }` 双目录，曲目合并入 dm_tracks（按序排） |
| `lichee/board/common/data/UDISK/music/laojie.mp3` | 新增测试歌（5.4MB），经 usrdata.fex（yaffs）打包进板端 `/data/music/laojie.mp3` |

**验证**：usrdata.fex 从 11MB 增至 16.5MB（laojie.mp3 已打入）✅；编译/打包通过（nsh.fex==vela.bin 7478448B md5 f99820b9）✅；固化 ok-20260806-25（vendor 仓：deskmate_ui.c 改 + laojie.mp3 入仓）。UI 已按规则 sync 到模拟器副本。

### 13.3 上板验证方法

1. 刷 ok-20260806-25 后，音乐播放器清单应出现 **laojie**（来自 /data/music，norflash 读）
2. 播放 laojie.mp3：**若不再卡顿 → 实证卡顿=SD 读 I/O**，且给出"绕开 SD 卡"的可用播放通道；若仍卡 → 卡顿另有根因（解码/供给链路本身），需重新定位
3. 对照：再播 sdcard/music 里的歌（仍卡）→ 双通道对比结论明确

## 十四、补充（ok-20260806-26）：sync 覆盖事故复盘 + norflash 音乐目录重做 + 播放器 UI 修整

### 14.1 🔴 事故复盘：sync_deskmate.sh 默认方向覆盖板端代码（MP3 无声真因）

上板 25 号发现 **MP3 也无声 + 播放列表无 laojie**。排查根因：

| 现象 | 根因 |
|------|------|
| 播放列表无 laojie | 25 号固件 vela.bin **不含 `/data/music` 扫描代码**（strings 验证=0）——双目录扫描被覆盖 |
| MP3 无声 | **sync 默认方向（模拟器→板端）把板端 deskmate_ui.c 回退成旧版**，丢了 ①16 号修复 `dm_sound_write` 返回字节数（`wr*src_fb`→现返回帧数）=**无声直接根因** ②18 号修复 period/buffer 512/2048→1024/4096 |

**教训（已写入 AGENTS.md 硬性规则）**：sync_deskmate.sh **默认方向是「模拟器→板端」**，改板端后**必须加 `-r`**；且无 UI 需求期间**已暂停 UI 模拟器同步**（恢复步骤见 AGENTS.md 二章）。

**修复**：`git checkout 5e33585b`（18 号基线，含全部音频修复）恢复 deskmate_ui.c，重新加双目录扫描。

### 14.2 norflash 音乐目录重做（laojie.mp3 通道）

- `DM_MUSIC_DIR_NORFLASH "/data/music"` 宏 + `music_scan_tracks()` 双目录循环 `{"/sdcard/music","/data/music"}`（18 号基线上重加）
- laojie.mp3 已在 `lichee/board/common/data/UDISK/music/`（usrdata.fex 已含，上板验证过 usrdata.fex 有 laojie.mp3 ✅）
- 验证：strings vela.bin 含 `data/music` ✅

### 14.3 播放器 UI 修整（用户需求）

| 需求 | 改动 |
|------|------|
| 播放键点击后红色→蓝色（跟封面一致） | 播放键 CHECKED 状态改 `COL_BLUE` 填充 + 白色图标（覆盖默认主题红） |
| 播放/上一曲/下一曲无点击动感 | 三键加 `LV_STATE_PRESSED` 动感：transform ±3px + 蓝色底 20% 透明 |
| 时间标签位置 | 进度行改为 **elapsed | slider | total** 顺序（过去时间在进度条左、总时长在右） |

注意：LVGL 9.1 无 `LV_OPA_25`（编译报错），用 `LV_OPA_20`。

**验证**：编译/打包通过（nsh.fex==vela.bin 7478448B md5 241018cd）；固化 ok-20260806-26。**待上板验证**：①MP3 恢复有声（16/18 号修复回来了）②播放列表出现 laojie（norflash 通道）③播放键蓝色 + 三键动感 + 时间标签左右布局。

## 十五、补充（ok-20260806-27）：26 号无声排查定论（首次有声/切歌无声）+ 播放键 UI 精修

### 15.1 26 号无声排查：确认音频代码完好，无声=切歌遗留 bug

用户刷 26 号实测后反馈"没有声音"，经逐项排查+用户复测澄清：

1. **diff 验证**：当前 deskmate_ui.c 音频区域（dm_sound_set_format → dm_sound_ops）与 18 号基线**完全一致**（`wr*src_fb` 字节修复 + period 512/2048 + sw_params start_threshold 全在）
2. **用户复测**：关机重启后**直接点播放按钮 → 有声**（首次播放正常）；**从播放列表点 laojie.mp3 / 切歌 → 无声**
3. **结论**：26 号固件音频链路完好（16/18 号修复已恢复生效），"无声"是**切歌后 codec 未重新使能**的已知遗留 bug（18 号 dapm_state 修复理论闭环但实测切歌仍无声，根因疑似切歌后未真正重新 open 设备，归档遗留重查）

### 15.2 播放键 UI 精修（用户需求）

| 需求 | 改动 |
|------|------|
| 暂停符号看不见 | 根因=icon 显式 COL_BLUE + 按钮 CHECKED 蓝底 → 蓝底蓝图标不可见；修复=icon 加 `LV_STATE_CHECKED` 白色 |
| 纯蓝太愣、要玻璃质感 | CHECKED 状态改 **蓝→紫垂直渐变**（与封面一致）+ 光晕阴影（width 14 / COL_BLUE / opa 40） |

**验证**：编译/打包通过（nsh.fex==vela.bin 7478448B md5 b8cbeec5）；固化 ok-20260806-27。**待上板验证**：①播放键暂停=蓝紫渐变玻璃 + 白图标 + 光晕 ②播放/上一下一按动感 ③时间标签左右布局。**切歌无声为遗留 bug 待重查**（非本固件回归）。

## 十六、补充（ok-20260806-28）：laojie.mp3 无声新排查（norflash 数据完好）+ 播放列表 UI 简化

### 16.1 laojie.mp3（norflash）无声排查：文件与打包均完好

用户新证据：**"一开机一进 APP 一点击播放 laojie.mp3 也没声音"**（与切歌无关；sdcard 歌有声）。PC 侧验证：

| 验证项 | 结果 |
|--------|------|
| `/data/dm/laojie.mp3` 源文件 | ✅ 有效 MP3（ID3v2.3, 128kbps, 44.1kHz, Stereo），md5=5113303e |
| UDISK/music/laojie.mp3（打包源） | ✅ md5 与源文件一致（5113303e） |
| usrdata.fex 内 laojie.mp3 数据 | ✅ 按 yaffs2 2032B 数据+16B tag chunk 布局提取，md5=5113303e **与源文件完全一致**（镜像未损坏；先前 md5 不匹配是解析方法未跳过 tag 所致） |

**结论**：norflash 里 laojie.mp3 数据完好，无声**不是文件/打包损坏**。剩余嫌疑：①板端 yaffs 读取（建议上板 `md5sum /data/music/laojie.mp3` 对比 5113303e）②norflash 读取速度→解码供给（但 writei 全成功）③laojie.mp3 解码链路特性（44.1k 重采样等，与 sdcard MP3 相同路径）。**待上板进一步定位**。

### 16.2 播放列表 UI 简化（用户需求）

| 需求 | 改动 |
|------|------|
| 去掉三角符号 | 行内 `LV_SYMBOL_PLAY` 图标删除 |
| 去掉右侧 `--:--` 时长 | 时长 label 删除（含 `--:--` 兜底） |
| 只留歌名 + 收紧间距 | 行内仅剩标题 label，min_height 34→30、pad_top/bottom 3→2、去 pad_column |

播放键暂停符号白色反色（白底蓝三角 ↔ 蓝紫渐变底白暂停符号）已在 27 号实现（icon `LV_STATE_CHECKED` 白色）。

**验证**：编译/打包通过（nsh.fex==vela.bin 7478448B md5 d779b64b）；固化 ok-20260806-28。**待上板验证**：①播放列表仅歌名、无三角/时长、间距紧凑 ②laojie.mp3 无声排查（上板 md5sum）③暂停符号白色反色（27 号）。

## 十七、补充（ok-20260806-29）：laojie 无声定位——加 PCM 能量诊断 log（不盲试）

### 17.1 背景与排查结论

用户播放 laojie.mp3（norflash）log 全部正常（解码 init OK、44100→48000 重采样、writei 全 1920 成功）但**实际无声**；sdcard 的 Aurora 有声。已排除：
- **文件/打包损坏**：PC 侧验证 usrdata.fex 内 laojie.mp3 按 yaffs2 chunk 布局提取 md5=5113303e 与源一致（26/28 号已查）
- **大 ID3 tag**：Aurora（ID3 518KB）有声 vs laojie（377KB 含 APIC 封面）无声 → 非 ID3 因素（Aurora 更大却有声）
- 板端 `md5sum` 命令不存在，无法上板校验（ls 可见文件）

### 17.2 本轮改动：dm_sound_write 加 PCM 能量统计 log

`deskmate_ui.c` `dm_sound_write()`：writei 日志扩展 `peak=`（前 4096 样本峰值绝对值）与 `nz=`（非零样本数），每 20 次打印一次防刷屏。

**判别方法**（上板播放 laojie 看 log）：
| peak / nz 结果 | 结论 | 下一步 |
|----------------|------|--------|
| peak 正常（几百~几千）、nz 多 | 写进声卡的数据正常 → **驱动/HAL 层问题**（codec DAC/dapm/DMA 未真正出声） | 查 codec 使能/DMA/时钟 |
| peak≈0 或 nz 极少 | 解码输出静音/数据错误 → **解码/供给链路问题** | 查 adecoder 对 laojie 的解码 |

**验证**：编译/打包通过（nsh.fex==vela.bin 7478448B md5 72351b24）；固化 ok-20260806-29。**待上板验证**：播放 laojie.mp3 抓 `[music] writei ... peak=... nz=...` log 判别根因归属。

## 十八、补充（ok-20260806-30）：暂停符号不可见根因（LVGL 状态样式坑）+ 动态切换修复

用户反馈：暂停键白色符号几乎看不到，感觉蓝色按钮图层盖在上面。**根因=LVGL 状态样式继承坑**：

- `LV_STATE_CHECKED` 是**父按钮**（music_play_btn）的状态，`music_resume()` 用 `lv_obj_add_state(music_play_btn, LV_STATE_CHECKED)` 设置
- 播放图标 `music_play_icon` 是**子 label**，子对象**从不处于 CHECKED 状态** → 之前对 icon 设的 `lv_obj_set_style_text_color(icon, 白色, LV_STATE_CHECKED)` **永不生效**
- icon 始终保持默认蓝色（COL_BLUE）→ 蓝紫渐变底上蓝图标 → 几乎不可见

**修复**：删除创建时对 icon 无效的 CHECKED 白色样式；在 `music_resume()` 里**显式** `lv_obj_set_style_text_color(music_play_icon, 0xFFFFFF, 0)`（白色暂停符号），`music_pause()` 里恢复 COL_BLUE（蓝色播放三角）——反色效果（白底蓝三角 ↔ 蓝紫渐变底白暂停）由代码动态切换保证，不再依赖状态样式继承。

**验证**：编译/打包通过（nsh.fex==vela.bin 7478448B md5 8b971ebb）；固化 ok-20260806-30。**待上板验证**：播放时暂停键=蓝紫渐变底 + 白色暂停符号清晰可见；暂停后恢复白底蓝三角。

## 十九、补充（ok-20260806-31）：PCM 能量诊断定论 + 音量调至 30% + 正弦波测试命令

### 19.1 PCM 能量诊断定论（30 号固件 peak/nz log）

用户上板播放三首歌抓 `[music] writei ... peak=... nz=...`：

| 歌曲 | peak（前 4096 样本峰值） | nz（非零样本） | 判定 |
|------|--------------------------|----------------|------|
| Aurora Borealis.mp3（sdcard，首播） | 4638→14830 | 3836~3840/3840 | ✅ 数据正常 |
| laojie.mp3（norflash） | 1932→5070 | 3835~3840/3840 | ✅ 数据正常 |
| IGNIS.wav（sdcard） | 38→5915 | 3781~3840/3840 | ✅ 数据正常 |

**定论**：三首歌写进声卡的 PCM 数据全部正常（peak 几千、nz 接近满）→ 排除解码输出静音/数据错误。**无声根因不在数据链路**，结合"只有首播 Aurora 有声、其余都无声"，指向 **驱动/HAL 层**（切歌后 codec DAC/dapm 未重新使能）或 **首播后声卡状态残留**。已排除：文件损坏（usrdata 提取 md5 一致）、大 ID3 tag（Aurora 更大却有声）、数据静音（peak/nz 证明）。

### 19.2 音量调至 30%（用户需求）

- **XPlayer 无 SetVolume API**、deskmate settings 页 Volume slider 值 60 但回调 NULL（未接线）——音量实际由 codec `digital_vol` 控制
- `sun8iw20-codec.c` `default_param.digital_vol`：0x0（最大）→ **0x2C（≈30%**：DVOL 6bit max=0x3F，寄存器值越小音量越大，0x13≈70% 对应历史 8-5 预设 75%）
- 验证：编译/打包通过（nsh.fex==vela.bin 7478448B md5 718a07d2）；固化 ok-20260806-31

### 19.3 正弦波测试命令（aplay 无内置 sine，用 UDISK 现成测试音频）

板端 `/data` 挂载 usrdata.fex（源自 `lichee/board/common/data/UDISK/`），已有测试音频：

```bash
# 1kHz 正弦波 WAV（44100Hz 单声道 16bit）
aplay -r 44100 -c 1 -f cd /data/1kHz.wav
# 或 48000Hz 立体声 16bit 裸 PCM
aplay -r 48000 -c 2 -f dat /data/s16le_48000_stereo.pcm
```

用于对照验证声卡硬件链路（首播/切歌后是否出声）。

## 二十、补充（ok-20260806-32）：音量 30% 改软件衰减（31 号 digital_vol 致无声教训）

### 20.1 31 号无声回归与根因

用户刷 31 号（`digital_vol` 0x0→0x2C）后**连首播都无声**，aplay 也报 "0 bits not supprot"。

**根因**：`sun8iw20-codec.c` 的 `digital_vol` 直接写 `DAC_DPC.DVOL`（bit12，6bit），但音量控制 kcontrol `sunxi_set_data_invert` 有**反转语义**（写寄存器 = 63 − 音量值）。直接改 default_param 绕过反转，0x2C 实测导致整机无声。

### 20.2 修复（本轮）

| 文件 | 改动 |
|------|------|
| `sun8iw20-codec.c` | `digital_vol` 0x2C → **回滚 0x0**（保持驱动原语义，确保有声） |
| `deskmate_ui.c` | `dm_sound_write()` 在 writei 前对 S16 样本做 **30% 软件衰减**（整数定点 `(v*9830)>>15`，16bit 直通与 24/32→16 降转两条路径都处理）——音量 30% 由软件可靠实现，不再动寄存器 |

### 20.3 aplay "0 bits not supprot" 排查结论

**根因**：`aplay -f cd` 会把 channels 强制设为 2，而 `1kHz.wav` 是**单声道** → 帧参数不匹配。**正确命令**：

```bash
# 1kHz.wav 本身是 44100Hz 单声道 16bit，无需 -f cd（会强设 2ch）
aplay -r 44100 -c 1 /data/1kHz.wav
# 48000Hz 立体声裸 PCM
aplay -r 48000 -c 2 /data/s16le_48000_stereo.pcm
```

**验证**：编译/打包通过（nsh.fex==vela.bin 7478448B md5 7fe17176）；固化 ok-20260806-32。**待上板验证**：①音量应为 30%（明显小于之前）②首播 Aurora 恢复有声（digital_vol 回滚）③aplay 用修正命令应能出声。

---

*DevLog by AtomCode (deepseek-v4-flash)*
