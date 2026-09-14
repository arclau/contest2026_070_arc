# DevLog 2026-08-08 — 切歌无声：软件侧最终排查 + 硬件验证指导

> 工程：openvela (R528) — nsh 配置
> 工作目录：`/data/dm`
> 前置：`8-7part3devlog.md`（数字链路 100% 排除，共 9 固化点 ok-20260807-11~19）

---

## 一、问题定位回顾（30 秒速读）

**现象**：第一首歌有声，切歌后无声（与歌曲无关）。

**已铁证排除**（不要再查这些方向）：

| 排除项 | 铁证 |
|--------|------|
| 歌曲/文件差异 | 换任何歌首播有声，切歌无声 |
| XPlayer 生命周期 | destroy&recreate 全新对象仍无声 |
| 数据静音/反相 | raw_peak 14746 满幅、mono 混音后仍无声 |
| codec 寄存器差异 | DPC/DAC_ANA/HP_ANA/DAC_REG/FIFOC/FIFOS/CNT/PA/VOL/DG/DAP/DRC 首播 vs 切歌逐位一致 |
| DMA 链路 | writei 全成功 + DAC_CNT 满速增长 |
| XRUN | 已修复（buffer 8192），非无声根因 |
| dm_sound 泄漏 | 已修复（g_music_sound），非无声根因 |

**唯一剩项**：DAC 模拟输出级在"声卡第二次 open"时可能未正确建立（寄存器显示使能但模拟输出为 0）。

---

## 二、今日任务（按优先级排序）

### 任务 A（纯软件，最高优先）：声卡 close→reopen 序列深审

> **核心疑问**：destroy&recreate 走了和首次播放一样的 create 路径，为什么第二次无声？区别只在于声卡硬件经历了一次 open→play→stop→close→reopen 循环。

#### A1. 审查 codec DAPM close→reopen 完整调用链

```
目标文件：nuttx/drivers/audio/sun8iw20-codec.c
关键函数：
  - sunxi_codec_playback_lineout_route（DAPM route on/off）
  - sunxi_codec_hw_free / sunxi_codec_shutdown（close 路径）
  - sunxi_codec_startup / sunxi_codec_hw_params（reopen 路径）
```

**具体操作**：
1. 在 `sunxi_codec_shutdown` 或 `sunxi_codec_hw_free` 中加 dump：
   ```c
   printf("[codec] SHUTDOWN: DAC_ANA=0x%08x HP_ANA=0x%08x LINEOUT=0x%08x\n",
          readl(base + DAC_ANA_REG), readl(base + HP_ANA_REG), readl(base + DAC_REG));
   ```
2. 在 `sunxi_codec_startup` / `sunxi_codec_hw_params` 入口加同样的 dump
3. **对比首次 startup 与第二次 startup 时这三个寄存器是否一致**

> 如果寄存器完全一致 → 寄存器级排除，问题转硬件（任务 B）
> 如果寄存器有差异 → 找到差异位，对照 datasheet 修复

#### A2. 检查模拟偏置建立时序（RAMP/RDEN）

```
在 sun8iw20-codec.c 中搜索：
  - LINEOUTR_RAMP / LINEOUTL_RAMP（渐入斜率）
  - RDEN / RAMP_EN 相关位
  - msleep / mdelay / udelay（偏置建立等待时间）
```

**关键疑点**：首次 open 时 RAMP 从"冷启动"逐级建立偏置（有足够延时），第二次 open 时寄存器可能残留上一次的值导致 RAMP 状态机认为"已建立"而跳过建立过程。

