---
name: deskmate-ui
description: Desktop Mate UI 设计师 + LVGL 实现技能 — 1920×1200 浅色主题横屏大屏。R528 + NuttX + LVGL 9.x。含浅色设计规范 + Liquid Glass 卡片 + 底部 Dock 布局 + LVGL 符号系统 + 字体策略 + 锁屏/待机 + PC 模拟器工作流。
---

# Desktop Mate UI 设计师 + LVGL 实现技能 (1920×1200 浅色主题版)

## ⚠️ 铁律（违反任意一条自动驳回）

**铁律 1：所有图标必须使用 LVGL 内置符号（`LV_SYMBOL_*`），禁止使用任意 Unicode 字符代替。**
原因：Montserrat 字体中不含 ♫🐾∑ 等字符，运行时必然显示为"□□"。LVGL 内置符号字体（FontAwesome）始终可用，渲染正确。

**铁律 2：每个 `lv_obj_set_pos` 之后必须紧跟边界断言。**
```c
lv_obj_set_pos(obj, x, y);
assert(x + w <= sw && y + h <= sh && "deskmate: 元素越界");
```

**铁律 3：LVGL 不支持的效果，就不做到 UI 设计里。** 写设计稿前，先查下面的 CSS→LVGL 映射表。

**铁律 4：闲置计时器必须被所有交互重置。**
屏幕级的 `LV_EVENT_PRESSED` / `LV_EVENT_KEY` 回调 + 每个交互回调末尾调用 `reset_idle_timer()`。

**铁律 5：锁屏 overlay 必须 `lv_obj_move_foreground()`。**
不调用则 dock 图标从 overlay 底下漏出来。

| 错误做法 | 后果 |
|---------|------|
| HTML 写了 `backdrop-filter: blur(40px)` | LVGL 无实时模糊，用半透明替代 |
| HTML 写了 `radial-gradient()` 多层渐变 | LVGL 不支持运行时画径向渐变，预渲染图片 |
| HTML 写了 `inset box-shadow` | LVGL 不支持内阴影，直接忽略 |
| 用 Unicode 字符当图标（♫🐾∑） | 项目字体不含这些字符，显示"□□" |
| 用 `LV_SYMBOL_WIFI` 但没启用符号字体 | 检查 `lv_conf.h` 中符号字体已包含 |
| 硬编码全局 Y 坐标 | 布局变化后元素悬浮或溢出 |
| 写了 `g_font_24` 但实际没加载 24px 字体 | 运行时崩溃或回退到 16px，UI 变形 |

---

## 🖥️ PC 模拟器开发工作流（必须使用）

**所有 UI 开发调试必须在 PC 模拟器上进行，确认无误后再同步到 NuttX 目标。**

### 工作流
```
① 编辑 /data/lv_port_linux/src/main.c → 改 UI
② cd /data/lv_port_linux/build && make -j$(nproc)
③ 重启 lvglsim → 立刻看效果（http://localhost:9009）
④ 满意后，同步到 apps/examples/deskmate/deskmate.c
⑤ 一次刷机到开发板
```

### 快速重启 lvglsim
```bash
kill $(pgrep -x lvglsim) 2>/dev/null; sleep 0.5
DISPLAY=:99 /data/lv_port_linux/build/bin/lvglsim -b SDL -W 1920 -H 1200 > /data/vela/logs/lvglsim.log 2>&1 &
```

### 开发环境架构
```
浏览器(ws://localhost:9009) → websockify → x11vnc(:5900) → Xvfb(:99) → lvglsim
```

---

## 屏幕规格

| 参数 | 值 |
|------|-----|
| 面板 | **BOE 1200×1920** (MIPI DSI 4-lane) |
| 显示方向 | **1920 × 1200 横屏**（硬件 G2D 旋转 90°） |
| 比例 | 16:10 |
| 帧缓冲 | 32-bit ARGB8888，~9.2MB |
| 硬件加速 | G2D (`CONFIG_LV_USE_DRAW_G2D=y`) |
| LVGL 版本 | 9.x |
| 可用字体 | Montserrat 8/16/48（PC 模拟器额外有 30） |
| 符号字体 | LVGL 内置 FontAwesome（`LV_SYMBOL_*` 系列） |
| 触摸屏 | 支持 (`CONFIG_INPUT_TOUCHSCREEN`) |
| 实体按键 | SW2~SW6 可通过 `/dev/input0` 读取 |

---

## 🧠 设计哲学：高雅 · 简约 · 大气 · 清新

**1920×1200 不是放大的 320×240。** 大屏和小屏的差异是质变，不是量变。

### 设计语言核心

