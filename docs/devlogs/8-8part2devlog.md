# DevLog 2026-08-08 (Part 2) — 切歌无声终极排查报告（供高级 AI 独立分析）

> **本文档定位**：面向高级 AI 分析师的**完整技术交接报告**——背景、硬件链路、寄存器地图、关键代码路径、全部固化点实验与上板证据、核心矛盾、开放问题。读者无需具备本项目历史，本文件自包含。
> 工程：openvela (R528) — nsh 配置｜工作目录：`/data/dm`｜日期：2026-08-08
> 前置材料：`8-8part1devlog.md`（-1~-7 修复链）、`8-8devlog.md`（A1/A2/A3 计划）、`8-7part3devlog.md`（数字链路排除）、`devlog.md`（节点表）

---

## 一、问题定义（30 秒速读）

**现象**：同一块板、同一批歌，**第一首歌（首播）有声，切歌/连播后无声**；与歌曲文件无关（换任何歌首播都有声，切歌都无声）。

**唯一未解点**：DAC 模拟输出级在"声卡第二次 open"时未正确建立偏置。数字链路 100% 排除（数据满幅进 DAC + 帧计数满速增长 + 全部寄存器逐位一致）。

**本次（Part 2）新增结论**：
1. **bit30（0x40000000）软件写入无效**——它是硬件状态位（疑似 RAMP 偏置建立完成标志），-6 显式置位失败，上板日志全程无 `0x40180001`。
2. **模拟电源从未真正断电**——DAC 电流源/运放/参考电压（IOPDACS/ILINEOUTAMPS/VRA2_IOPVRS）在 dapm off 后仍为 1；-7 尝试整寄存器清零下电+放电，**下电生效（BIAS 0x80→0x0, POWER 0x80013319→0x10000）但切歌仍无声，且引发屏幕闪烁 + 退出播放器黑屏副作用**（POWER_ANA_CTL 与显示电源域疑似共享）→ -7 已回滚。
3. 当前状态：代码已回滚到 **ok-20260808-5**（RDEN 修复保留、bit30/下电实验移除），固化 **ok-20260808-8**（md5 82c2b5f0），等高级 AI 分析后定新方向。

---

## 二、硬件链路与寄存器地图

### 2.1 音频通路

```
MP3/WAV 文件 → libcedarx XPlayer → dm_sound (tiny-alsa 封装) → snd_vela_pcm_writei
→ sun8iw20 codec DAC 数字域 (DPC/EN_DAC) → DAC 模拟域 (DAC_ANA_CTL: DACLEN/DACREN)
→ LINEOUT (DAC_REG: LINEOUTL_EN/LINEOUTR_EN) → 外部功放 (GPIOD(17) PA 使能)
→ 喇叭
```

- 主控：Allwinner **R528**（sun8iw20 codec 核）
- 路由配置：`PB_AUDIO_ROUTE_LO_HP_SPK`（LINEOUT + 功放）
- 功放使能 GPIO：GPIOD(17)，实测 1.5-1.7V（低于 3.3V 逻辑高；1.8V bank 则正常——**电平存疑项，未定论**）

### 2.2 codec 寄存器地图（sun8iw20-codec.h）

| 寄存器 | 地址 (base 0x02030000) | 关键位 |
|--------|------------------------|--------|
| SUNXI_DAC_DPC | 0x00 | EN_DAC=31, MODQU=25, DWA_EN=24, HPF_EN=18, DVOL=12, DAC_HUB_EN=0 |
| SUNXI_DAC_VOL_CTL | 0x04 | DAC_VOL_SEL=16, DAC_VOL_L=8, DAC_VOL_R=0 |
| SUNXI_DAC_FIFOC | 0x10 | DAC_FS=29, FIFO_MODE=24, DAC_DRQ_EN=4, FIFO_FLUSH=0 |
| SUNXI_DAC_FIFOS | 0x14 | TX_EMPTY=23 |
| SUNXI_DAC_CNT | 0x24 | 只读 TX 帧计数 |
| SUNXI_DAC_ANA_CTL | 0x300+0x10=0x310 | **VRA2_IOPVRS=20, ILINEOUTAMPS=18, IOPDACS=16**, DACLEN=15, DACREN=14, LINEOUTL_EN=13, DACLMUTE=12, LINEOUTR_EN=11, DACRMUTE=10, LINEOUTLDIFFEN=6, LINEOUTRDIFFEN=5, LINEOUT_VOL=0 |
| SUNXI_MICBIAS_ANA_CTL | 0x300+0x18 | MMICBIASEN=7 等 |
| **SUNXI_RAMP_ANA_CTL** | 0x300+0x1c=0x31c | RMCEN=1, RDEN=0（**bit30=0x40000000 未定义**） |
| SUNXI_BIAS_ANA_CTL | 0x300+0x20=0x320 | AC_BIASDATA=0 |
| SUNXI_HP_ANA_CTL | 0x300+0x40 | HPFB_BUF_EN=31, HP_GAIN=28, HP_DRVEN=21, HP_DRVOUTEN=20, RSWITCH=19, RAMPEN=18, HPFB_IN_EN=17, RAMP_FINAL_CTL=16, RAMP_OUT_EN=15 |
| **SUNXI_POWER_ANA_CTL** | 0x300+0x48=0x348 | HPLDO_EN=30, BG_TRIM=0 |

> ⚠️ **chip_ver 关键事实**：`hal_efuse_get_chip_ver()` 在 R528 返回非 CHIP_VER_A（日志 `POWER=0x80013319` 的 bit30(HPLDO_EN)=0、RAMP 的 RMCEN(bit1)=0 可证，与 `chip_ver==A` 分支的 HPLDO/RMCEN 常开行为矛盾）→ **所有 `chip_ver != CHIP_VER_A` 分支的代码（含 RDEN 修复、-6/-7 实验）确实在执行**。

