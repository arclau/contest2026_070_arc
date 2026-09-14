# DevLog 2026-08-07 Part3 — 切歌无声决战：数字层铁证排除 → 歌曲级 → XPlayerReset → 最后收敛

> 工程：openvela (R528) — nsh 配置
> 工作目录：`/data/dm`
> 前置：`8-7devlog.md`（Part1：音量链路→死机→codec 深挖→字体→解码审查）、`8-7part2devlog.md`（Part2：歌曲级差异报告，供 ChatGPT 分析）
> 本文记录 **ok-20260807-11 → ok-20260807-19** 共 9 个固化点的排查，含三次方向修正（歌曲级→XPlayerReset→声卡第二次 open）

---

## 一、任务背景（Part3 起点）

Part2 结束时结论：**Aurora 有声，Beautiful Love/laojie 无声，与位置/操作无关** → 怀疑歌曲文件。但用户后续测试推翻该结论，最终锁定：**与歌曲无关，纯切歌问题**。本文按时间线记录全部证据与排除过程。

---

## 二、三次方向修正（核心脉络）

| 阶段 | 假设 | 证据 | 结论 |
|------|------|------|------|
| 1 | 歌曲文件差异（Beautiful Love/laojie 无声） | corr 0.06~0.8、raw_rms 满幅、laojie.mp3 文件解析正常 | ❌ 排除：用户换 IGNIS.wav 首播再切 Aurora 依然无声 |
| 2 | XPlayerReset 切歌复用路径残留 | create 路径有声、XPlayerReset 路径无声 | ❌ 排除：destroy&recreate（全新 XPlayer+全新 dm_sound）仍无声 |
| 3 | **声卡设备第二次 open 状态残留** | destroy&recreate 等价首播却无声；数字层 100% 一致 | 🎯 **当前唯一剩项**（待万用表 AC 档测 LINEOUT 二分） |

---

## 三、诊断打点体系（本轮全部代码改动）

### 3.1 ok-20260807-11：三闭环证据点（deskmate_ui.c + sun8iw20-codec.c）

| 证据点 | 改动 | 目的 |
|--------|------|------|
| ① writei 返回值 | `[music] writei ERR wr=%d state=%d`（非 -EPIPE 不再静默）+ 正常日志加 state | 抓"数据没进声卡"（EBUSY/-EBADFD 曾被 `if(wr<0) return 0` 吞掉） |
| ② DAC 消费状态 | out_dump 加 `FIFOS(TXE)` + `CNT(delta)`（DAC_CNT 只读 TX 帧计数） | **判断数据是否真进 DAC**（决定性） |
| ③ trigger 命令名 | out_dump tag：START/STOP/PAUSE_PUSH/PAUSE_RELEASE | 暂停续播 vs 切歌序列可对比 |

**上板结果**：writei 全成功（wr=1920 state=3 零 ERR）→ EBUSY/-EBADFD 方向排除；**DAC_CNT 满速增长**（每段 delta≈21 万帧≈4.5s@48k，切歌后同样）→ **数据确实进 DAC 数字域**；暂停 vs 切歌寄存器逐位一致。

### 3.2 ok-20260807-12：强制 mono 混音实验（DM_FORCE_MONO=1）

```c
#define DM_FORCE_MONO 1
/* writei 前：L/R 平均 → dual mono，统计在 mono 前完成（corr 仍反映原始 stereo） */
m = (int16_t)(((int)l + (int)r) / 2);
```

**目的**：验证"外部功放单端合并处反相抵消"假设。
**结果**：mono 混音后无声歌曲依然无声 → **反相抵消假设 100% 排除**（mono 后 L=R 完全同相，不可能再抵消）。注：DM_FORCE_MONO=1 一直延续到 ok-20260807-19，用户全程测的都是 mono 混音数据。

### 3.3 ok-20260807-13：corr/RMS 统计 + ProcessBalance 注释

- dm_sound_write 加 `raw_rms`（衰减前）+ `L_rms/R_rms`（分声道能量）+ `corr`（皮尔逊相关，+1 同相 / -1 反相）
- `audioRenderComponent.c` `ProcessBalance` 调用注释（`#if 0`）——核查后该函数本是 no-op（nConfigOutputBalance 由 calloc 清零=0，函数内 `nOutBalance==0` 直接 return）

**上板结果**（用户提供的完整 corr 数据）：
- Aurora：corr=0.702→0.931→0.878（正常音乐范围）
- Beautiful Love：corr 波动 0.065~0.813，**无稳定 -0.95** → 反相排除
- **数据满幅**：Beautiful Love raw_peak=5984/5666、raw_rms=1972/1790，与 Aurora 同量级 → 静音排除

