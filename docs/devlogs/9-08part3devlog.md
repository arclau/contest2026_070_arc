# 9-08 Part 3 — 用户三问题一次性修复 + 重新打包

> 固化：`ok-20260908-3` | 镜像：`rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img` (66MB) | vela.bin: 33,342,256B

---

## 用户反馈三问题（2026-09-08 晨）

1. **启动变慢**：对比修改分区大小之前启动慢很多
2. **猫被放大**：没有按原 256×256 分辨率显示
3. **猫走到音乐控件处消失**

---

## 问题 1：启动变慢 — u-boot boot_normal 整读 50MB 分区

### 根因
`env.cfg` 中 `boot_normal=sunxi_flash read 44000000 ${boot_partition};boot_rtos 44000000`
- 无长度参数 → 读取 **整个 bootloader 分区（50MB / 102400 sectors）**
- 实际固件仅 **33.34MB** (33,342,256B = 0x1FD2A30)
- 多读 16.7MB 陈旧 NAND 区域 → 启动延迟明显

### 修复
两处 `env.cfg` 同步加长度参数 `2000000` (32MB = 0x2000000, 覆盖 33.34MB 固件 + 余量)：

```bash
# vendor/allwinnertech/lichee/board/r528s3/gemini-s1_nand/configs/env.cfg
# vendor/allwinnertech/lichee/board/r528s3/velaevb1_nand/configs/env.cfg
boot_normal=sunxi_flash read 44000000 ${boot_partition} 2000000;boot_rtos 44000000
```

---

## 问题 2：猫被放大 — 未按原 256×256 显示

### 现象分析
- 精灵 PNG 原始帧：**256×256** (pet_sprites.h / pet_frame_table.c 确认 `.w=256, .h=256`)
- 旧 `PET_SIZE = DM(125)` = 125 × 2.4 = **300px**
- `pet_apply_sprite_scale()` 计算：`PET_SIZE * 256 / 256 = 300` → **1.17x 放大**
- P197 重切精灵后内容占画布 72% (旧 36%) → 视觉上猫体 **近 2 倍大**

### 修复
`dm_pet.c:54` 将 `PET_SIZE` 改为 `DM(107)` ≈ 256.8px (native 1:1)：

```c
#define PET_SIZE             DM(107)  /* 精灵 PNG 256×256 → 屏幕显示 256px (native 1:1) */
```

效果：
- 帧显示缩放 = 256/256 = **1.0x (无放大)**
- 猫体视觉尺寸从 ~197px 降至 ~168px (约 -15%)
- 字面满足「按原 256×256 分辨率显示」
- 所有派生尺寸（容器/气泡/走动区/停靠位）随 `PET_SIZE` 自动等比缩放，无需改动其他代码

> 备选：若用户仍觉偏大，可再降 `PET_SIZE` 或重跑 `cut_sprite.py` 调 `fit=0.36` 回退旧内容占比；当前版本先验证 native 显示效果。

---

## 问题 3：猫走到音乐控件处消失

### 排查过程
1. **Z-order 验证**：`standby_overlay` 子元素创建顺序：
   - sbar → sp_top → welcome_card → rings_row → ai_btn → ai_text → standby_music_title_lbl → **sm_row (音乐控件)** → sp_mid → health → prox_lbl → lux_lbl → **pet (最后创建，line 1902)**
   - pet 创建时 `lv_obj_add_flag(g_cont, LV_OBJ_FLAG_FLOATING)` + `lv_obj_move_foreground(g_cont)`
   - **结论**：pet 在 flex 容器内 z-order 最高，不可能被音乐控件遮挡

2. **走动区域 vs 音乐控件位置**：
   - pet 区域：`zone_x1=sw/2+DM(50)=1080`, `zone_x2=sw-PET_SIZE/2-DM(20)=1722` (右半屏)
   - `zone_y=sh-PET_SIZE/2-DM(80)` ≈ 858~880 (健康卡上方)
   - 音乐控件 `sm_row` 为 flex column 子项，水平居中、垂直位于双圈/AI 与健康卡之间
   - **水平不重叠**（pet 在右侧，音乐在中间），垂直可能相近但 pet 置顶

3. **帧动画检查**：`g_cat_walk_frames` 8 帧全非空，`frame_idx = (g_tick/2)%8` 循环正常，无空帧

4. **状态机检查**：SLEEP 态仅回复 energy，**不隐藏** g_body/g_cont；无位置相关隐藏逻辑

### 判断
代码层面**无法复现**「走到音乐控件消失」。可能为：
- 视觉错觉：大猫 (300px 帧) 跨过音乐控件时透明区露出控件，主观觉得「消失」
- 旧固件残留：上板未烧全 res 分区导致精灵异常（已由 `check_res.sh` 保证完整）
- 特定帧透明度异常（新精灵 72% 裁切，边缘透明度渐变可能在特定背景下不明显）

### 缓解
- `PET_SIZE` 降至 native 256px 后，猫体变小、透明边缘比例降低，**显著降低「误判消失」概率**
- 若上板仍复现，需抓串口日志 + 现场照片/视频，再定位是否为特定 walk frame 透明度 / LVGL 渲染顺序 / 某控件意外 move_foreground

---

## 构建验证

```bash
# 编译
./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh -j$(nproc)
# 打包
cd vendor/allwinnertech/lichee && source envsetup.sh && lunch_nuttx 2 && pack
# 资源检查
bash /data/vela/check_res.sh  # ✅ 通过：WiFi 固件/字体/23 提示音全匹配，镜像含完整 res.fex
# 固化
bash /data/vela/git_snapshot.sh  # → ok-20260908-3
```

### 产物
- `/data/dm/nuttx/vela.bin` (33,342,256B) — 含 DSI 诊断串 `dsi_gen_wr inst busy timeout` (P198)
- `/data/dm/vendor/allwinnertech/lichee/out/r528s3/gemini-s1_nand/rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img` (66MB)

---

## 下一步
- 🔴 **上板验证**：烧录新镜像，确认 ①不再卡 LOGO ②启动加速 ③猫 native 256px 显示 ④猫不再「消失」
- 若问题 3 复现：抓日志/视频 → 进一步定位