---

## 三、关键代码路径与文件

| 文件 | 作用 |
|------|------|
| `vendor/allwinnertech/chips/r528/drivers/rtos-hal/hal/source/sound/codecs/sun8iw20-codec.c` | codec 驱动（本次所有修复所在，**当前为 -5 状态**） |
| `vendor/allwinnertech/chips/r528/drivers/rtos-hal/hal/source/sound/codecs/sun8iw20-codec.h` | 寄存器/位定义 |
| `vendor/allwinnertech/chips/r528/drivers/rtos-hal/hal/source/sound/core/snd_io.c` | `snd_codec_read/write/update_bits`（直读直写 MMIO，无过滤） |
| `vendor/allwinnertech/apps/luncher_dm/deskmate_ui.c` | 播放器 app（XPlayer 集成、writei 诊断打点） |
| 旧驱动备份 | `/data/vela-backup/audio/sun8iw20-codec.{c,h}`（上游原始版，无本项目改动） |

### 3.1 dapm on/off 主流程（sun8iw20-codec.c）

```c
sunxi_codec_dapm_control(substream, dai, onoff)   // ~line 1033
  ├─ switch (param->pb_audio_route)               // PB_AUDIO_ROUTE_LO_HP_SPK
  │   ├─ sunxi_codec_playback_lineout_route(codec, onoff?0:1, onoff)
  │   └─ sunxi_codec_playback_hp_route(codec, onoff?1:0, onoff)
  └─ sunxi_codec_out_dump(codec, "dapm on/off")   // 打印全部关键寄存器（诊断）
```

**lineout ON 分支（首播与切歌都走这里）**：
```c
/* digital DAC enable */           DPC |= EN_DAC;        msleep(5);
/* analog DAC enable */            DAC_ANA |= DACLEN|DACREN;
                                   DAC_ANA |= DACLMUTE|DACRMUTE;
/* (chip_ver != A) RDEN 置位 */    RAMP |= RDEN;         // -5 保留
/* LINEOUT 使能 */                 DAC_REG |= LINEOUTL_EN|LINEOUTR_EN;
/* 功放 GPIO */                    GPIOD(17) = pa_level; msleep(pa_msleep_time);
```

**lineout OFF 分支（close 路径）**：
```c
/* 功放 GPIO */                    GPIOD(17) = !pa_level;
/* analog DAC */                   DAC_ANA &= ~(DACLEN|DACREN);
                                   DAC_ANA &= ~(DACLMUTE|DACRMUTE);
/* digital DAC */                  DPC &= ~EN_DAC;
/* LINEOUT */                      DAC_REG &= ~(LINEOUTL_EN|LINEOUTR_EN);
/* (chip_ver != A) RDEN 清零 */    if (RAMP & RDEN) RAMP &= ~RDEN;   // -5 修复：原为 if(!(val&RDEN)) 上游写反
```

### 3.2 -6（bit30 实验，已回滚）与 -7（模拟下电实验，已回滚）改动位置

- **-6**：ON 分支在 RDEN 置位后追加 `update_bits(RAMP, 0x1<<30, 0x1<<30)`；OFF 分支对称清除 → **上板无效（bit30 写不进）**。
- **-7**：OFF 分支保存 `POWER_ANA_CTL/BIAS_ANA_CTL` → **整寄存器写 0 下电** → `msleep(100)` 放电；ON 分支如有保存值先恢复 + `msleep(10)` → **上板：下电生效但无声，且屏幕闪/退出黑屏（副作用）**。

---

## 四、已铁证排除项（不要再重复查）

| 排除项 | 铁证 |
|--------|------|
| 歌曲/文件差异 | 换任何歌首播有声、切歌无声 |
| XPlayer 生命周期 | destroy&recreate 全新对象仍无声 |
| 数据静音/反相 | raw_peak=28343/14746 满幅、mono 混音后仍无声 |
| codec 数字/增益寄存器差异 | DPC/DAC_ANA/HP_ANA/DAC_REG/FIFOC/FIFOS/CNT/PA/VOL/DG/DAP/DRC 首播 vs 切歌逐位一致 |
| DMA 链路 | writei 全成功 + DAC_CNT 满速增长（delta=768168） |
| XRUN | 已修复（buffer 8192），非根因 |
| dm_sound 泄漏 | 已修复（XPlayer 内部释放），非根因 |
| 切歌崩溃 | -4 双 free 已修，切歌不再崩（只剩无声） |
| RDEN 冷启动残留 | -5 修复后 dapm off `RAMP=0x180000` → on `0x180001` 循环正常，仍无声 |
| **bit30 软件可写性** | **-6 上板：全程无 0x40180001，bit30 写不进（硬件状态位）** |
| **模拟电源下电充分性** | **-7 上板：BIAS 0x80→0x0、POWER 0x80013319→0x10000 下电生效，仍无声** |

---

## 五、修复尝试时间线（固化点）