### 3.4 ok-20260807-14：WRITE_GAP 单包间隔检测

```c
/* 每次 writei 前记录 tick，间隔 >60ms 打印（正常 40ms/包） */
if (gap > 60) printf("[music] WRITE_GAP %u ms\n", gap);
```

### 3.5 ok-20260807-15/16：XRUN 修复（buffer 2048→8192）

**关键日志发现**（15 版）：`status->state:4`（=SND_VELA_PCM_STATE_XRUN）反复出现 + 周期性 `[codec] STOP`（CNT≈211968~215040 ≈ 4.4s@48k 就停）+ `WRITE_GAP 71~2036ms`。

**根因链（代码级确认）**：
```
writei 偶发停顿（WRITE_GAP 71~2036ms：解码器/线程调度/重采样抖动）
        ↓
buffer 仅 2048 帧 = 42.7ms → 停顿超过余量 → ringbuffer 耗尽
        ↓
snd_pcm.c:678 avail >= stop_threshold → xrun() → STOP（state=4）
        ↓
dm_sound_write 收到 -EPIPE → prepare → 重新 START
        ↓
每 4.4 秒循环
```

**修复**（16 版）：dm_sound_open buffer 2048→8192（170ms）、period 512→1024。
**结果**：✅ **XRUN 周期性重启消除**——Beautiful Love 连续播放 13.27 秒（CNT=637080）无一次 STOP。**但无声依旧** → 证明 XRUN 不是无声根因（只是断续）。

### 3.6 ok-20260807-17：XPlayerReset → XPlayerDestroy+重建（用户关键反馈触发）

**用户决定性反馈**："Beautiful Love 第一首/点击播放都有声，切歌无声" → 歌曲因素排除，问题锁定切歌路径。

**代码分析**（create vs reset 调用链）：
| 场景 | XPlayer 状态 | 路径 | 结果 |
|------|-------------|------|------|
| Beautiful Love 第一首自动播 | 无（首次） | `XPlayerCreate`+`dm_sound_create()` | ✅ 有声 |
| 进播放器点歌单 | 无（未创建） | create 路径 | ✅ 有声 |
| Aurora 播放中切歌 | 已存在 | `XPlayerReset`（PlayerClear 销毁 audioRender 重建，SoundCtrl 复用） | ❌ 无声 |

**修复**：切歌分支 `XPlayerReset` → `XPlayerDestroy` + 重建（走已验证有声的 create 路径）。
**结果**：❌ **未命中**——destroy&recreate 后切 Aurora 依然无声（数据满幅 raw_peak=14746、CNT=694472）→ **XPlayer 生命周期彻底排除**。

### 3.7 ok-20260807-18：增益寄存器 dump（最后代码盲区）

**新假设**：切歌路径可能改写增益/静音类寄存器（此前从未 dump）：
- `DAC_VOL_CTL(0x04)` 音量、`DAC_DG(0x28)` 数字增益、`DAC_DAP_CTL(0xF0)`、`DAC_DRC_CTRL(0x108)` 动态范围压缩

**上板结果**：首播 vs 切歌 **逐位一致**（VOL=0x1a0a0 DG=0x0 DAP=0x0 DRC=0x80）→ 增益假设排除。

### 3.8 ok-20260807-19：dm_sound 对象泄漏修复

**代码发现**：`XPlayerDestroy` → `PlayerClear` → `AudioRenderCompDestroy` 只 free audioRender 组件，**不释放我们传入的 SoundCtrl（dm_sound）** → 每次切歌重建都新建 dm_sound 对象、旧对象从不释放 → 逐次泄漏。
**修复**：加 `g_music_sound` 全局引用，切歌重建时 `g_music_sound->ops->destroy(g_music_sound)` 显式释放。

---

## 四、aplay EBUSY 实验（重要解读纠正）

**实验**：切歌无声状态后 `aplay /sdcard/music/IGNIS.wav`（绝对路径）→ 报 `-16 EBUSY`。
**纠正**：此结果 **不能证明"切歌后未释放"**——测试时播放器还在运行（切歌后新歌仍在"播放"），单用户声卡被播放器占用，aplay 打不开是预期行为。

---

## 五、最终排除清单（截至 ok-20260807-19）

