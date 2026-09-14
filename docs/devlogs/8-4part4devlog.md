# DevLog 2026-08-04 (P4) — Dock 图标换用 Flyme 高清 + 交互优化 + 一键锁屏

> 工程：openvela (R528) — nsh 配置
> 工作目录：`/data/dm`
> 工具链：SDK 自带 `prebuilts/gcc/linux-x86_64/arm-none-eabi` (GCC 13.4.0)
> 打包流程：`lichee` 下 `source envsetup.sh && lunch_nuttx 2 && pack`，镜像输出至 `lichee/out/r528s3/gemini-s1_nand/`
> **AI 交接：新会话请先读 `AGENTS.md`（环境/命令/进度/已知坑都在那里），本文件是当日详细归档，仅按需查阅。**

---

## 一、任务背景

P3 阶段 dock 图标使用 FontAwesome 字体渲染（`LV_SYMBOL_*`），用户认为"图标丑爆了"。本阶段目标：
1. 从第三方图标包提取真实高清 PNG 图标替换 dock 6 个 APP 图标
2. 解决图标在白色 dock 背景上显示菱角/锯齿的问题
3. 优化点击交互动画，去掉闪烁选中效果
4. 去掉独立锁屏按钮，dock 最右侧新增一键锁屏图标

---

## 二、Flyme 高清图标库提取

### 2.1 图标包 APK 解包

用户提供了两个图标包 APK：

| 文件 | 说明 |
|------|------|
| `/data/dm/补全计划one.apk` | 331 个图标，三星风格图标包 |
| `/data/dm/Flyme高清_1.0.0.apk` | 4066 个图标，Flyme 高清风格 |

最终采用 Flyme 高清图标包，提取完整图标库至 `/data/dm/flyme_icons/`（中文命名 + `.png`，512x512 RGBA）。

### 2.2 APK 资源解析（关键排障）

Flyme 图标包 APK 的资源文件经过混淆：
- 图标文件存放在 `aa/`、`ab/`、... `zz/` 共 678 个单字母目录中
- 文件名被混淆为 `csw`、`ftr`、`nui` 等无扩展名文件（实际是标准 PNG）
- `assets/appfilter.xml` 提供了 `drawable名（如"aiqiyi.icon"）→ 中文应用名` 的映射
- `resources.arsc` 编译资源表提供了 `drawable名 → 混淆文件路径` 的映射

**解析 resources.arsc 的核心难点**：ARSC 是 Android 编译后的二进制资源索引格式，需逐 chunk 解析 `ResTable_header`（magic=0x0002）→ `ResStringPool`（4153 个字符串）→ `ResTable_package`（typeStrings/keyStrings/entryTable）。通过写 Python 脚本解析出 4068 个 drawable 资源的映射关系。

### 2.3 提取结果

- 输出目录：`/data/dm/flyme_icons/`
- 4066 个 512x512 RGBA PNG 图标
- 以中文应用名命名（如 `微信.png`、`支付宝.png`、`Apple Music.png`）
- 总大小约 1.1 GB

---

## 三、Dock 图标更换（三次迭代）

### 3.1 第一次：补全计划one 图标

| APP | 图标文件 | 符号名 |
|-----|---------|--------|
| Music | `yinyue.icon.png` | `dm_icon_music` |
| Games | `youxikongjian.icon.png` | `dm_icon_games` |
| Books | `yuedu.icon.png` | `dm_icon_books` |
| Files | `wenjianguanli.icon.png` | `dm_icon_files` |
| AI | `yuyinzhushou.icon.png` | `dm_icon_ai` |
| Settings | `shezhi.icon.png` | `dm_icon_settings` |

技术实现：226x226 RGBA PNG → 缩放到 154x154（DOCK_ICON_SIZE）→ 转 ARGB8888 C 数组（B,G,R,A 字节序）→ `lv_image` 代替 `lv_label`+`LV_SYMBOL_*` 渲染。新增 `deskmate_icons.c`（3.5MB，6 个图标位图数据）。

### 3.2 第二次：Flyme 高清图标

用户不满意补全计划 one 的图标质量，改用 Flyme 高清图标：

| APP | 图标文件 | 符号名 |
|-----|---------|--------|
| Music | `Apple Music.png` | `dm_icon_music` |
| Games | `Gamepad Space.png` | `dm_icon_games` |
| Books | `阅读.png` | `dm_icon_books` |
| Files | `文件.png` | `dm_icon_files` |
| AI | `小爱同学.png` | `dm_icon_ai` |
| Settings | `设置.png` | `dm_icon_settings` |

### 3.3 第三次：用户微调

用户指定最终方案：

| APP | 图标文件 | 符号名 |
|-----|---------|--------|
| Music | `Apple Music.png` | `dm_icon_music` |
| Games | `游戏中心.png` | `dm_icon_games` |
| Books | `qq阅读.png` | `dm_icon_books` |
| Files | `我的文件.png` | `dm_icon_files` |
| AI | `语音搜索.png` | `dm_icon_ai` |
| Settings | `设置.png` | `dm_icon_settings` |
| Lock | `一键锁屏.png` | `dm_icon_lock`（新增第 7 个） |

---

## 四、Dock 菱角问题解决（三次迭代）

### 4.1 问题根因

图标 PNG 四角有透明像素（圆角图标），但边缘约 44% 像素为半透明。在白色 dock 背景上，半透明边缘与白色背景混合产生锯齿感，视觉上表现为"菱角"。

### 4.2 方案一：深色磨砂 dock + clip_corner

