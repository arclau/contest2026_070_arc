# tokens.md — 1920×1200 → 1440×720 缩放 token 表

> 落地位置：`vendor/allwinnertech/apps/luncher_dm/dm_theme.h`
> 布局定稿：`project_docs/specs/ui/home-layout-1440x720.md`（尺寸/配色/字号以此为准）

## 1. 分辨率基准

| token | 值 | 说明 |
|-------|-----|------|
| `DM_DESIGN_W/H` | **720 × 360** | 逻辑画布（新） |
| `DM_SCALE` | **2.0** | 逻辑→物理 |
| `DM_SCREEN_W/H` | **1440 × 720** | 板端物理 |
| `DM(x)` | `(lv_coord_t)((x) * DM_SCALE)` | 唯一缩放宏 |
| 旧 `DM_SCALE` | ~~2.4~~ | 旧逻辑 800×500，废弃 |

自检：`DM_DESIGN_W * DM_SCALE == DM_SCREEN_W`。

## 2. 布局 token（逻辑 → 物理 @2.0）

| token | 逻辑 | 物理 | 旧 1920 值 | 备注 |
|-------|------|------|-----------|------|
| `TOP_BAR_H` | 29 | **58** | ~~132~~ | 状态栏 |
| `COL_W_LEFT` | 186 | **372** | — | 左栏（时间+健康） |
| `COL_W_MID` | 300 | **600** | — | 中栏（问候+入口） |
| `COL_W_RIGHT` | 186 | **372** | — | 右栏（宠物舞台） |
| `COL_GAP` | 11 | **22** | ~~CARD_GAP 48~~ | 栏间距 |
| `RAD_CARD` | 13 | **26** | ~~40~~ | 统一圆角 |
| `STAGE_PAD_Y` | 10 | **20** | — | 舞台上/下 |
| `STAGE_PAD_X` | 13 | **26** | ~~EDGE_PAD 96~~ | 舞台左/右 |
| `CARD_PAD` | 12 | **24** | — | 卡片内边距 |
| `CLOCK_PAD` | 8 | **16** | — | 时钟卡纵向 |
| `HEALTH_RING` | 75 | **150** | — | 健康圈直径 |
| `HEALTH_RING_GAP` | 23 | **46** | — | 两圈间距 |
| `ICON_TILE` | 69 | **138** | ~~DOCK_ICON_SIZE 154~~ | 入口方块 |
| `ICON_TILE_RAD` | 17 | **34** | — | 入口圆角 |
| `ICON_GAP_X` | 6 | **12** | — | 入口列距 |
| `ICON_GAP_Y` | 17 | **34** | ~~DOCK_ICON_GAP 67~~ | 入口行距 |
| `ICON_PAD_B` | 28 | **56** | ~~DOCK_H 240~~ | 入口区底留白 |
| `BATT_W/H` | 27 / 14 | **54 / 27** | — | 电池 |
| `BAR_H` | 7 | **14** | — | 进度条高 |

> `SETTING_ROW_H`：子页设置行按新画布重定，建议逻辑 **~32**（物理 64），以 40cm 触摸 ≥10mm 为准。

## 3. 字号 token（逻辑 → 物理 @2.0）

| token / 用途 | 逻辑 | 物理 | 旧档位 |
|--------------|------|------|--------|
| `FONT_HERO` hero 时钟 | 71 | **142** | 旧 title×scale |
| `FONT_BAR_DATE` 状态栏日期 | 21 | **43** | |
| `FONT_BAR_ICON` 状态栏图标 | 18 | **36** | |
| `FONT_BAR_TEXT` SSID/电量 | 16 | **32** | |
| `FONT_GREET` 问候标题 | 22 | **44** | |
| `FONT_GREET_SUB` 副标题 | 18 | **36** | |
| `FONT_LABEL` 图标标签 | 14 | **28** | |
| `FONT_RING` 圈内数字 | 21 | **42** | |
| `FONT_RING_SUB` 剩余 | 11 | **22** | |
| `FONT_BODY` 卡片正文 | 16 | **32** | **正文下限 32 物理** |
| `FONT_CAPTION` 次要 | 12 | **24** | 宠物进度等 |

字体仍走 `dm_font_*`（MiSans freetype）；仅字号 token 变，加载方式不变。

## 4. 配色（不变，沿用 `deskmate_ui.h` COL_*）

`COL_BG #F2F2F7` · `COL_BG_GRAD #E5E5EC` · `COL_TEXT #1C1C1E` · `COL_SEC #8E8E93`
`COL_BLUE #007AFF`（久坐/WiFi）· `COL_ORANGE #FF9500`（喝水/饱食）· `COL_GREEN #34C759`（电量/心情）
`COL_RED #FF3B30` · `COL_PURPLE #8B5CF6` · 玻璃 `rgba(255,255,255,.72)` → `dm_theme_*` 磨砂 PNG

## 5. 旧常量删除清单（迁完清零）

```
deskmate_ui.h / deskmate_ui.c 中的：
  DM_SCALE 2.4f / DM_DESIGN 800×500 系
  TOP_BAR_H 132 / CARD_GAP 48 / DOCK_ICON_SIZE 154
  DOCK_ICON_GAP 67 / DOCK_H 240 / EDGE_PAD 96
  SETTING_ROW_H 125 / RAD_CARD 40（裸值）
```

## 6. 换屏开关（P2 点亮后）

只改 `dm_theme.h` 分辨率块 → 全页 `DM()` 自动跟。
若逻辑画布变，同步改 tokens 表 + 本文件 + `home-layout-1440x720.md`。