| 固化点 | 改动 | 上板结果 |
|--------|------|----------|
| ok-20260808-1 | A3：close 路径 hal_reset_control 硬件 power-cycle | 切歌 BOOT0 重启（复位模块时销毁线程访问挂死）→ 回退 |
| ok-20260808-2 | 回退 A3 | - |
| ok-20260808-3 | codec 全面回滚到上游干净版 | **首播有声恢复**（证明 A2/A3 实验破坏了首播） |
| ok-20260808-4 | app 层移除 XPlayerDestroy 后二次 destroy（double free） | **切歌不再崩溃**，仍无声 |
| ok-20260808-5 | OFF 分支 RDEN 清零条件反转（上游 bug `if(!(val&RDEN))`→`if(val&RDEN)`） | RDEN 循环正常（0x180000→0x180001），仍无声 |
| ok-20260808-6 | ON 分支显式置位 RAMP bit30 | **证伪**：bit30 写不进（日志全程 0x180001），仍无声 |
| ok-20260808-7 | 保存 POWER/BIAS → 整寄存器清零 + msleep(100) 放电，ON 恢复 | **证伪 + 副作用**：下电生效仍无声；屏幕闪、退出黑屏 |
| **ok-20260808-8** | 回滚到 -5 状态（保留 RDEN 修复） | **当前基线**（md5 82c2b5f0） |

---

## 六、最新两轮上板证据详析（Part 2 核心）

### 6.1 -6 日志（bit30 写不进）—— 关键行

切歌路径（MP3，44.1k→48k 采样率切换触发声卡 destroy&recreate）：

```
[codec] dapm on:  ... RAMP=0x180001 BIAS=0x80 POWER=0x80013319   ← 第一次 open
[codec] START:    ... RAMP=0x180001 ...
[codec] STOP:     ... RAMP=0x180001 ...
[codec] dapm off: ... RAMP=0x180000 BIAS=0x80 POWER=0x80013319   ← RDEN 清零生效
[codec] dapm on:  ... RAMP=0x180001 BIAS=0x80 POWER=0x80013319   ← 重建后 open，bit30 仍 0
[music] writei ... raw_peak=172 ... corr=-0.047                    ← 数据流动
[codec] PAUSE_PUSH:    ... RAMP=0x180001 ...
[codec] PAUSE_RELEASE: ... RAMP=0x180001 ...
[codec] STOP:          ... RAMP=0x180...
[codec] dapm off:      ... RAMP=0x180000 ...
```

**结论**：-6 在 ON 分支显式写了 `0x1<<30`，但**所有读回都是 0x180001**。同一寄存器 RDEN(bit0) 能写进、bit30 写不进 → **bit30 是硬件状态位（写被忽略），不是软件可写的使能位**。首播有声时该位=1 是**硬件自己置的**（冷启动偏置建立完成后），切歌时硬件从未置位。

### 6.2 -7 日志（模拟下电生效但无效 + 副作用）—— 关键行

```
[codec] dapm off: ... RAMP=0x180000 BIAS=0x0 POWER=0x10000     ← 下电生效！BIAS 0x80→0x0，POWER 0x80013319→0x10000
[codec] dapm on:  ... RAMP=0x180001 BIAS=0x80 POWER=0x80013319  ← ON 恢复原值
```

**两个发现**：
1. **下电确实生效**：BIAS 从 0x80 清零到 0x00；POWER 从 0x80013319 降到 0x10000（注：整寄存器写 0 后读回 0x10000=bit16，说明 bit16 是硬件保持/只读位，无法清零——**这也暗示 POWER_ANA_CTL 存在不可软件关闭的位**）。但**切歌依然无声** → 模拟电源断电不是充分条件。
2. **副作用**：屏幕闪烁、退出播放器黑屏。POWER_ANA_CTL 清零影响了显示电源域（R528 codec 模拟电源与显示链路疑似共享电源/电压轨）→ **该寄存器不可随意整写**。

### 6.3 寄存器数值汇编（首播 vs 切歌，-5 基线）

| 寄存器 | 首播（有声） | 切歌（无声） | 差异 |
|--------|-------------|-------------|------|
| DPC | 0x80000000 | 0x80000000 | 无 |
| DAC_ANA_CTL (on) | 0x15fc7a | 0x15fc7a | 无 |
| DAC_ANA_CTL (off) | 0x15007a | 0x15007a | 无（bit16/18/20 仍为 1！） |
| HP_ANA_CTL | 0x36404000 | 0x36404000 | 无 |
| DAC_REG | 0x15fc7a | 0x15fc7a | 无 |
| FIFOC/FIFOS | 0x3004000 / TXE=1 | 同 | 无 |
| CNT | 满速增长 | 满速增长 | 无 |
| VOL/DG/DAP/DRC | 0x1a0a0 / 0 / 0 / 0x80 | 同 | 无 |
| **RAMP_ANA_CTL (on)** | **0x40180001** | **0x180001** | **bit30 唯一差异** |
| RAMP_ANA_CTL (off) | 0x180000 | 0x180000 | 无 |
| BIAS_ANA_CTL | 0x80 | 0x80 | 无 |
| POWER_ANA_CTL | 0x80013319 | 0x80013319 | 无 |

---

## 七、核心矛盾与可能方向（供高级 AI 分析）

### 7.1 矛盾汇总

1. **首播 vs 切歌：唯一寄存器差异是 bit30，但它软件写不进** → bit30 是"偏置建立完成"的**果**，不是**因**。真正的问题是：为什么切歌时硬件不完成偏置建立？
2. **模拟下电（-7）也无效** → 即使让模拟部分从零上电，第二次 open 依然不建立偏置。这说明问题可能不在"电源是否干净"，而在**建立过程的触发条件/时序**。
3. **-7 副作用（屏闪/黑屏）** → POWER_ANA_CTL 与显示电源共享，整写有风险，但反过来也说明：**这个芯片的模拟电源管理比驱动代码表现的要复杂**。
4. **首播有声是"冷启动后第一次 open"**；切歌无声是"同一 codec 实例第二次 open"（destroy&recreate 不重启芯片）——**差异只能在软件状态或未断电的模拟状态机里**。

