# DevLog 2026-08-08 — 切歌无声终极排查：A2/A3 证伪 → codec 回滚 → 首播恢复 → 双 free 修复 → RDEN/bit30 链路

> 工程：openvela (R528) — nsh 配置
> 工作目录：`/data/dm`
> 前置：`8-8devlog.md`（A2 修复计划）、`8-7part3devlog.md`（数字链路 100% 排除，9 固化点 ok-20260807-11~19）
> 状态：**bit30 显式置位已证伪（-6 上板写入无效，bit30 为硬件状态位），改用模拟电源真正下电修复（固化 ok-20260808-7），待上板验证**

---

## 一、背景

8-8devlog.md 规划了任务 A（代码侧 A1/A2/A3 阶梯）与任务 B（万用表）。今日按决策树执行：上板验证 A2（RDEN 冷启动修复，固化于 ok-20260807-20）→ 若无效走 A3（硬件 power-cycle）→ 再无效走任务 B。实际过程经历了 **六次固化、三次方向修正**，最终收敛到 **RAMP bit30 唯一寄存器差异**。

---

## 二、排障过程（按「现象→根因→方案→验证」）

### 2.1 A2 上板证伪：RDEN 冷启动循环正常但切歌仍无声

| 项 | 内容 |
|----|------|
| **现象** | 刷 -20（A2：ON 分支强制 RDEN 0→1 + msleep(10/50)）上板，dapm on `RAMP=0x180001` → off `RAMP=0x180000` → 再 on `0x180001`——**RDEN 每次 open 都正确 0→1 冷启动循环，但切歌依然无声** |
| **铁证** | writei 全成功（wr=1920）、CNT 满速增长（delta=721032）、raw_peak=3969 满幅、PA=1、corr=0.028 无反相；**POWER=0x80013319 / BIAS=0x80 在 dapm on/off 完全一致 = 模拟电源从未真正断电**（代码只清了 DACLEN/EN_DAC/LINEOUT 数字使能位，POWER_ANA_CTL/BIAS 全程未动） |
| **结论** | RAMP RDEN 冷启动假设排除；剩项仍为 DAC 模拟输出级 |

### 2.2 A3 硬件 power-cycle 落地 → 上板失败（切歌 BOOT0 重启）→ 回退

| 项 | 内容 |
|----|------|
| **方案** | dapm off（close）时 `hal_reset_control_get/assert` 把 codec 全部模拟状态机打回上电初值 → `msleep(100)` 放电 → `deassert` 重新上电 → `sunxi_codec_init` 重跑初始化（固化 ok-20260808-1） |
| **现象** | `[codec] power-cycle done (close)` 正常打出，但**切歌（destroy&recreate）时系统直接 BOOT0 重启** |
| **根因** | reset assert 在 close 路径复位整个 codec 模块（含总线/时钟），此时 audioRender/解码线程仍在销毁（`destroy_ResampleInfo`），访问被复位模块挂死 → 看门狗重启 |
| **处置** | 回退 A3（固化 ok-20260808-2，md5 4e66299e） |

### 2.3 切歌 luncher_dm panic + 首播无声 → codec 层全面回滚

| 项 | 内容 |
|----|------|
| **现象** | 刷 -2（已回退 A3，A2 仍在）复测：切歌时 luncher_dm 进程 **`Undefined instruction at 0x4239b2a0` panic**（destroy_ResampleInfo 后）；首播 Aurora 也无声（raw_peak=0/首包，CNT 涨到 3008512 但无声） |
| **判断** | A2/A3 实验均未解决无声且带来不稳定（首播回归 + panic）；codec 层已无可信增量 |
| **处置** | **codec 层全面回滚**：`git checkout 6f7340d5 -- sun8iw20-codec.c`（-19 A2 前上游干净版，撤销 A2 RDEN 改动 + out_dump 扩展）；固化 ok-20260808-3（md5 beeb283d） |

### 2.4 🎉 首播有声恢复 + 切歌死机真凶：XPlayer 内部双 free

| 项 | 内容 |
|----|------|
| **现象** | 刷 -3 上板：**首播有声了**（writei raw_peak=28343 满幅、corr=0.868 正常、用户确认"不卡顿了，听起来"）→ 证明 A2/A3 实验确实破坏了首播；但切歌仍 panic（地址每次不同 0x4239b2a0/0x423303c0 = 堆地址跳转执行） |
| **根因** | 深挖 libcedarx：`XPlayerDestroy` → `PlayerDestroy` → `PlayerClear`（player.c:356，`CONFIG_ONLY_DISABLE_AUDIO=0` 编译生效）**内部已调用 `SoundDeviceDestroy(p->pAudioSink)` 销毁 sink**——pAudioSink 正是 `XPlayerSetAudioSink` 传入的 dm_sound（ops->destroy = dm_sound_destroy → snd_vela_pcm_close + free）；而 app 层 `music_audio_start`（ok-20260807-19 加"防泄漏"）在 XPlayerDestroy 后又调 `g_music_sound->ops->destroy()` **二次 free 同一对象 → use-after-free → 崩溃**（-19 注释"XPlayer 不释放 SoundCtrl"是错的） |
| **修复** | **移除 app 层手动 destroy，只置 `g_music_sound = NULL`**（内存由 XPlayer 内部 PlayerClear 释放）；固化 ok-20260808-4（md5 727776fc） |