| 维度 | 原则 |
|------|------|
| **风格** | **浅色主题**，`#F2F2F7` → `#E5E5EC` 微渐变背景，类 iOS 风格 |
| **留白** | 元素之间至少 20px 间距，边缘至少 24px 内边距。不堆砌。 |
| **层次** | 状态栏 → Hero 时钟 → 信息卡片（天气/音乐）→ 底部 Dock，四级视觉层次分明 |
| **材质** | **Liquid Glass**：半透明白底 (`bg_opa=60%`) + 半透白边 + 柔和阴影 |
| **色彩** | **浅色调**：白/浅灰底衬托彩色图标。深色文字 `#1C1C1E` / `#3A3A3C`。强调色 `#007AFF`。 |
| **字体** | PC 模拟器用 Montserrat 30/48；NuttX 目标仅可用 16/48。符号用 LVGL 内置符号字体。 |
| **图标** | 一律 `LV_SYMBOL_*`，彩色背景 + 白色符号，不依赖外部字体文件。 |
| **状态栏** | **128px 双行**：上行时钟+天气+日期，下行图标状态。磨砂玻璃效果。 |
| **导航** | **底部 Dock**（7 图标横排居中），非网格布局。键盘焦点 + 触摸兼容。 |

### 大屏 vs 小屏对比

| 维度 | 小屏 (320×240) | 大屏 (1920×1200) |
|------|---------------|-----------------|
| 信息密度 | 一屏只放 1-2 个元素 | 一屏可放 10+ 个元素 |
| 导航 | 列表滚动，逐级深入 | Dock + 信息卡片 + 子页面 overlay |
| 交互 | 点击为主 | 点击 + 键盘焦点 + 快捷键 |
| 布局 | 单列垂直 | 状态栏 → 卡片 → Dock 三层结构 |
| 视觉层次 | 扁平简单 | 丰富层次（阴影、渐变、半透明叠加） |
| 主题 | 深色 | 浅色 + 微渐变 |
| 时钟 | 顶部小时钟 | Hero 大时钟 800×220 卡片 |

---

## CSS → LVGL 映射规则

| CSS 特性 | LVGL 支持 | 替代方案 |
|----------|:---------:|---------|
| `backdrop-filter: blur()` | ❌ | 半透明白色 `bg_opa=60%` + 白边 + 阴影 |
| `radial-gradient()` | ❌ | 预渲染图片，`lv_img` 加载 |
| `inset box-shadow` | ❌ | 忽略 |
| `linear-gradient()` | ✅ | `lv_obj_set_style_bg_grad()` |
| `box-shadow` | ✅ | `lv_obj_set_style_shadow_*()` |
| `border-radius` | ✅ | `lv_obj_set_style_radius()` |
| `opacity` | ✅ | `lv_obj_set_style_opa()` |
| `transform: scale()` | ✅ | `lv_obj_set_style_transform_scale()`（单位 1/256） |
| `flex / grid` | ✅ | LVGL 9 `lv_flex` / `lv_grid` |
| `SVG` | ⚠️ | 预渲染 PNG → `lv_img` |
| `text-shadow` | ❌ | 忽略（可用阴影 label 模拟） |
| `font-awesome 图标` | ✅ | `LV_SYMBOL_*` 宏，内置符号字体 |

---

## 图标策略

### 铁律：一律使用 LVGL 内置符号，禁止 Unicode 字符

LVGL 内置符号字体（FontAwesome）通过 `LV_SYMBOL_*` 宏提供，**始终可用，无需额外加载任何字体文件**。

### Dock 应用图标映射表（7 个）

| # | APP | LV_SYMBOL 宏 | 背景色 | 色值说明 |
|---|-----|-------------|--------|---------|
| 1 | Music | `LV_SYMBOL_AUDIO` | `#0ea5e9` | 天空蓝，冷静 |
| 2 | Games | `LV_SYMBOL_VIDEO` | `#ef4444` | 珊瑚红，激情 |
| 3 | Ebooks | `LV_SYMBOL_FILE` | `#f59e0b` | 琥珀橙，活跃 |
| 4 | Files | `LV_SYMBOL_DIRECTORY` | `#6366f1` | 靛蓝，专业 |
| 5 | Pet | `LV_SYMBOL_BELL` | `#10b981` | 翡翠绿，生机 |
| 6 | AI | `LV_SYMBOL_CALL` | `#8b5cf6` | 星云紫，科技 |
| 7 | Settings | `LV_SYMBOL_SETTINGS` | `#6b7280` | 暖灰，中性 |

### 状态栏图标映射表

| 用途 | LV_SYMBOL 宏 | 说明 |
|------|-------------|------|
| WiFi | `LV_SYMBOL_WIFI` | WiFi 信号 |
| 蓝牙 | `LV_SYMBOL_BLUETOOTH` | 蓝牙 |
| 电池 | `LV_SYMBOL_BATTERY_FULL` / `LV_SYMBOL_BATTERY_3` / `LV_SYMBOL_BATTERY_2` / `LV_SYMBOL_BATTERY_1` / `LV_SYMBOL_BATTERY_EMPTY` | 五级电量 |
| 音量 | `LV_SYMBOL_VOLUME_MID` / `LV_SYMBOL_MUTE` | 音量 |
| 返回 | `LV_SYMBOL_LEFT` | 返回箭头 |
| 设置 | `LV_SYMBOL_SETTINGS` | 齿轮 |
| 主页 | `LV_SYMBOL_HOME` | 房子 |
| 播放 | `LV_SYMBOL_PLAY` | 播放 |
| 暂停 | `LV_SYMBOL_PAUSE` | 暂停 |
| 上一首 | `LV_SYMBOL_PREV` | 上一曲 |
| 下一首 | `LV_SYMBOL_NEXT` | 下一曲 |
| 电源/锁屏 | `LV_SYMBOL_CHARGE` | 闪电（锁屏按钮） |
| 关闭 | `LV_SYMBOL_CLOSE` | 关闭/叉号 |
| 确定 | `LV_SYMBOL_OK` | 勾选 |
| 刷新 | `LV_SYMBOL_REFRESH` | 刷新 |
| 编辑 | `LV_SYMBOL_EDIT` | 编辑 |
| 删除 | `LV_SYMBOL_TRASH` | 垃圾桶 |