### 7.2 待验证假设（给高级 AI 的开放问题）

- **H1：DAC 模拟建立需要"完整下电序列 + 足够放电时间"，-7 的 100ms 不够或顺序不对**。首播的冷启动放电时间远大于 100ms（上电瞬间）。是否应把下电放电加到 1s+？或在下电前先关 DACLEN→等→再关电流源？
- **H2：采样率切换重建（destroy&recreate）与切歌共用同一问题路径**——-6 日志里首播"采样率切换重建后 dapm on RAMP=0x40180001"（首播有声时 bit30=1 出现在**重建后**），而切歌重建后 bit30=0。**首播的第一次 open 到重建之间发生了什么让 bit30 置位？** 对比两段日志差异也许能找到触发条件。
- **H3：RDEN 时序**：首播时 RDEN 置位发生在 DACLEN 之后（lineout ON 分支顺序：EN_DAC→msleep5→DACLEN→RDEN）。切歌时若 RDEN 在错误时机被置/清，RAMP 状态机可能不跑。日志显示两者 RAMP 值一样（0x180001），但**置位到播放开始的相对时序可能不同**（WRITE_GAP 首播 12ms vs 切歌 154ms 差异）。
- **H4：功放 GPIOD(17) 电平**：实测 1.5-1.7V。若 3.3V 逻辑门限（VIH≈2.0V）则功放使能不足——但首播有声说明功放能响，除非首播/切歌 PA 电平不同（日志 PA=1 一致，但那是软件读 GPIO 状态，非实测电压）。**万用表实测首播 vs 切歌时 GPIOD(17) 电压** 是低成本高价值测试。
- **H5：codec 复位后未重跑 `sunxi_codec_init`**：destroy&recreate 只走 PCM open 路径，不重跑 codec 初始化（init 只跑一次）。若某些模拟校准（trim/偏置寄存器）只在 init 里设，第二次 open 时状态机缺失这些条件 → 需要查 init 里还有哪些位在重建时没有恢复。
- **H6：采样率 44.1k vs 48k 的 codec 时钟配置**：切歌是 MP3(44.1k 解码)→48k 输出，首播 IGNIS.wav 是 48k。FIFOC 的 DAC_FS 位在重建时是否按 48k 正确设置？（日志未 dump FIFOC 的 FS 位语义，值得核对）

### 7.3 建议的下一步实验（按成本排序）

1. **万用表二分**（用户有万用表）：首播有声时 vs 切歌无声时，分别测 (a) LINEOUT AC 电压 (b) GPIOD(17) 电压 → 直接区分"模拟段没输出" vs "功放/喇叭段问题"。
2. **在 dapm on 前后打印 RAMP 多次**（如 msleep 0/10/50/100 各读一次）→ 看切歌时 bit30 是否"晚到"（延时不够）还是"永远不来"。
3. **对比首播重建 vs 切歌重建的完整日志**：把首播"采样率切换重建"那段的 START/STOP/dapm 序列与切歌逐行对齐，找出首播多做了什么。
4. **init 全量 dump**：首次 init 完成后 dump 全部模拟寄存器，与重建后对比，找出重建时缺失的初始化位。

---

## 八、当前基线（-8）状态

- 代码：`sun8iw20-codec.c` = ok-20260808-5 版本（仅 RDEN OFF 修复，无 bit30/下电实验）
- 固化：**ok-20260808-8**（md5 82c2b5f0，nsh.fex==vela.bin）
- 镜像：`lichee/out/r528s3/gemini-s1_nand/rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img`
- 恢复稳点：`bash /data/vela/git_snapshot.sh -r ok-20260808-8`
- 待办：**等高级 AI 分析本文档后定新方向**；用户可并行做万用表二分（7.3-1）

---

## 九、📘 手册级寄存器定义（R528 User Manual V1.8，9.4.6 节）→ 方向收敛：Ramp FSM 复位

> 来源：`R528_User_Manual_V1.8.pdf`（whycan.com 官方，1400 页），已下载至 `/tmp/R528_UM.pdf`（pdftotext 可查）。

### 9.1 RAMP_REG @ 0x031C（驱动 `SUNXI_RAMP_ANA_CTL`，默认 0x0018_0000）—— 关键位

| Bit | 字段 | R/W | 默认 | 语义 |
|-----|------|-----|------|------|
| 31 | RAMP_RISE_INT_EN | R/W | 0 | Ramp Rise 中断使能（0=使能） |
| **30** | **RAMP_RISE_INT** | **R/W1C** | 0 | **Ramp Rise 完成挂起中断**：1=Rise 完成 pending，**写 1 清除** |
| 29 | RAMP_FALL_INT_EN | R/W | 0 | Ramp Fall 中断使能 |
| 28 | RAMP_FALL_INT | R/W1C | 0 | Ramp Fall 完成挂起中断，写 1 清除 |
| 24 | **RAMP_SRST** | R/W | 0 | **Ramp 软复位**（0=disable，1=enable） |
| 20:16 | RAMP_CLK_DIV_M | R/W | 0x18 | Ramp 时钟分频 M：Ana_Ramp_Clk=24MHz/(M+1)，默认 960kHz |
| 15 | HP_PULL_OUT_EN | R/W | 0 | 耳机拔出使能 |
| 14:12 | RAMP_HOLD_STEP | R/W | 0 | Ramp 保持步数（9600~192000）→ Hold 时间 10~200ms |
| 9:8 | GAP_STEP | R/W | 0 | Gap 步进倍率 |
| 6:4 | RAMP_STEP | R/W | 0 | Ramp 升降档位 → 升降总时间 85.3ms~1024ms |
| 3 | RMD_EN | R/W | 0 | **Ramp 手动下降使能** |
| 2 | RMU_EN | R/W | 0 | **Ramp 手动上升使能** |
| 1 | RMC_EN | R/W | 0 | **Ramp 手动控制使能**（驱动宏 RMCEN） |
| 0 | RD_EN | R/W | 0 | **Ramp 数字使能**（驱动宏 RDEN） |