### 2.5 切歌不崩但无声 → RDEN OFF 分支清零条件写反（上游 bug）

| 项 | 内容 |
|----|------|
| **现象** | 刷 -4 上板：**切歌不再崩溃**（double free 修复生效），但**切歌依然无声**；用户决定性反馈"**第一首听到声音了，切歌后没声音**"→ **推翻"硬件物理段"假设**（同板首播能响 = 硬件链路通，问题 100% 在切歌路径软件状态） |
| **根因** | 代码审查发现 **`sunxi_codec_playback_lineout_route` OFF 分支 RDEN 清除条件写反**（上游原始 bug）：`if(!(reg_val & RDEN))` 只在 RDEN **已为 0** 时才写 0 = **永不清除 RAMP 使能** → 首播 close 后 RDEN 残留 1，切歌第二次 open 时 RAMP 状态机认为偏置已建立而跳过逐级建立 → DAC 模拟输出 0 → 无声（与"首播有声/切歌无声/寄存器全一致"铁证完全吻合）。此前 A2 修复方向正确但改太大（ON 分支强制 0→1 破坏首播） |
| **修复** | **只修 OFF 分支一行**：`if(!(val&RDEN))` → `if(val&RDEN)`（RDEN 置位时才清零），ON 分支首播路径保持不动；out_dump 加回 RAMP/BIAS/POWER 诊断；固化 ok-20260808-5（md5 162c7113） |

### 2.6 🔴 RDEN 循环已正常但 bit30 未置位 → 显式置位修复（当前状态）

| 项 | 内容 |
|----|------|
| **现象** | 刷 -5 上板，日志铁证：**RDEN 循环已完全正常**（dapm off `RAMP=0x180000` → dapm on `RAMP=0x180001`，-5 修复生效）但**切歌依然无声** → RDEN 残留假设排除 |
| **关键新差异** | 逐位对比首播 vs 切歌的 RAMP 值：**首播（有声）采样率切换重建后 dapm on `RAMP=0x40180001`（bit30 置位）**，而**切歌（无声）所有 dapm on 均为 `RAMP=0x180001`（bit30 未置位）**——**bit30 (0x40000000) 是首播 vs 切歌唯一寄存器差异**，疑似 RAMP 偏置建立使能/完成标志 |
| **修复** | `sunxi_codec_playback_lineout_route` ON 分支每次 open 显式置位 bit30（`0x1<<30`），OFF 分支对称清除——复现首播有声时的寄存器值 0x40180001；固化 ok-20260808-6（md5 1cc8ccf1） |
| **验证指引** | 刷 ok-20260808-6 → 切歌后 out_dump 的 RAMP 应变 **0x40180001**（bit30 置位）；切歌恢复有声 = bit30 坐实为偏置建立标志；仍无声 → 任务 B 万用表 AC 档测 LINEOUT 二分兜底 |

### 2.7 ❌ bit30 显式置位证伪（-6 上板）→ ✅ 模拟电源真正下电（-7，当前状态）

| 项 | 内容 |
|----|------|
| **现象** | 刷 -6 上板，切歌（第二首为 **MP3，44.1k→48k 采样率切换重建**）后**仍无声**；用户确认"第二首仍无声" |
| **铁证①（bit30 写不进）** | 逐行核对整段日志：**所有 dapm on/START/PAUSE 均为 `RAMP=0x180001`，dapm off 为 `0x180000`，全程未出现 `0x40180001`** —— -6 在 ON 分支的 `update_bits(0x1<<30, 0x1<<30)` 写入无效；同寄存器 RDEN(bit0) 能写进（循环 0→1 正常）而 bit30 写不进 → **bit30 是硬件状态位（偏置建立完成标志），软件无法强制置位**，-6 方向证伪 |
| **铁证②（模拟电源从未断电）** | DAC_ANA on=0x15fc7a → off=0x15007a 只差 bit10-15（DACLEN/DACREN/LINEOUT_EN/MUTE 数字使能位），**bit16(IOPDACS)/bit18(ILINEOUTAMPS)/bit20(VRA2_IOPVRS) 在 off 后仍为 1**；POWER=0x80013319 / BIAS=0x80 全程一致 → **DAC 电流源/运放/参考电压从未下电** → 切歌第二次 open 时 RAMP 状态机认为偏置已建立而跳过逐级建立 → bit30（完成标志）永不置位 → 模拟输出 0 → 无声。与首播（冷启动完整建立、bit30 由硬件置位、有声）完全吻合 |
| **修复（-7）** | `sunxi_codec_playback_lineout_route`：**OFF 分支**保存 POWER_ANA_CTL/BIAS_ANA_CTL 当前值 → **整寄存器清零下电 → `hal_msleep(100)` 放电**；**ON 分支**如有保存值先恢复 POWER/BIAS + `msleep(10)` 再走原有数字/模拟 DAC 使能序列——让每次 open 都走首播那样的冷启动偏置建立路径（比 A3 reset 温和：只动 POWER/BIAS 寄存器，不碰总线/时钟） |
| **验证指引** | 刷 ok-20260808-7 → 切歌后 dapm off 日志应见 **POWER=0x00000000 / BIAS=0x00**（下电铁证）→ dapm on 恢复原值 → 切歌恢复有声 = 模拟下电坐实；若切歌仍无声但下电生效 → 模拟下电非充分条件，走任务 B 万用表 AC 档测 LINEOUT 二分兜底 |