- dock 背景改为深色（`0x1C1C1E`，80% opa）
- 每个图标按钮启用 `lv_obj_set_style_clip_corner(btn, true, 0)` 按 radius=14 圆角裁剪

**结果**：模拟器 LVGL 版本不支持 `clip_corner` API，导致 `munmap_chunk(): invalid pointer` 崩溃。放弃。

### 4.3 方案二：深色磨砂 dock + 按钮同色背景

- dock 背景深色磨砂（`0x1C1C1E`，80% opa）
- 按钮背景改为同色（`0x1C1C1E`，100% opa），遮住图标边缘透明锯齿

**结果**：用户反馈"黑色的背景好难看"，要求改回浅色。

### 4.4 最终方案：浅色磨砂 dock + 按钮同色背景

- dock 背景浅色磨砂（`COL_BG=0xF2F2F7`，60% opa）
- 按钮背景使用与 dock 相同的浅色（`COL_BG`，100% opa）
- 图标透明边缘透出按钮背景，按钮背景与 dock 同色，视觉上无缝融合

**结果**：用户满意，无菱角问题。

---

## 五、点击交互动画优化

### 5.1 原有动画（已删除）

```c
/* 缩放回弹：1.0x → 0.85x → 1.0x (bounce) */
lv_anim_set_values(&a, 256, 218);   /* 缩放因子 */
/* 阴影脉冲：0 → 28 → 0 */
lv_anim_set_values(&s, 0, 28);
```

问题：缩放回弹导致按钮背景在缩小过程中露出 dock 底色，产生"残缺一圈"的视觉效果。

### 5.2 新动画：iOS 风格按压淡出

```c
/* 淡出淡入：255 → 128 → 255 (opacity) */
lv_anim_set_values(&a, 255, 128);   /* 100% → 50% */
lv_anim_set_time(&a, 80);           /* 80ms 淡出 */
lv_anim_set_playback_time(&a, 120); /* 120ms 恢复 */
lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
```

特点：不缩放、无阴影、无边缘残缺，视觉上干净利落。

### 5.3 去掉 focus 呼吸动画

删除了 `focus_cb`、`glow_pulse_cb`、`g_focused_btn` 相关代码：
- 呼吸脉冲：shadow 16→28→16 无限循环（1200ms）
- 按钮的 `LV_STATE_FOCUSED` 高亮样式（蓝色阴影）
- `lv_group_focus_next()` 自动聚焦

保留 `g_group` 键盘导航功能，但无视觉高亮。

---

## 六、锁屏按钮重构

### 6.1 删除旧锁屏按钮

删除位于 dock 上方居中位置的圆形锁屏按钮（`LV_SYMBOL_POWER`，圆形边框，底部居中）。

### 6.2 新增一键锁屏图标

在 dock 最右侧（Settings 右边）新增第 7 个 APP 图标：

```c
{ "lock", "Lock", &dm_icon_lock },
```

点击处理：
```c
else if (strcmp(name, "lock") == 0) show_standby();
```

锁屏画面（`show_standby()`）为全屏遮罩层，显示大时钟 + 日期，点击任意位置唤醒。

---

## 七、文件改动清单

| 文件 | 改动 |
|------|------|
| `apps/luncher_dm/deskmate_icons.c` | 新增（7 个 154x154 ARGB8888 图标位图，~4.1MB） |
| `apps/luncher_dm/deskmate_icons.h` | 新增（7 个 `LV_IMAGE_DECLARE` 声明） |
| `apps/luncher_dm/deskmate_ui.c` | dock 背景浅色磨砂、按钮同色背景、点击动画改为淡出、去掉 focus 呼吸动画、去掉旧锁屏按钮、新增一键锁屏图标 |
| `apps/luncher_dm/Makefile` | CSRCS 加 `deskmate_icons.c` |
| `data/lv_port_linux/CMakeLists.txt` | 加 `deskmate_icons.c` 源文件 |
| `/data/dm/flyme_icons/` | 新增（4066 个 512x512 高清图标库） |
| `/tmp/apk_extract/convert_icons.py` | PNG → ARGB8888 C 数组转换脚本 |

---

## 八、验证结果

```bash
# 每次修改后：
cd /data/dm && ./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh -j$(nproc)
# 编译通过

cd /data/dm/vendor/allwinnertech/lichee
source envsetup.sh && lunch_nuttx 2 && pack
# 打包成功，镜像输出至 lichee/out/.../rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img

# 产物验证
ls -la /data/dm/vendor/allwinnertech/lichee/out/r528s3/gemini-s1_nand/image/nsh.fex /data/dm/nuttx/vela.bin
cmp nsh.fex vela.bin  # 字节一致
```

- 模拟器 lvglsim 重建重启通过（web 预览可交互）
- 板端编译打包 5 次均通过（nsh.fex == vela.bin，5213628 bytes）

---

## 九、遗留事项 / 下一步

1. **上板验证（重点）**：WiFi 联网取真实天气、dock PNG 图标渲染效果、刷新率/内存表现
2. 背景图 `/resource/imgs/luncher_mini_bg_new.png` 为 320x240 旧图，1920x1200 下拉伸且有 PNG 解码告警 → 建议换 1920x1200 横屏图
3. 天气城市坐标写死上海（`dm_weather.c` 的 `DM_W_LAT/LON`），后续可做设置页切换
4. AI 卡仍为占位（"Ready to help"），可换音乐/日程/健康等实用功能卡
5. Flyme 图标库 4066 个图标在 `/data/dm/flyme_icons/`，可直接用于后续替换

---

*DevLog by AtomCode (deepseek-v4-flash)*