### LVGL 代码示例

```c
// ✅ 正确：使用 LV_SYMBOL_* 宏
lv_obj_t *icon = lv_label_create(btn);
lv_label_set_text(icon, LV_SYMBOL_AUDIO);  // 显示为♪
lv_obj_set_style_text_font(icon, FONT_48, 0);

// ❌ 错误：使用 Unicode 字符
lv_label_set_text(icon, "♫");  // 在 Montserrat 中无此字符 → 显示"□□"
```

---

## 布局系统

### 布局常量

```c
/* 布局常量（main.c 同步） */
#define TOP_BAR_H       128     /* 8英寸屏两倍高 */
#define CARD_HEIGHT     100     /* 信息卡片高度 */
#define CARD_GAP         24     /* 卡片间距 */
#define CARD_RADIUS      24     /* 卡片圆角 */
#define DOCK_ICON_SIZE  112     /* 底部图标尺寸 */
#define DOCK_ICON_GAP    64     /* 底部图标间距 */
#define DOCK_H          176     /* 底部栏高度（含标签） */
#define CARD_WIDTH      1200    /* 卡片居中不撑满 */
#define HERO_Y          176     /* Hero 时钟卡片 Y */
#define HERO_H          220     /* Hero 时钟卡片高度 */

/* 字体宏 */
#define FONT_48  (&lv_font_montserrat_48)
#define FONT_30  (&lv_font_montserrat_30)   /* 仅 PC 模拟器可用 */
#define FONT_16  (&lv_font_montserrat_16)
```

**DOCK 布局铁律**：图标 Y 不能用 `(DOCK_H - ICON_SIZE)/2` 居中。必须 `dock_y + 10` 偏上放置，底部留给标签。`DOCK_H ≈ ICON_SIZE + 标签高度 + 上下 padding`。

### 屏幕分区

```
┌──────────────────────────────────────────────────────────────────┐
│ 状态栏 (128px 双行)                                               │
│  12:30  ☀️ 26°C                              Mon 2026-07-23      │
│  [⚡WiFi][Bluetooth][🔔]  [🔋85%]                               │
├──────────────────────────────────────────────────────────────────┤
│                                                                  │
│    ┌─────────────────────────────────────┐                       │
│    │                                     │  ← Hero 时钟          │
│    │           12:30                     │    800×220px          │
│    │        Mon, July 23                 │    Liquid Glass 卡片  │
│    │                                     │    圆角 24px          │
│    └─────────────────────────────────────┘                       │
│                                                                  │
│  ┌──────────────────────────────────────────────┐                │
│  │  ☀️ Sunny  26°/18°  💧 45%  🌬 12km/h       │  ← 天气卡片     │
│  └──────────────────────────────────────────────┘    1200×100px  │
│                                                                  │
│  ┌──────────────────────────────────────────────┐                │
│  │  ♪  Bohemian Rhapsody  ───●──────────  3:45  │  ← 音乐卡片     │
│  │     Queen              ◄‖  ▶‖  ►              │    1200×100px  │
│  └──────────────────────────────────────────────┘                │
│                                                                  │
│  ⚡           [♪♪♪] [▶▶▶] [📁] [📂] [🔔] [📞] [⚙]              │
│  锁屏          Music  Games Ebooks Files  Pet   AI  Settings     │
│  按钮                                          ← 底部 Dock       │
│  64px                                           7 图标           │
│                                               DOCK_H=176px       │
└──────────────────────────────────────────────────────────────────┘
```

### 布局规则

| 区域 | 位置 | 说明 |
|------|------|------|
| **状态栏** | Y=0, H=128px | 双行：上行时钟+天气+日期，下行图标状态。全宽 1920px，磨砂玻璃效果 |
| **Hero 时钟** | Y=176 (HERO_Y), H=220 (HERO_H) | 800×220 居中，Liquid Glass 卡片 |
| **天气卡片** | Hero 下方 24px | 1200×100（居中不撑满），Liquid Glass 卡片 |
| **音乐卡片** | 天气下方 24px | 1200×100（居中不撑满），Liquid Glass 卡片 |
| **底部 Dock** | Y = sh - DOCK_H | 7 图标横排居中，每个 112px，间距 64px |
| **锁屏按钮** | 左下角 (24, sh-64-20) | 64px 圆形，细边框淡色，`LV_SYMBOL_CHARGE` |

### 状态栏布局细节

状态栏是 128px 双行布局，不是简单单行：

```c
/* --- 上行：Y=20, 左右分布 --- */
// 左侧: 48px 时钟（FONT_48, #1C1C1E）
// 右侧: 日期（FONT_30, #8E8E93）

/* --- 下行：Y=76, 左右分布 --- */
// 左侧: 天气图标 + 温度（FONT_30, #3A3A3C）
// 右侧: WiFi + 蓝牙 + 铃铛 + 电池（FONT_30, #3A3A3C）
```