---

## 三、改动文件表

| 固化点 | 改动文件 | 说明 |
|--------|----------|------|
| ok-20260808-1 | `vendor/allwinnertech/chips/r528/drivers/rtos-hal/hal/source/sound/codecs/sun8iw20-codec.c` | A3：dapm off 加 hal_reset_control 硬件 power-cycle（后回退） |
| ok-20260808-2 | 同上 | 回退 A3（-1） |
| ok-20260808-3 | 同上 | codec 全面回滚到 -19 上游干净版（git checkout 6f7340d5） |
| ok-20260808-4 | `vendor/allwinnertech/apps/luncher_dm/deskmate_ui.c` | 移除 XPlayerDestroy 后二次 `g_music_sound->ops->destroy()`（double free），只置 NULL |
| ok-20260808-5 | `vendor/allwinnertech/chips/r528/drivers/rtos-hal/hal/source/sound/codecs/sun8iw20-codec.c` | OFF 分支 RDEN 清零条件反转（`if(!(val&RDEN))` → `if(val&RDEN)`）+ out_dump 加回 RAMP/BIAS/POWER |
| ok-20260808-6 | 同上 | ON 分支显式置位 bit30（`0x1<<30`）、OFF 分支对称清除（后证伪回滚） |
| ok-20260808-7 | 同上 | 回滚 -6 bit30；OFF 分支保存 POWER/BIAS 后全清下电 + `msleep(100)` 放电，ON 分支恢复（模拟电源真正断电） |

---

## 四、验证命令与产物

```bash
# 每次固化：
cd /data/dm && ./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh -j$(nproc)
cd /data/dm/vendor/allwinnertech/lichee && source envsetup.sh && lunch_nuttx 2 && pack
md5sum .../image/nsh.fex /data/dm/nuttx/vela.bin   # 每次均一致
bash /data/vela/git_snapshot.sh                     # 自动 tag
```

| 固化点 | md5 | 状态 |
|--------|-----|------|
| ok-20260808-1 | d7480cbd | A3 落地，上板失败（切歌重启）→ 已回退 |
| ok-20260808-2 | 4e66299e | A3 回退 |
| ok-20260808-3 | beeb283d | codec 全面回滚 |
| ok-20260808-4 | 727776fc | 双 free 修复，切歌不再崩 |
| ok-20260808-5 | 162c7113 | RDEN OFF 分支修复，循环正常 |
| ok-20260808-6 | 1cc8ccf1 | bit30 显式置位，**上板证伪（写不进）→ 已回滚** |
| ok-20260808-7 | 66f60e65 | 模拟电源真正下电（保存→全清→放电→恢复），**待上板验证** |

---

## 五、遗留事项

1. **🔴 切歌无声（最高优先）**：模拟电源下电修复（-7）待上板验证——切歌后 dapm off 应见 POWER=0x00000000/BIAS=0x00（下电铁证），恢复有声 = 坐实；仍无声 → 任务 B 万用表 AC 档测 LINEOUT 二分（首播有 AC / 切歌无 AC → 模拟段继续查；都有 AC → 功放/喇叭物理段，查 GPIOD(17) 1.5-1.7V 电平）
2. **退出播放器重进点播放卡死**（新发现）：`close_subpage` 只 XPlayerStop 不 destroy → g_xplayer 残留，重进走 destroy&recreate 路径卡死（app 层，另案）
3. 卡顿：`MMCSD_MULTIBLOCK_LIMIT=1` 单块读（稳定不崩，卡顿仍在，-22 结论）；多块读需先上板验证 ok-20260807-4 修复
4. Settings 亮度/音量（-23 待验证）；诊断打点已收敛（-24）
5. 独立遗留：shtc3 HPWORK 254 阻塞、bt_recv 50% 空转、Settings 其余开关无回调

---

*DevLog by AtomCode (deepseek-v4-flash)*