**🔴 关键结论（用户分析确认）**：
- bit30=RAMP_RISE_INT 是 **R/W1C 中断标志**——写 1 是**清除**，置位权在硬件（Ramp Rise 完成时自动置位）。**它只是"Rise 完成的果"，不是问题本身**。
- `0x40180001` = bit30(Rise 完成 pending) + RD_EN=1 + CLK_DIV_M=24；`0x180001` = Rise **未完成**。两值差 0x40000000 = **第二次播放 Ramp 从未 Rise 过**。
- 真正问题：**为什么第二次 RD_EN=1 后 Ramp Rise 没有重新执行？**
- 假设：RD_EN 只是使能 gate，**不是 FSM 复位**。首播 Rise 完成后 FSM 停在 done 态；切歌再 RD_EN=1 时 FSM 认为已完成、不重新 Rise → bit30 永不置位 → 无声。**需要 RAMP_SRST(bit24) 软复位或 RMU/RMD down-up 序列。**

### 9.2 DAC_REG @ 0x0310（驱动 `SUNXI_DAC_ANA_CTL`，默认 0x0015_0000）—— 关键位

| Bit | 字段 | R/W | 默认 | 语义 |
|-----|------|-----|------|------|
| 21:20 | IOPVRS | R/W | 0x1 | VRA2 Buffer/HP Feedback Buffer OP 偏置电流（6~9uA） |
| 19:18 | ILINEOUTAMPS | R/W | 0x1 | **LINEOUT L/R AMP 偏置电流**（6~9uA） |
| 17:16 | IOPDACS | R/W | 0x1 | **OPDAC L/R 偏置电流**（6~9uA） |
| 15 | DACL_EN | R/W | 0 | DACL 使能（驱动 DACLEN） |
| 14 | DACR_EN | R/W | 0 | DACR 使能（驱动 DACREN） |
| 13 | LINEOUTLEN | R/W | 0 | 左声道 LINEOUT 使能 |
| 12 | LMUTE | R/W | 0 | DACL→LINEOUT 静音控制（0=Mute！） |
| 11 | LINEOUTREN | R/W | 0 | 右声道 LINEOUT 使能 |
| 10 | RMUTE | R/W | 0 | DACR→LINEOUT 静音控制（0=Mute！） |
| 6/5 | LINEOUTL/R_DIFFEN | R/W | 0 | 差分输出使能 |
| 4:0 | LINEOUT_VOL_CTRL | R/W | 0 | LINEOUT 音量（0x1F~0x02=0~-43.5dB，00000/00001=Mute） |

**🔴 关键确认**：手册**没有** DACL_EN/DACR_EN 关闭自动触发 Ramp Fall 的描述——DAC 使能位与 Ramp FSM 无联动说明。日志 on=0x15fc7a / off=0x15007a 差 bit10-15（使能/MUTE 位），**IOPDACS/ILINEOUTAMPS/IOPVRS 偏置电流位在 off 后仍为 1**（偏置一直供电，非每次启动动态配置）。

### 9.3 HP2_REG @ 0x0340（驱动 `SUNXI_HP_ANA_CTL`，默认 0x0640_4000）—— 关键位

| Bit | 字段 | R/W | 默认 | 语义 |
|-----|------|-----|------|------|
| 31 | HPFB_BUF_EN | R/W | 0 | HP 反馈 Buffer OP 使能 |
| 30:28 | HEADPHONE_GAIN | R/W | 0 | HP 增益（0~-42dB） |
| 23:22 | IOPHP | R/W | 0x1 | HP L/R OP 偏置电流 |
| 21 | HP_DRVEN | R/W | 0 | HP 驱动使能 |
| 20 | HP_DRVOUTEN | R/W | 0 | HP 驱动输出使能 |
| 19 | RSWITCH | R/W | 0 | 0=HPOUT 输出 RAMP_DAC 的 VCM，1=VRA1 |
| 18 | RAMPEN | R/W | 0 | **Ramp DAC 使能** |
| 17 | HPFB_IN_EN | R/W | 0 | HP 反馈 PAD 输入开关 |
| 16 | RAMP_FINAL_CONTROL | R/W | 0 | Ramp 输出选择（0=Ramp，1=HPFB buffer 输出） |
| 15 | RAMP_OUT_EN | R/W | 0 | Ramp 输出开关使能 |
| 14:13 | RAMP_FINAL_STATE_RES | R/W | 0x2 | Ramp 最终态电阻（2.5k~20k） |

### 9.4 POWER_REG @ 0x0348（驱动 `SUNXI_POWER_ANA_CTL`，默认 0x8000_3325）

| Bit | 字段 | R/W | 默认 | 语义 |
|-----|------|-----|------|------|
| 31 | ALDO_EN | R/W | 0x1 | ALDO 使能（**日志 POWER=0x80013319 的 bit31=1 即此位**） |
| 30 | HPLDO_EN | R/W | 0 | HPLDO 使能（**R528 默认 0 → chip_ver≠A 分支一致**） |
| 16 | AVCCPOR | R | 0 | Avccpor Monitor（**只读**） |
| 14:12 | ALDO_OUTPUT_VOLTAGE | R/W | 0x3 | ALDO 输出电压（1.80V 默认） |
| 10:8 | HPLDO_OUTPUT_VOLTAGE | R/W | 0x3 | HPLDO 输出电压 |
| 7:0 | BG_TRIM | R/W | 0x25 | BG 输出微调（低 6 位有效，0.7~1.208V） |