### 验算（1920×1200 横屏）

```
状态栏:         128px
Hero 时钟:      176y + 220h = 396 → 176~396 (间距 48)
天气卡片:       396 + 24 + 100 = 520
音乐卡片:       520 + 24 + 100 = 644
剩余高度:       1200 - 644 - 176(DOCK) = 380px ✅ 充足弹性间距
Dock 可用宽度:  1920 - 24×2 = 1872
7 图标总宽:     7×112 + 6×64 = 1168px ≤ 1872px ✅
```

### 边界断言（必须）

```c
/* 创建元素后立即断言 */
#define DM_ASSERT_BOUNDS(obj, x, y, w, h) \
  do { \
    lv_coord_t _sw = lv_disp_get_hor_res(lv_disp_get_default()); \
    lv_coord_t _sh = lv_disp_get_ver_res(lv_disp_get_default()); \
    assert((x) >= 0 && (y) >= 0); \
    assert((x) + (w) <= _sw && (y) + (h) <= _sh && "deskmate: 越界"); \
  } while(0)

// 使用示例
lv_obj_set_pos(btn, x, y);
DM_ASSERT_BOUNDS(btn, x, y, DOCK_ICON_SIZE, DOCK_ICON_SIZE);
```

---

## 配色系统

高雅、简约、清新。**浅色背景**衬托彩色图标，半透明材质营造层次感。

| 用途 | 色值 | LVGL | 说明 |
|------|------|------|------|
| **主背景** | `#F2F2F7` | `lv_color_hex(0xF2F2F7)` | 浅灰白底，柔和 |
| **主背景渐变** | `#E5E5EC` | `lv_color_hex(0xE5E5EC)` | 微渐变终点，自上而下 |
| **卡片/毛玻璃底** | `#FFFFFF` opa 60% | `lv_color_hex(0xFFFFFF)` + LV_OPA_60 | 半透明白 |
| **主标题文字** | `#1C1C1E` | `lv_color_hex(0x1C1C1E)` | 近黑色，高对比 |
| **正文文字** | `#3A3A3C` | `lv_color_hex(0x3A3A3C)` | 深灰色 |
| **次要文字** | `#8E8E93` | `lv_color_hex(0x8E8E93)` | 中灰色 |
| **强调色** | `#007AFF` | `lv_color_hex(0x007AFF)` | iOS 蓝，焦点高亮 |
| **绿色(OK)** | `#34C759` | `lv_color_hex(0x34C759)` | 电池满/正常 |
| **橙色(警告)** | `#FF9500` | `lv_color_hex(0xFF9500)` | 低电量/注意 |

### Liquid Glass 卡片配方

```c
/* 所有卡片统一配方 */
lv_obj_set_style_bg_color(card, lv_color_hex(0xFFFFFF), 0);
lv_obj_set_style_bg_opa(card, LV_OPA_60, 0);        /* ~60% 半透白 */
lv_obj_set_style_border_width(card, 1, 0);
lv_obj_set_style_border_color(card, lv_color_hex(0xFFFFFF), 0);
lv_obj_set_style_border_opa(card, LV_OPA_50, 0);     /* 半透白边 */
lv_obj_set_style_shadow_width(card, 8, 0);            /* 柔和阴影 */
lv_obj_set_style_shadow_color(card, lv_color_hex(0x000000), 0);
lv_obj_set_style_shadow_opa(card, LV_OPA_10, 0);
```

### Dock 应用图标色板

| APP | 背景色 | 符号 | 色值说明 |
|-----|--------|------|---------|
| Music | `#0ea5e9` | `LV_SYMBOL_AUDIO` | 天空蓝 |
| Games | `#ef4444` | `LV_SYMBOL_VIDEO` | 珊瑚红 |
| Ebooks | `#f59e0b` | `LV_SYMBOL_FILE` | 琥珀橙 |
| Files | `#6366f1` | `LV_SYMBOL_DIRECTORY` | 靛蓝 |
| Pet | `#10b981` | `LV_SYMBOL_BELL` | 翡翠绿 |
| AI | `#8b5cf6` | `LV_SYMBOL_CALL` | 星云紫 |
| Settings | `#6b7280` | `LV_SYMBOL_SETTINGS` | 暖灰 |

---

## 字体策略

**PC 模拟器**可用 Montserrat 字号：12/14/16/18/20/22/24/26/28/30/32/34/36/38/40/42/44/46/48

**NuttX 目标**仅可用：**8/16/48**（在 deskmate.c 中不能使用 `&lv_font_montserrat_30`，会链接错误）

| 需求场景 | 模拟器 (main.c) | NuttX (deskmate.c) |
|---------|----------------|-------------------|
| 状态栏时钟 | `FONT_48` | `FONT_48` |
| 状态栏日期/温度 | `FONT_30` | `FONT_16` 或 `FONT_48` |
| 状态栏图标 | `FONT_30` | `FONT_16` |
| Hero 时钟时间 | `FONT_48` ×2 (transform_scale 512) | 同左 |
| 卡片文字 | `FONT_30` | `FONT_16` |
| 图标标签（Dock 下） | `FONT_30` | `FONT_48`（偏大，但唯一可用大字） |
| 图标符号 | `FONT_48` | `FONT_48` |

