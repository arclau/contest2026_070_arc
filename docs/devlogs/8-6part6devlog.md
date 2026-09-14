# DevLog 2026-08-06 (Part6) — 切歌无声根因重大突破：-16 EBUSY（声卡设备未释放）

> 工程：openvela (R528) — nsh 配置
> 工作目录：`/data/dm`
> 工具链：SDK 自带 prebuilts/gcc/linux-x86_64/arm-none-eabi (GCC 13.4.0)
> 打包流程：`lichee` 下 `source envsetup.sh && lunch_nuttx 2 && pack`，镜像输出至 `lichee/out/r528s3/gemini-s1_nand/`
> **AI 交接：新会话请先读 `AGENTS.md`，本文件是当日归档，仅按需查阅。**
> 前置 Part：Part1~5（`8-6part1devlog.md`~`8-6part5devlog.md`），当前基线 tag **ok-20260806-32**

---

## 一、任务背景

Part5 结束（ok-20260806-32）时遗留：**切歌后无声**（首播有声、切歌/换曲后静音），PCM 能量诊断（peak/nz log）证明三首歌写进声卡的数据全部正常 → 根因指向驱动/HAL 层。音量 30% 已改软件衰减（`dm_sound_write` 内 `(v*9830)>>15`，codec digital_vol 直接改会绕过反转语义致无声——31 号教训，已回滚 0x0）。

本 part 用户上板测得**决定性证据**，切歌无声根因基本锁定。

## 二、🔴 决定性证据：第二次 aplay 报 -16 (EBUSY)，声卡设备被占用未释放

用户上板操作序列（ok-20260806-32 固件）：

| 步骤 | 结果 |
|------|------|
| 1. `aplay -r 44100 -c 1 /data/1kHz.wav`（首次） | ✅ **正常**：RIFF/WAVE 解析 OK、open OK、播放完成、363 snd_vela_pcm_close |
| 2. 进音乐播放器播 Aurora Borealis.mp3（首播） | ✅ **有声**：解码 OK、writei 全 1920、peak 正常 |
| 3. 切歌 → Beautiful Love.mp3 / IGNIS.wav | ❌ **无声**：writei 成功（peak 正常）但听不到声音 |
| 4. 退出播放器后再 `aplay /data/1kHz.wav` | ❌ **`snd_vela_pcm_open_config failed (return: -16)`** + `audio open error:-16` |

**-16 = EBUSY（设备忙）**：切歌后声卡设备**没有真正释放**，残留被占用 → 后续任何进程再 open 声卡都失败。

## 三、根因分析（切歌后 sound device 未真正 close）

**关键线索链**：
- 首播/首次 aplay 一切正常 → 声卡硬件链路、驱动初始化、播放路径全 OK
- 切歌（XPlayerReset）→ 无声 → 再 open 报 EBUSY
- **结论：XPlayerReset 切歌时，声卡设备未走完整 close 释放路径**，或 close 了但硬件占用未释放 → 新播放 open 到同一设备时状态残留（codec DAC 未重新使能 / substream 未复位）→ writei 成功但无声

**嫌疑点（下一会话深挖）**：
1. `snd_vela_pcm_close`（pcm.c:359）：`if (pcm->setup && !pcm->donot_close)` 才 drop+hw_free——**`pcm->donot_close` 是否被置位**导致 close 跳过真正释放？（grep 全库仅 pcm.c:366 一处读取，赋值处待查）
2. `TinaSoundDeviceStop → closeSoundDevice → snd_pcm_close` 链路是否每次切歌都走到（用户日志切歌有 `363, snd_vela_pcm_close`，但 EBUSY 说明硬件层没释放干净）
3. `snd_pcm_open_config` 返回 -16 的具体来源：设备节点占用检查/引用计数/上次未 drop
4. 与 18 号 dapm_state 修复的关系：dapm_state 只是 codec 使能状态，若设备根本未重新 open（EBUSY），dapm_state 无从谈起——**18 号方向可能错，真因是 close 不彻底**

## 四、已验证事实（避免重复排查）

| 项 | 结论 |
|----|------|
| 首播/首次 aplay | ✅ 正常出声 |
| 切歌后 | ❌ 无声（writei 成功但听不到） |
| 切歌后再 aplay | ❌ **-16 EBUSY**（声卡被占用） |
| PCM 数据（三首歌） | ✅ 全部正常（peak 几千、nz≈满）→ 非解码/数据问题 |
| laojie.mp3 文件/打包 | ✅ 完好（usrdata.fex 提取 md5=5113303e 与源一致） |
| 大 ID3 tag | ❌ 非根因（Aurora 518KB 有声 vs laojie 377KB 无声） |
| 音量 | 30% 软件衰减已实现（32 号）；codec digital_vol 勿直接改（反转语义） |

## 五、当前固件基线

- **ok-20260806-32**（最新稳点）：音量 30% 软件衰减 + 双目录扫描 + 播放器 UI 精修
- 镜像：`lichee/out/r528s3/gemini-s1_nand/rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img`
- 三仓状态：vendor 仓有未提交改动需检查固化；nuttx/apps 干净

## 六、下一步（新会话）

1. **🔴 深挖 -16 EBUSY 根因**（最高优先）：切歌 close 后声卡设备未释放 →
   - 查 `pcm->donot_close` 赋值处（全库仅 pcm.c:366 读取）
   - 查 `snd_pcm_open_config` 返回 -16 的检查逻辑（设备占用/引用计数）
   - 查 `snd_vela_pcm_close → ops->close → vela_pcm_close` 完整释放链（drop/hw_free/mutex）
   - 复现路径：aplay 一次 → aplay 第二次（应复现 -16）→ 定位占用残留
2. 修复方向：切歌/停止时确保声卡设备完整释放（或 open 时容忍残留状态强制复位）
3. 修好 EBUSY 后：切歌无声大概率随之解决（能重新 open → codec 重新使能）
4. 遗留不变：卡顿（SDMMC 多块读 DMA bug）、bt_recv 50% 空转、UI 模拟器同步暂停中（恢复见 AGENTS.md 二章）

---

*DevLog by AtomCode (deepseek-v4-flash)*