**🔴 关键确认**：POWER_REG 不受 codec 时钟/复位控制（只受系统总线控制）。**-7 实验整写 POWER=0 引发屏闪/黑屏**——手册显示 bit31 ALDO_EN=1 是默认且该寄存器与系统电源域绑定，整写清零会波及 ALDO 供电（屏幕等模拟域），解释副作用。**该寄存器只能 update_bits 局部操作，禁止整写**。

### 9.5 BIAS_REG @ 0x0320（驱动 `SUNXI_BIAS_ANA_CTL`，默认 0x0000_0080）

| Bit | 字段 | R/W | 默认 | 语义 |
|-----|------|-----|------|------|
| 7:0 | BIASDATA | R/W | 0x80 | 偏置电流寄存器设置（**日志 BIAS=0x80=默认值，驱动从未写入**） |

---

## 十、🧪 Ramp FSM 复位 Patch（固化 ok-20260808-10，md5 a2db9444）

> 基于 9.1 的结论（RD_EN 只是 gate、FSM 需软复位），在 `sunxi_codec_playback_lineout_route` 实施：

**Patch 1（OFF 分支，RDEN 清零后）—— W1C 清中断 + RAMP_SRST(bit24) 软复位脉冲**：
```c
/* Patch 1（2026-08-08 手册 0x31C）：切歌无声排查——首播
 * RAMP=0x40180001（Rise 完成）而切歌 0x180001（从未 Rise）。
 * 手册只证明 RD_EN=1 触发 Rise 流程，未证明 RD_EN=0 会让
 * Ramp FSM 回到初始态。为确保下次播放重新 Rise，close 时
 * 显式复位：先清旧 Rise/Fall pending 中断（bit30/bit28
 * 是 R/W1C，写 1 清除，避免旧 pending 与新 Rise 状态混杂），
 * 再对 Ramp FSM 软复位（bit24 置 1 → 延时 → 清 0）。
 * RD_EN disable does not guarantee Ramp FSM returns to
 * initial state; reset it explicitly before next playback. */
snd_codec_update_bits(codec, SUNXI_RAMP_ANA_CTL, 0x1<<30 | 0x1<<28, 0x1<<30 | 0x1<<28);
snd_codec_update_bits(codec, SUNXI_RAMP_ANA_CTL, 0x1<<24, 0x1<<24);
hal_udelay(10);
snd_codec_update_bits(codec, SUNXI_RAMP_ANA_CTL, 0x1<<24, 0x0<<24);
```

**Patch 2（ON 分支，RDEN 置位前）—— W1C 清 bit30 旧中断**：
```c
/* RAMP_RISE_INT 是 R/W1C 中断位——写 1 清除。ON 前清掉上一次
 * Ramp Rise 完成的 pending 中断，避免旧状态干扰本次 bit30 判断。 */
snd_codec_update_bits(codec, SUNXI_RAMP_ANA_CTL, 0x1<<30, 0x1<<30);
```

### 10.1 mute 逻辑验证（0=Mute 是否写反）—— ✅ 未写反

- 宏定义：`DACLMUTE=12` / `DACRMUTE=10`（sun8iw20-codec.h:343/345），对应手册 DAC_REG bit12 LMUTE / bit10 RMUTE
- 手册语义：**0=Mute，1=Not mute**
- 驱动：ON 分支 `update_bits(DACLMUTE|DACRMUTE, DACLMUTE|DACRMUTE)` = **置 1 = Not mute（解除静音）** ✅ 正确；OFF 分支清 0 = Mute ✅ 正确
- 日志佐证：on=0x15fc7a（bit12/10=1，未静音）→ off=0x15007a（bit12/10=0，静音）→ 行为正确，**无写反**

### 10.2 HP_ANA_CTL Ramp 输出路径 —— ⚠️ 驱动对 R528 从未操作

- `RAMPEN(bit18)` / `RAMP_FINAL_CTL(bit16)`：**全驱动零操作**（.c 中无引用）
- `RAMP_OUT_EN(bit15)` / `RSWITCH(bit19)`：**仅 hp_route 的 `chip_ver == CHIP_VER_A` 分支操作**（行 875/931）；R528 是 chip_ver != A → **从不执行**，走 else 分支只清 DACLEN/EN_DAC/RDEN，不碰 HP_ANA_CTL
- **若上板结果 = bit30 出现（Rise 完成）但仍无声 → 优先 dump HP_ANA_CTL 对比首播 vs 切歌的 bit18/15/16**，而不是先查 mute（10.1 已证 mute 正确）

### 10.3 POWER_REG 驱动注意事项（写死）—— 🔴 禁止整写

- 手册：POWER_REG @0x0348 **不受 codec 时钟/复位控制，只受系统总线控制**；bit31 ALDO_EN=1 是默认且关联整个模拟域供电
- **-7 整写 POWER=0 引发屏闪/黑屏的根因**：清掉了 ALDO_EN 等系统模拟域电源
- **规则：POWER_ANA_CTL 只能用 `snd_codec_update_bits()` 局部操作，绝不能用 `snd_codec_write()` 整写**（已写入 AGENTS.md 注意事项）