**FONT_48 标签宽度参考**：FONT_48 每字符约 28-30px 宽。居中文本的 label 宽度建议 ≥1000px。

**关键理解：** LVGL 的符号字体（`LV_SYMBOL_*`）是独立于 Montserrat 的内置字体。当你用 `lv_label_set_text(icon, LV_SYMBOL_AUDIO)` 时，即使 label 设置的是 Montserrat 字体，LVGL 也会自动 fallback 到符号字体去渲染。

---

## 交互规范

### 焦点高亮

```c
/* Dock 图标焦点：蓝色光环 24px + 放大 1.1x */
lv_obj_set_style_shadow_width(btn, 24, LV_STATE_FOCUSED);
lv_obj_set_style_shadow_color(btn, lv_color_hex(0x007AFF), LV_STATE_FOCUSED);
lv_obj_set_style_shadow_opa(btn, LV_OPA_70, LV_STATE_FOCUSED);
lv_obj_set_style_transform_scale(btn, 282, LV_STATE_FOCUSED);   /* 1.1x (256×1.1=282) */
lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_FOCUSED);

/* 按下反馈：缩小 0.95x */
lv_obj_set_style_transform_scale(btn, 243, LV_STATE_PRESSED);   /* 0.95x (256×0.95=243) */
```

### 按键导航

| 按键 | 功能 | LVGL 实现 |
|------|------|----------|
| UP/SW3 | 上移焦点 | `lv_group_send_data(group, LV_KEY_UP)` |
| DOWN/SW2 | 下移焦点 | `lv_group_send_data(group, LV_KEY_DOWN)` |
| ENTER/SW5 | 确认/启动 | `lv_group_send_data(group, LV_KEY_ENTER)` |
| BACK/SW4 | 返回 | `lv_group_send_data(group, LV_KEY_ESC)` |
| HOME/SW6 | 回桌面 | `lv_group_send_data(group, LV_KEY_HOME)` |

### 闲置定时器（30s 待机）

```c
/* 创建（在 create_deskmate_screen 中） */
idle_countdown = IDLE_TIMEOUT;          /* 30 */
idle_timer = lv_timer_create(idle_timer_cb, 1000, NULL);

/* 必须在所有交互回调末尾调用 */
reset_idle_timer();

/* 屏幕级事件也必须重置（已注册） */
lv_obj_add_event_cb(scr, screen_input_cb, LV_EVENT_PRESSED, NULL);
lv_obj_add_event_cb(scr, screen_input_cb, LV_EVENT_KEY, NULL);
```

### 触摸交互
- 点击 Dock 图标：`LV_EVENT_CLICKED` → 打日志（未来启动子页面）
- 点击锁屏按钮：`LV_EVENT_CLICKED` → `show_standby()`
- 触摸兼容按键导航，两者不冲突

---

## 锁屏 / 待机（Standby）

### 触发条件
| 事件 | 行为 |
|------|------|
| 无操作 30s | 自动进入待机（`idle_timer_cb` → `show_standby()`） |
| 点击左下角 ⚡ 按钮 | 立即进入待机（`lock_btn_click_cb` → `show_standby()`） |
| 待机中点击/按键 | 返回主页（`wake_from_standby()`） |

### 锁屏 UI 设计
```
┌──────────────────────────────────────────────────────────────────┐
│  ⚪ 520px 光环圆（半透明白边，旋转动画）                         │
│                                                                  │
│      ┌──────────────────┐                                       │
│      │                  │                                       │
│      │     12:30        │  ← 时钟 FONT_48 ×2（transform_scale 512）│
│      │                  │     等效 96px                          │
│      │  Mon, July 23    │  ← 日期 FONT_30                       │
│      │                  │                                       │
│      └──────────────────┘                                       │
│                                                                  │
│  ⚪ 光环旋转动画（arc_opa 渐变动画）                             │
└──────────────────────────────────────────────────────────────────┘
```

**实现关键：**

```c
/* 创建 overlay 覆盖全屏 */
lv_obj_t *overlay = lv_obj_create(lv_scr_act());  // ✅ 正确：在 active screen 上建覆盖层
lv_obj_set_size(overlay, sw, sh);
lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
lv_obj_move_foreground(overlay);  // ⚠️ 铁律：必须 move_foreground

/* 时钟 2x 放大 */
lv_obj_set_style_transform_scale(clock_label, 512, 0);  // 256=1x, 512=2x

/* 光环动画 — 必须用包装函数，禁止类型强转 */
static void arc_opa_anim_cb(void *obj, int32_t v) {
    lv_obj_set_style_arc_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}
```

---

## 子页面设计原则

**大屏子页面不同于小屏的"全屏切换"模式。** 1920×1200 的子页面应：

1. **保留状态栏**：顶部状态栏始终可见
2. **overlay 覆盖**：在 active screen 上创建覆盖层，非独立 screen
3. **左上角返回按钮**：`LV_SYMBOL_LEFT` + 标题
4. **充分利用宽度**：多列布局，避免单列窄条
5. **内容区域**：Y 从状态栏底部开始，到 Dock 上方为止

