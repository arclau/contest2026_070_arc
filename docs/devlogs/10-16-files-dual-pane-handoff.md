# P264 — 文件页 1440 双栏改造 · 接续文档

> 已由 [P265 实现与验证](10-16-files-dual-pane-implementation.md) 接续。
> 用户本轮授权调整导航与操作状态；下文为改造前的历史半成品记录，不再作为当前实现状态。

> 目的：本会话定稿布局方案并**改了一半代码**，额度耗尽未构建验收。下一会话按本文直接接手。
> 状态：**半成品，不可交付**。`ui_files_create` 仍是旧竖叠布局，双栏骨架未写入。

## 目标 / 目标平台 / 验收现象

- **目标**：文件页适配 1440×720（2:1）。竖叠「顶栏+快捷+面包屑+列表」把列表压到约 1 行；改为双栏。
- **平台**：goldfish ARM64，LVGL v9，`LV_USE_DRAW_G2D=n` 纯软渲；逻辑画布 720×360（`DM_SCALE=2.0`）。
- **验收**：左栏固定「位置」（内部存储/TF卡，选中高亮）；右栏活动目录列表可滚动；路径在顶栏可点返回层级；长按选择时右下玻璃圆钮坞（正圆无方角）；复制后左栏剪贴板角标出现。

## 用户拍板的布局（2026-10-16，勿再改产品方向）

```
┌──────────────────────────────────────────────────────────┐
│ [←]  文件        内部存储 › music › album         [↑]     │  ← 顶栏 DM(48)
├────────────┬─────────────────────────────────────────────┤
│  位置       │  ┌───────────────────────────────────────┐  │
│ [🏠 内部]   │  │ [●] 青花瓷.mp3               3:24     │  │
│ [💾 TF卡]   │  │ ─────────────────────────────────────  │  │
│ [剪贴板 n]  │  │ ...（现有行样式不变）                   │  │
│            │  └───────────────────────────────────────┘  │
│            │              ◎复制 ◎剪切 ◎粘贴 ◎删 ◎+ ◎×     │  ← 选择坞
└────────────┴─────────────────────────────────────────────┘
  DM(148)      flex 右栏
```

- 路径**并入顶栏**（原独立面包屑层取消，高度还给列表）。
- 左栏只放「位置」+剪贴板角标；**不做** Finder 三栏 column view。
- 选择坞 `LV_ALIGN_BOTTOM_RIGHT`，圆钮一律 `dm_theme_glass_round_opa`。
- 风格：`RAD_CARD` / `FONT_BODY` / `FONT_CAPTION` / iOS 徽章色；平行音乐页「锚点+活动列表+圆钮」。

## 已写入源码（本会话，未编译）

文件：`vendor/allwinnertech/apps/luncher_dm/ui/ui_files.c`（已改，工作区 dirty）

1. 新增 `files_rail_sync()`：左栏两磁贴高亮（`/sdcard` 前缀=TF）+ 剪贴板角标显隐。
2. `files_update_breadcrumb()` / 复制 / 剪切后调用 `files_rail_sync()`。
3. 新静态指针 `files_rail_internal/tf/clip`；`files_ui_clear_ptrs()` 一并置空列表/面包屑/左栏（防野指针）。
4. 选择坞 6 圆钮、图片查看器圆钮、`files_up_btn`：frost→`dm_theme_glass_round_opa`，删 `border_width(0)`（P263 配方）。
5. 选择坞 `lv_align(BOTTOM_RIGHT, -DM(16), -DM(16))`。
6. 面包屑文字 `FONT_BODY`→`FONT_CAPTION`（顶栏单行腾宽）。

## 待做（下一步必做，未写入）

### A. 重写 `ui_files_create()`（仍是旧竖叠，约 L1480 起）

结构（保持全部回调/扫描/剪贴板逻辑不动，只换布局骨架）：

```
parent flex COLUMN, pad_row DM(6)
  app_bar 100%×DM(48) flex ROW: title「文件」 | files_breadcrumb flex_grow | files_up_btn
  body flex_grow=1 flex ROW, pad_column CARD_GAP
    rail DM(148)×100% flex COLUMN: 「位置」CAPTION + 两磁贴 DM(56) + files_rail_clip
    right flex_grow=1 flex COLUMN:
      files_list_scrl flex_grow=1 → files_list_cont（玻璃分组卡，行样式不变）
```

- 磁贴：`lv_btn` + 圆形徽章 DM(28) + `FONT_BODY` 标签；初始底色透明，由 `files_rail_sync` 刷选中态。
- 两磁贴回调沿用 `files_cb_internal_root` / `files_cb_tf_root`。
- `files_breadcrumb` 改为 `app_bar` 子对象（单行、`clip_corner`、`SCROLLBAR_OFF`、非 SCROLLABLE）。
- **删掉**独立 `quick_cont` 与独立整行 `files_breadcrumb` 卡。
- 高度预算：content 可用约 270 逻辑；app_bar 48 + gap 6 → body≈216 → 列表约 3～4 行（`DM(56)` 行高保持触摸目标）。

### B. 构建验收（P263 验证法）

1. `cd /data/dm && ./build.sh vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap --cmake -j$(nproc)`
2. `bash /data/vela/run-emu.sh restart`；等 `f0=0x...`（约 28s）。
3. 注入：`event mouse x y 0 1` + `... 0 0`（单 tap，勿连发）。文件图标 ~(946,350)；进子目录 ~(200,561)；长按 1.3s 选择模式。
4. 截图：`DISPLAY=:99 import -window root /tmp/x.png`；crop 1440×720；放大 3~5× 看圆钮圆角。
5. 验收点：双栏比例、路径可点、左栏高亮切换、选择坞正圆、粘贴角标、返回/重开无野指针。

## 边界 / 风险

- **P263 文件页 frost 诊断**已由本轮 glass_round 改动覆盖（选择坞/上级钮/图片钮）；`ui_files.c` 若仍有 `dm_theme_frost_bg_opa` 仅限矩形卡（列表/顶栏），圆钮不得再用。
- 余下子 APP 同类 site 见 [P263](10-16-subapp-glass-handoff.md) 盘点，不在本轮范围。
- 板端 R528 `pack.sh` 未做；共享代码交付时补。
- 改布局时**整段读完** `ui_files_create` 再动手；禁止散落裸像素，尺寸走 `DM()`/token。
- 改完跑 `workspace_check.py` + `finalize_check.py`；更新 devlog 节点与 ROADMAP（若有里程碑变化）。

## 相关

- 设计语言：`project_docs/specs/ui/home-layout-1440x720.md`、`dm_theme.h`（`glass_round`/`RAD_CARD`）
- 音乐页参照：`ui_music.c` `music_play_btn_create` / 菜单圆钮
- 前序：[P262 音乐圆钮](10-16-music-glass-buttons.md)、[P263 文件 frost 诊断](10-16-subapp-glass-handoff.md)