| 层 | 证据 | 结论 |
|----|------|------|
| 歌曲/文件 | 换 IGNIS.wav 首播再切 Aurora 依然无声；laojie.mp3 帧解析正常 | ❌ |
| 暂停操作 | 不按暂停直接切歌也无声 | ❌ |
| XPlayerReset 复用 | destroy&recreate 全新创建也无声 | ❌ |
| dm_sound handle 残留 | stop/reset 均 close+置 NULL，destroy 也 close | ❌ |
| dm_sound 对象泄漏 | 已修复（ok-20260807-19） | ❌（独立问题） |
| 数据静音/反相 | raw_peak 14746 满幅、CNT 增长、corr 正常无 -0.95 | ❌ |
| 反相抵消 | mono 混音（DM_FORCE_MONO=1）后仍无声 | ❌ |
| codec 全部寄存器 | DPC/DAC_ANA/HP_ANA/DAC_REG/FIFOC/FIFOS/CNT/PA/VOL/DG/DAP/DRC 逐位一致 | ❌ |
| DMA 描述符/cyclic | prep_cyclic 每次重建 desc、cyclic=true 重设、旧 desc 正确释放 | ❌ |
| XRUN/underrun | buffer 8192 后 13 秒无中断（已修复断续，但非无声根因） | ❌（独立问题） |
| **DAC 模拟输出级第二次使用** | destroy&recreate 等价首播却无声，数字层 100% 一致 | 🎯 **唯一剩项** |

---

## 六、当前状态 / 下一步（唯一焦点）

**🔴 切歌无声，数字链路 100% 排除（数据满幅进 DAC + CNT 增长 + 全寄存器一致 + PA=1 + mono 混音 + destroy&recreate），唯一剩解释：DAC 模拟输出级（LINEOUT 引脚）在"声卡第二次使用"时实际无信号——寄存器显示使能但模拟输出可能为 0（偏置未建立/输出级时序）。**

**待验证（不用示波器，万用表 AC 档）**：
1. **首播（有声）**：测 LINEOUT 引脚 AC 电压（应有几百 mV 波动）
2. **切歌（无声）**：测 LINEOUT 引脚 AC 电压
   - 首播有 AC、切歌无 AC → **DAC 模拟输出级第二次使用未建立** → 查 `sunxi_codec_playback_lineout_route` 第二次 on 的模拟偏置时序（RAMP/RDEN）与 dapm off/on 循环
   - 两者都有 AC 但喇叭无声 → 功放/喇叭物理段（查 GPIOD17 电平 1.5-1.7V 疑点：低于 3.3V 逻辑高，若 1.8V bank 则正常）

---

## 七、固化点与产物汇总

| 固化点 | md5 | 改动 |
|--------|-----|------|
| ok-20260807-11 | 6ecf0bdf | deskmate_ui.c（writei ERR+state）+ sun8iw20-codec.c（FIFOS/CNT/命令名） |
| ok-20260807-12 | 3c634bc3 | DM_FORCE_MONO=1 + corr 统计 |
| ok-20260807-13 | ebb9d177 | raw_rms/L_rms/R_rms + ProcessBalance 注释 |
| ok-20260807-14 | c80e6bcf | WRITE_GAP 单包检测 |
| ok-20260807-15 | 3c634bc3 | （14 同源连续固化） |
| ok-20260807-16 | 2e61aa13 | buffer 2048→8192 修 XRUN |
| ok-20260807-17 | 1020f46a | XPlayerReset→Destroy+重建 |
| ok-20260807-18 | b5a0f421 | out_dump 加 VOL/DG/DAP/DRC |
| ok-20260807-19 | ca8cd5c0 | dm_sound 对象泄漏修复（g_music_sound） |

验证命令：`./build.sh <nsh> -j$(nproc)` → `lunch_nuttx 2 && pack` → `md5sum nsh.fex vela.bin` → `bash /data/vela/git_snapshot.sh`

---

## 八、遗留事项

- **主问题**：切歌无声（DAC 模拟输出级第二次使用待物理验证）
- 独立已修复：XRUN 周期重启（buffer 8192）、dm_sound 泄漏、aplay EBUSY 残留（旧固件现象，本固件 destroy&recreate 后未见）
- 待收敛：诊断打点（corr/RMS/WRITE_GAP/out_dump 扩展）在问题解决后需降频/撤除
- shtc3 温湿度传感器 `Failed to send measure command` 反复出现（HPWORK 254 > 音频线程 240，up_mdelay(50) 阻塞）——独立低优先问题

---
*DevLog by AtomCode (deepseek-v4-flash)*