### 子页面框架实现

```c
static lv_obj_t *g_subpage_overlay = NULL;

static void show_subpage(const char *title)
{
  if (g_subpage_overlay) return;  /* 已打开 */
  lv_coord_t sw = lv_disp_get_hor_res(lv_disp_get_default());
  lv_coord_t sh = lv_disp_get_ver_res(lv_disp_get_default());

  g_subpage_overlay = lv_obj_create(lv_scr_act());
  lv_obj_set_size(g_subpage_overlay, sw, sh);
  lv_obj_set_style_bg_color(g_subpage_overlay, lv_color_hex(0xF2F2F7), 0);
  lv_obj_set_style_bg_opa(g_subpage_overlay, LV_OPA_COVER, 0);

  /* 返回按钮 */
  lv_obj_t *back_btn = lv_btn_create(g_subpage_overlay);
  lv_obj_set_size(back_btn, 80, 80);
  lv_obj_set_pos(back_btn, 16, 16);
  lv_obj_set_style_radius(back_btn, LV_RADIUS_CIRCLE, 0);
  lv_obj_add_event_cb(back_btn, subpage_back_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *back_icon = lv_label_create(back_btn);
  lv_label_set_text(back_icon, LV_SYMBOL_LEFT);
  lv_obj_set_style_text_font(back_icon, FONT_48, 0);
  lv_obj_center(back_icon);

  /* 标题 */
  lv_obj_t *title_lbl = lv_label_create(g_subpage_overlay);
  lv_label_set_text(title_lbl, title);
  lv_obj_set_style_text_font(title_lbl, FONT_48, 0);
  lv_obj_set_style_text_color(title_lbl, lv_color_hex(0x1C1C1E), 0);
  lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 32);
}

static void close_subpage(void)
{
  if (g_subpage_overlay) {
    lv_obj_del(g_subpage_overlay);
    g_subpage_overlay = NULL;
  }
}
```

---

## 音乐播放控件规范

```c
/* ❌ 裸 label 当按钮 — 无质感 */
/* ✅ 圆形 lv_btn + LV_RADIUS_CIRCLE */
lv_obj_t *btn = lv_btn_create(parent);
lv_obj_set_size(btn, 52, 52);
lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
// 两侧：bg_opa=10%, 蓝色图标
// 中间播放键：bg_opa=COVER, 64px, 白色图标
```

---

## LVGL 可用/不可用能力

| ✅ 可用 | ❌ 不可用（原因） |
|---------|-----------------|
| 两色线性渐变 | SVG (ThorVG 未启用) |
| 半透明叠加模拟毛玻璃 | Lottie 动画 |
| 圆角 + 阴影 | 多色复杂渐变 |
| 弹性动画 `lv_anim_path_bounce` | 实时模糊 |
| `LV_SYMBOL_*` 55+ 内置图标 | FreeType (编译依赖问题) |
| PNG/GIF 图片 | extra 内置字体（只有 16/48 已启用） |
| 暗色主题 | G2D 硬件加速 |
| `transform_scale` 放大（单位 1/256） | |

---

## ⚠️ 红牌警告（会崩溃，必须遵守）

### 1. 禁止 `lv_obj_create(NULL)` + `lv_scr_load()`

```c
// ❌ 崩溃：独立屏幕无主题样式，子对象 style_cnt==0
scr = lv_obj_create(NULL);
lv_scr_load(scr);
lv_label_create(scr);  // → get_local_style assertion failed!

// ✅ 正确：在 active screen 上建全屏覆盖层
overlay = lv_obj_create(lv_scr_act());
```

### 2. 禁止动画回调类型强转

```c
// ❌ 崩溃：lv_obj_set_style_arc_opa 有 3 个参数，动画回调只传 2 个
lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_style_arc_opa);

// ✅ 正确：用包装函数
static void arc_opa_anim_cb(void *obj, int32_t v) {
    lv_obj_set_style_arc_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}
lv_anim_set_exec_cb(&a, arc_opa_anim_cb);
```

### 3. 禁止 `(lv_timer_cb_t)lv_obj_del` 类型强转

定时器回调参数不匹配 → 崩溃。必须用包装函数。

### 4. 禁止图标用 Unicode 字符

```c
// ❌ lv_label_set_text(icon, "♫");  → 显示 "□□"
// ✅ lv_label_set_text(icon, LV_SYMBOL_AUDIO);
```

### 5. 禁止 LVGL 线程中 sleep()/usleep()

阻塞 UI 刷新。用 `lv_timer` 代替。

### 6. 闲置计时器必须被所有交互重置

```c
// ❌ 只靠 idle_timer_cb 递减 → 用户点图标/按键后仍被踢到待机
// ✅ 屏幕级 PRESSED/KEY 回调 + 每个交互回调都调用 reset_idle_timer()
```

### 7. static 函数交叉调用需前向声明

```c
// ❌ static void B() 调用 static void A() — A 定义在 B 后面 → 编译错误
// ❌ "static declaration follows non-static declaration"
// ✅ 在文件顶部 forward declarations 区添加：
static void reset_idle_timer(void);
```

### 8. `lv_obj_set_style_transform_scale_*` 单位是 1/256，不是百分比