**操作**：
1. 找到 LINEOUT route on 函数中的 RAMP 配置代码
2. 在 route on 之前**强制复位**模拟偏置相关位（先 off 再 on，加延时）：
   ```c
   /* 强制复位 LINEOUT 模拟偏置 */
   snd_codec_update_bits(codec, DAC_REG, (1<<LINEOUTREN)|(1<<LINEOUTLEN), 0);
   msleep(10);
   snd_codec_update_bits(codec, DAC_REG, (1<<LINEOUTREN)|(1<<LINEOUTLEN),
                         (1<<LINEOUTREN)|(1<<LINEOUTLEN));
   msleep(50); /* 偏置建立等待 */
   ```
3. 编译上板测试

#### A3. 尝试"暴力解法"——切歌时彻底 power-cycle codec

在 `dm_sound_destroy`（或切歌路径的 close 点）加入 codec 完全下电→上电序列：

```c
/* 在 close 声卡后、reopen 前 */
// 1. 通过 DAPM 或直接寄存器将 DAC 模拟部分完全断电
// 2. msleep(100) 等电容放电
// 3. 重新 open（走冷启动路径）
```

具体找 `sunxi_codec_trigger` 中 `SNDRV_PCM_TRIGGER_STOP` 分支，确认它是否真正关闭了模拟输出级。

---

### 任务 B（需硬件，与 A 并行）：万用表验证

> 用户有万用表但无示波器。

1. **找到 LINEOUT 引脚**（查原理图或 R528 核心板 PIN 定义）
2. 万用表拨到 **AC V 档**
3. 测试序列：
   - 播第一首歌（有声）→ 读数（应有几百 mV AC）
   - 切歌（无声）→ 读数
   - **如果首播有 AC、切歌无 AC** → 确认 codec 模拟输出级问题 → 执行 A2/A3
   - **如果两者都有 AC** → 问题在功放之后 → 查 GPIOD(17) PA 使能（实测 1.5-1.7V 低于 3.3V 逻辑高，1.8V bank 则正常）

---

### 任务 C（低优先）：诊断打点收敛

当主问题解决后：
- `DM_FORCE_MONO` → 0（恢复立体声）
- corr/RMS/WRITE_GAP 日志降频（每 100 包打一次 → 每 1000 包）
- out_dump 中扩展的寄存器打印保留但加开关（`#ifdef DM_CODEC_DEBUG`）

---

## 三、操作 checklist（deepseek 逐步执行）

```
[ ] 1. 读本文件 + AGENTS.md 第三节了解当前状态
[ ] 2. 执行 A1：审查 shutdown/startup 调用链，加寄存器 dump
[ ] 3. 编译上板（build.sh → pack → 用户刷机测试）
[ ] 4. 根据 A1 结果：
      - 寄存器有差异 → 找差异位修复
      - 寄存器无差异 → 执行 A2（RAMP 强制复位）
[ ] 5. 如果 A2 无效 → 执行 A3（暴力 power-cycle）
[ ] 6. 用户如已做万用表测试 → 根据 B 结果调整方向
[ ] 7. 问题修复后 → 执行 C（诊断收敛）
[ ] 8. 编译打包成功 → bash /data/vela/git_snapshot.sh
[ ] 9. 更新 AGENTS.md 第三节 + devlog.md 节点表
```

---

## 四、关键文件快速索引

| 文件 | 作用 |
|------|------|
| `apps/luncher_dm/deskmate_ui.c` | 播放器逻辑、dm_sound_create/write/destroy |
| `nuttx/drivers/audio/sun8iw20-codec.c` | codec 驱动（DAPM、寄存器操作、trigger） |
| `nuttx/drivers/audio/sun8iw20-codec.h` | 寄存器定义（DAC_ANA_REG、DAC_REG、HP_ANA_REG） |
| `nuttx/drivers/audio/sunxi_snd_pcm.c` | PCM 子系统（open/close/writei/xrun） |

---

## 五、需要用户配合

1. 万用表测 LINEOUT（任务 B）—— **这是最快定性的方法**
2. 刷固件后测试：播第一首 → 切歌 → 报告有声/无声

---

*DevLog by Antigravity (Claude Opus 4.6) · 2026-08-08*