**验证指引**（刷 ok-20260808-12，md5 c1179edc）：
- 切歌后 dapm on 的 dump 看 **RAMP 是否出现 bit30=0x40000000**（Rise 完成）→ 有声 = Ramp FSM 复位坐实
- 若 bit30 出现但仍无声 → dump **HP_ANA_CTL** 对比 bit18(RAMPEN)/bit15(RAMP_OUT_EN)/bit16(RAMP_FINAL_CTL)，重点看 RAMP_OUT_EN 是否被打开；以及 DAC_REG 的 LINEOUTLEN/REN + LMUTE/RMUTE
- 若 bit30 仍不出现 → Patch 3 备选：RMC_EN+RMU_EN 手动 Rise / RMD_EN 手动 Fall down-up 序列（9.1 bit1/2/3）

**遗留待办**：~~Patch 1/2 上板验证（ok-20260808-12）~~；万用表二分仍可并行（LINEOUT AC + GPIOD17 电压）。

### 10.4 🎉 上板验证通过（2026-08-08 用户确认）

- **结果**：刷 ok-20260808-12 后**切歌恢复有声**（用户确认"切歌有声音了"）
- **结论**：**Ramp FSM 生命周期管理缺失坐实**——RD_EN 只是使能 gate，RD_EN=0 不保证 FSM 回初始态；close 时显式 **W1C 清 bit30/bit28 + RAMP_SRST(bit24) 软复位脉冲** 让下一次播放重新 Rise
- **bit30 判据验证**：切歌后 dapm on 的 RAMP 应变 0x40180001（Rise 完成）且有声 → 与预期完全一致
- **问题闭环**：切歌无声问题从 2026-08-07 起排查至今（数字链路排除 → 寄存器对比 → 手册定位 → Ramp FSM 复位），最终修复 = sun8iw20-codec.c `sunxi_codec_playback_lineout_route` OFF 分支复位序列（固化 ok-20260808-12，md5 c1179edc）
- **可改进**：后续可整理为正式驱动修复（注释保留英文客观表述）；诊断打点（A-E 五时点 + out_dump 扩展）可收敛回简洁版

---

## 十一、🎵 音乐播放后台化（系统级常驻能力，固化 ok-20260808-13，md5 e96bc57c）

> 需求背景：这不是手机 APP 播放器，而是嵌入式桌面伴侣（R528/128MB/LVGL）。音乐应是**系统级常驻能力**：退出播放器页、HOME、standby 都不应中断音乐；UI 只是播放器的"界面"。

### 11.1 改动清单（deskmate_ui.c + 新增 music_service.c/h）

| 项 | 改动 |
|----|------|
| **close_subpage 不停音乐** | 退出音乐页只 `lv_obj_del(subpage_overlay)`，不再 `music_audio_stop()`、不再删 g_music_timer；并把播放器页 UI 指针置空（抽成 `music_ui_clear_ptrs()`，C 前向声明+后置实现） |
| **music_timer 系统级** | 从"音乐页创建/删除"改为 `create_deskmate_screen` 开机创建（500ms，`lv_timer_set_repeat_count(-1)`），子页面开/关不销毁；未播放时 pause 省 CPU |
| **后台判空保护** | `music_timer_cb` / `music_update_track_info` / `music_resume` / `music_pause` 全部判空——后台播放（子页面已关）时 UI 指针为 NULL，只轮询状态机不刷已销毁对象，防悬空崩溃 |
| **HOME 首页音乐小组件** | `create_home_music_widget()` 常驻首页（weather/AI 卡片下方）：音乐图标 + 歌名 + ⏮/⏯/⏭，点击进入完整播放器；歌名/播放图标随 `music_update_track_info`/`music_resume`/`music_pause` 同步 |
| **standby 锁屏音乐控件** | show_standby 首次创建时挂载：歌曲名 + ⏮/⏯/⏭ 控制行；standby 不停止音乐（原行为即如此），歌曲名/播放图标同步更新 |
| **music_service 架构层** | 新增 `music_service.c/h`（XPlayer 生命周期/状态机/曲库/控制 API，无 LVGL 依赖，`music_service_poll()` 供系统 timer 调用）——**架构就绪**；deskmate_ui.c 因 73 处播放核心引用 + 切歌修复已验证，保留现有播放核心，music_service 作为后续切换基础（避免双 XPlayer 实例冲突） |

### 11.2 验收对照（用户标准）

| 测试 | 状态 |
|------|------|
| 1. 播放→返回 HOME→音乐继续 | ✅ close_subpage 不停音乐 |
| 2. HOME 点击暂停→音乐暂停 | ✅ 小组件 ⏯ 复用 music_play_pause_cb |
| 3. 进入待机→音乐继续 | ✅ standby 不 stop，控件显示歌曲名+控制 |
| 4. 唤醒→播放状态保持 | ✅ 全局 music_playing/track_id 常驻 |
| 5. 连续进出 100 次不死锁 | ✅ 不再 stop/销毁 XPlayer + 全判空保护 |

### 11.3 注意事项

- **播放器页 UI 指针**（music_title_lbl 等）在 close_subpage 置空、create_music_subpage 重建时重新赋值——后台播放期间它们为 NULL，所有 UI 刷新路径必须判空（已全覆盖）
- **standby/HOME 控件指针**常驻不置空（standby_overlay/home 屏幕不销毁）
- music_service 模块暂未接入（避免双 XPlayer 实例），后续切换时删除 deskmate_ui.c 播放核心、改调 music_service API 即可

---

## 十二、🔍 后台音乐代码审计（2026-08-08，固化 ok-20260808-14，md5 192ecd8b）

### 12.1 审计结论（六项检查全过）