```c
// ❌ 以为 150 = 1.5x → 实际 150/256 = 0.59x 缩小！
// ✅ 正确：256 = 1.0x，384 = 1.5x，512 = 2.0x
lv_obj_set_style_transform_scale(label, 512, 0);  // 2x 放大
```

### 9. `LV_OPA_*` 只有 10 的整数倍

```c
// ❌ LV_OPA_5, LV_OPA_8, LV_OPA_15 — 全都不存在
// ✅ 可用：LV_OPA_0, LV_OPA_10, LV_OPA_20, ... LV_OPA_100
```

### 10. 锁屏 overlay 必须 `lv_obj_move_foreground()`

```c
// ❌ 不调用 → dock 图标从 overlay 底下漏出来
// ✅ 创建 standby_overlay 后立即：
lv_obj_move_foreground(standby_overlay);
```

---

## ⚠️ LVGL 落地强制规则

### 规则 1：只使用项目可用的字体

PC 模拟器可用 FONT_48 / FONT_30 / FONT_16。NuttX 仅可用 FONT_48 / FONT_16（**无 30px**）。不要声明 `g_font_24` 等不存在的变量。

### 规则 2：布局自适应 + 边界断言

```c
lv_coord_t sw = lv_disp_get_hor_res(lv_disp_get_default());
lv_coord_t sh = lv_disp_get_ver_res(lv_disp_get_default());
assert(x + w <= sw && y + h <= sh);
```

### 规则 3：图标使用 LVGL 内置符号

```c
// ✅ 正确
lv_label_set_text(icon, LV_SYMBOL_AUDIO);
lv_label_set_text(icon, LV_SYMBOL_WIFI);
lv_label_set_text(icon, LV_SYMBOL_SETTINGS);

// ❌ 错误
lv_label_set_text(icon, "♫");
lv_label_set_text(icon, "🐾");
```

### 规则 4：按键导航是主要交互

```c
g_group = lv_group_create();
lv_group_set_wrap(g_group, true);
lv_group_set_focus_cb(g_group, focus_cb);
lv_indev_set_group(g_keypad_indev, g_group);
```

### 规则 5：子页面用 overlay 而非独立 screen

```c
g_subpage_overlay = lv_obj_create(lv_scr_act());
lv_obj_set_size(g_subpage_overlay, sw, sh);
// 不要用 lv_scr_load()
```

---

## 涉及文件

| 文件 | 行数 | 作用 | 何时改 |
|------|------|------|--------|
| `/data/lv_port_linux/src/main.c` | 912 | PC 模拟器源码，**主力开发文件** | 每次 UI 改动 |
| `/data/lv_port_linux/build/bin/lvglsim` | — | 编译产物 | make 后自动生成 |
| `/data/openvela/apps/examples/deskmate/deskmate.c` | 985 | NuttX 目标源码 | main.c 验证后同步 |
| `/data/vela/start-deskmate-web.sh` | 70 | Web 预览启动脚本 | 不改 |
| `/data/vela/vela-dev.sh` | 118 | 开发模式（自动重载） | 不改 |
| `/data/vela/devlog.md` | — | 开发日志 | 每次 session 更新 |

### 两文件同步对照

| main.c 函数 | deskmate.c 函数 | 差异 |
|------------|----------------|------|
| `create_status_bar` | `create_top_bar` | 函数名不同，内容同步 |
| `clock_update_cb` | `clock_update_cb` | 相同 |
| `focus_cb` | `focus_cb` | 相同 |
| `create_big_clock` | `create_big_clock` | 相同 |
| `create_weather_card` | `create_weather_card` | 相同 |
| `create_music_card` | `create_music_card` | 相同 |
| `create_app_dock` | `create_app_dock` | 相同 |
| `show_standby` | `show_standby` | 相同 |
| `create_deskmate_screen` | `create_deskmate_screen` | 相同 |

**同步注意事项：**
- `FONT_30` 仅 main.c 有定义 → 同步到 deskmate.c 时改为 `FONT_48` 或 `FONT_16`
- `lv_group_set_edge_cb` 仅存在于 deskmate.c（NuttX 特有），不要删除
- deskmate.c 的 NuttX 特有区域不可改动：`button_poll_cb`、`g_btn_fd`、`edge_cb`、`lv_nuttx_init`、`boardctl`、`main`

---

## ✅ 自检清单（20 项）

| # | 检查项 | 说明 | 验证方式 |
|---|--------|------|---------|
| 1 | 字体使用正确 | NuttX 只用 8/16/48，无 `FONT_30` 或自定义字体变量 | 全文搜索 `font_montserrat_30` 在 deskmate.c |
| 2 | 布局自适应 | 使用 `lv_disp_get_hor_res()` 动态获取尺寸 | 代码审查 |
| 3 | **无 Unicode 图标** | 全文搜索 `"♫\|"🐾\|"∑\|"木` 确认无使用 | `grep -n '"♫\|"🐾\|"∑\|"木' *.c` |
| 4 | **使用 LV_SYMBOL_*** | 所有图标使用 `LV_SYMBOL_*` 宏 | `grep -n 'LV_SYMBOL_' *.c` |
| 5 | 无硬编码绝对坐标 | 无 `lv_obj_set_pos.*,\d{4,}` 或 Y>1200 的硬编码 | 代码审查 |
| 6 | 按键导航就绪 | `lv_group` + `lv_indev_set_group` 已实现 | 代码审查 |
| 7 | 边界断言就绪 | 每个 `lv_obj_set_pos` 后有 `assert` 边界检查 | `grep -A2 'lv_obj_set_pos' *.c` |
| 8 | 图标 dock 而非列表/网格 | 主界面是 dock 布局（7 图标横排），非垂直滚轮列表 | 代码审查 |
| 9 | 状态栏 128px 双行 | 状态栏高度 128px，双行布局 | 检查 TOP_BAR_H |
| 10 | Dock 布局铁律遵守 | 图标 Y = dock_y + 10，非居中 | 代码审查 |
| 11 | 闲置定时器重置 | 屏幕级 PRESSED/KEY + 每个回调调用 reset_idle_timer | 代码审查 |
| 12 | 锁屏 overlay foreground | show_standby 中调用 `lv_obj_move_foreground` | 代码审查 |
| 13 | 锁屏不使用 lv_obj_create(NULL) | 使用 `lv_scr_act()` 建 overlay | 代码审查 |
| 14 | 动画回调用包装函数 | 无 `(lv_anim_exec_xcb_t)` 类型强转 | 代码审查 |
| 15 | LV_OPA_* 使用正确 | 只使用 LV_OPA_0/10/20/…/100，不在中间的 | 代码审查 |
| 16 | transform_scale 单位正确 | 使用 256=1x, 512=2x 等 1/256 单位 | 代码审查 |
| 17 | 子页面用 overlay | 使用 `lv_obj_create(lv_scr_act())` 非 `lv_scr_load` | 代码审查 |
| 18 | Liquid Glass 配方一致 | 卡片使用统一的半透明白+白边+阴影配方 | 代码审查 |
| 19 | 在模拟器验证过 | 已通过 lvglsim 编译+运行测试 | 运行确认 |
| 20 | 无编译警告 | `make` 无 warning | 编译确认 |

---

## 当前状态（v3.3，2026-07-23）

### 已实现

- ✅ 浅色背景 `#F2F2F7` + 微渐变 `→ #E5E5EC`
- ✅ 状态栏 128px 磨砂玻璃，双行布局（时钟+天气 | 日期）
- ✅ Hero 时钟 Liquid Glass 卡片 800×220
- ✅ 天气/音乐卡片 Liquid Glass 1200×100（居中不撑满）
- ✅ 底部应用栏 7 个图标（112px，FONT_30 标签）
- ✅ 焦点高亮：`LV_STATE_FOCUSED` 蓝影 24px + 放大 1.1x
- ✅ 锁屏：520px 光环 + 2x 缩放时钟（等效 96px）+ FONT_30 日期
- ✅ 音乐卡片：圆形 lv_btn 播放控件（52/64px）
- ✅ 子页面框架：overlay + 返回按钮 + 标题
- ✅ 左下角闪电按钮一键锁屏

### 已知限制

- **FreeType 不可用**：PC 端 CMake/libevdev 依赖问题 + NuttX 端系统头文件不兼容 → 全部用内置位图字体
- **时钟最大 48px**：但可通过 `transform_scale` 2x 放大到等效 96px
- **NuttX 无 30px 字体**：deskmate.c 里 `&lv_font_montserrat_30` 会导致链接错误，需改用 48 或 16
- **应用图标点击只打日志**：未实现子页面启动
- **模拟器无输入设备**：`indev_read_cb is not registered` 是正常警告
- **CMakeCache.txt 不可删除**：删除后 CMake 重配置会因 libevdev 缺失失败

### TODO

- [ ] FreeType 矢量字体（PC 模拟器 CMake/libevdev + NuttX 内部头文件）
- [ ] 应用图标点击启动子页面
- [ ] 天气卡片对接真实数据
- [ ] 音乐卡片对接播放器
- [ ] 健康提醒计时器（久坐/喝水/护眼）
- [ ] 锁屏健康卡片数据实时更新

---

## 静默升级说明

本次重写（v4.0）相比旧版（深色主题 v2.0）的关键变更：

| 变更项 | 旧版（深色） | 新版（浅色） |
|-------|-------------|-------------|
| 主题 | 深色 `#0f172a` | 浅色 `#F2F2F7` → `#E5E5EC` |
| 应用数 | 11 个网格布局 | 7 个 Dock 布局 |
| 状态栏 | 64px 单行 | 128px 双行 |
| 时钟 | 360×150 小组件 | 800×220 Hero 卡片 |
| 布局 | 图标网格 | 状态栏 → 卡片 → Dock |
| 配色 | `#1e293b` 深色状态栏 | 磨砂玻璃半透明白 |
| 文字颜色 | 白色/浅灰 | `#1C1C1E` / `#3A3A3C` / `#8E8E93` |
| 焦点高亮 | 天空蓝 `#38bdf8` | iOS 蓝 `#007AFF` |
| 锁屏 | 简单深色 | 520px 光环 + 2x 动画时钟 |
| 锁屏按钮 | 无 | 左下角 ⚡ 按钮 |
| 红牌警告 | 无 | 10 条铁律 |