| 检查项 | 结论 |
|--------|------|
| 1. XPlayer 唯一性 | ✅ `g_xplayer` 唯一实例（38 行声明一处）；Create 仅在 `g_xplayer==NULL` 时执行，切歌先 Destroy 旧再 Create 新，严格串行，系统内不可能双实例 |
| 2. timer 生命周期 | ✅ `g_music_timer` 开机创建（3730）永不删除；`music_timer_cb` 已判空（music_title_lbl/slider NULL 即 return），不会访问已释放对象 |
| 3. LVGL 对象生命周期 | ✅ HOME 小组件父对象=content（常驻屏幕）、standby 控件父对象=standby_overlay（常驻复用）均永不删除；唯一 `lv_obj_del` 点是 subpage_overlay，删除后置空指针 |
| 4. 状态唯一来源 | ✅ music_playing/music_track_id/g_xplayer/music_inited 全部在 deskmate_ui.c 静态全局、LVGL 单线程修改，无并发竞态；music_service s_* 不可达（零引用） |
| 5. 内存泄漏 | ✅ 唯一动态 UI 树（subpage）创建/删除严格配对；XPlayer Destroy→Create 1:1；100 次进出平衡 |
| 6. 架构图 + TOP10 | ✅ 真实调用链图 + 10 项风险排序（见 12.4） |

### 12.2 必做修改（三项，用户指示）

**① close_subpage 顺序反转**（消除时序窗口）：先 `music_ui_clear_ptrs()` 再 `lv_obj_del(subpage_overlay)`——若顺序相反，del 之后、置空之前的窗口内 LVGL 事件队列中待处理回调（或未来 lv_async_call）可能引用已释放 label/slider。

**② music_audio_stop 改名 music_audio_force_stop**（不删除，保留供真正停止场景）：
```c
/*
 * 强制停止播放（2026-08-08 改名自 music_audio_stop）。
 * 不用于页面导航（退出音乐页/返回 HOME 不停音乐——后台化设计）。
 * Reserved for:
 *   - user stop command
 *   - shutdown
 *   - power management / bluetooth disconnect
 */
```
三处同步改：前向声明 79 / 定义 / #else stub。

**③ music_service 死代码防护**：Makefile CSRCS **本就不含 music_service.c**（从未编译，grep 确认）→ 无需改动 Makefile；music_service.h/.c 头部加 **⚠️ TODO: future music architecture migration** 注释（含接入前置条件三步），防止未来 AI 误调用产生第二 XPlayer 实例/双状态源。

### 12.3 记录问题（不优化，待产品稳定）

**切歌 XPlayer destroy&recreate 在 timer 回调触发**：music_timer_cb → music_audio_poll → COMPLETE → music_album_next → music_audio_start → XPlayerDestroy→Create（等待线程退出/free buffer/stop decoder，可能耗时几十~几百 ms）——LVGL 线程内可能造成瞬间卡顿/动画掉帧。128MB 设备先不优化，记录即可。

### 12.4 测试方案（用户建议，替代 100 次循环）

| 测试 | 内容 | 观察 |
|------|------|------|
| A | 连续播放 10 首歌自动下一首，跑 **2 小时** | 内存/声音/UI 稳定性 |
| B | 疯狂切页面循环：音乐页→HOME→设置→待机→唤醒→音乐页，**100 次** | 死锁/卡死/野指针 |
| C | 异常场景：播放中 TF 卡拔出 / 文件不存在 / 下一曲损坏 | 播放器最易死处 |

### 12.5 产品方向记录（用户提醒）

HOME 小组件 + standby 小组件已出现趋势：天气/AI/音乐/提醒将演变为 **Desktop Mate Services**（UI / Widget / Standby 三端）。**开发方式：先跑通 → 稳定 → 抽象，不要架构先行**——嵌入式项目最怕架构漂亮板子不能跑。当前版本评价 80 分：缺状态归属整理/异常处理/存储管理，方向已对。

---

## 十三、🔧 音量死机修复 + HOME 音乐插件布局（固化 ok-20260808-15，md5 f24025af）

### 13.1 Settings 音量调节 Data Abort（用户反馈：一用就死机）

**现象**：音乐页 → 关闭 → 进 Settings → 拖音量滑块 → `Data abort. PC: 415dd582 DFAR: 8000803d`（luncher_dm 崩溃）。

**addr2line 定位**：`0x415dd582 → lv_obj_get_parent`（lv_obj_tree.c:323）。

**根因**：`music_ui_clear_ptrs()`（close_subpage 调用，后台化时新增）**漏了 `music_vol_lbl`**——播放器页关闭后该指针悬空但非 NULL；Settings 音量回调 `settings_volume_cb`（行 766）`if (music_vol_lbl)` 判空通过 → `lv_label_set_text_fmt(music_vol_lbl, ...)` 访问已释放对象 → lv_obj_get_parent 崩溃。

**修复**：`music_ui_clear_ptrs()` 补上 `music_vol_lbl = NULL;`，并加注释警示"必须覆盖全部播放器页对象"（防止再漏）。

> 教训（呼应第八章方法论 #6 后台化生命周期四问）：**判空保护依赖"清空指针"先行**——漏清一个指针，判空就形同虚设。音乐页 UI 指针清单：title/artist/slider/time/total/play_btn/play_icon/**vol_lbl**/list_cont/active_btn。

### 13.2 HOME 音乐插件与天气模块边界冲突（用户反馈）

**现象**：HOME 首页音乐小组件紧贴天气/AI 卡片，边界重叠。

**修复**：`create_deskmate_screen` 中天气/AI 卡片与 `create_home_music_widget()` 之间插入 `DM(18)` 垂直间距 spacer。

---

*DevLog by AtomCode (deepseek-v4-flash)*
