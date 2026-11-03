# P263 — 子 APP 圆钮玻璃修复 · 接续文档

> 目的：本会话**只产出本文档（MD）**，未修改任何 `.c`（用户明确要求）。
> 记录“音乐页已交付 / 文件页已诊断待改码”的当前进度，供下一会话 AI 直接接手。

## 目标 / 目标平台 / 验收现象

- **目标**：修复各子 APP 圆形按钮被 `dm_theme_frost_bg_opa` 改成方形磨砂（露四角）+ CIRCLE 阴影平底假边的问题。
- **平台**：goldfish ARM64（openvela trunk，LVGL v9，`LV_USE_DRAW_G2D=n` → 纯软件渲染）。
- **验收现象**：所有圆钮为正圆玻璃（白→浅蓝渐变 + 白边高光 + 对称柔影），放大 3~5× 无方角、无平底；装饰子对象不吞点击（中心点可命中）。

## 已完成（本会话前）：音乐页 = P262

见 [P262](10-16-music-glass-buttons.md)。改动：`dm_theme.c/.h` 新增 `dm_theme_glass_round_opa`；`deskmate_ui.c`（`music_round_btn` 共享 helper、返回键）、`ui_music.c`（播放键、菜单键、播放列表卡）替换 frost→glass；并修菜单键中心点不动 bug（装饰圆点 `lv_obj` 默认可点吞点击）。已构建 + 模拟器截图验收通过。

## 进行中（本会话仅文档）：文件页

**状态：已诊断确认，源码尚未修改。下一会话接手时按下方改法落地并截图验收。**

### 待修 site（ui_files.c）

1. **L531** 选择栏 6 个圆钮 `b`：`dm_theme_frost_bg_opa(b, LV_OPA_80)` + 紧随 `lv_obj_set_style_border_width(b, 0, 0)`（会杀掉玻璃白边高光）。
2. **L1493** 上级圆钮 `files_up_btn`：`dm_theme_frost_bg_opa(files_up_btn, LV_OPA_80)`。
   - 同文件 L505/1493 周围是 `LV_RADIUS_CIRCLE` 圆钮；L974 toast、L1066/1079 cancel/ok 也 CIRCLE，但走别的 style，需核对是否同样误用 frost（本会话未确认，留待接手核对）。

### 根因（与 P262 同源，已在 lv_draw_rect.c 核实）

- LVGL `lv_draw_rect.c`：fill/border/box_shadow 三 task 带 `dsc->radius`（行 180/264/152），但 **bg_img task（行 207/212）不带 radius**（`lv_draw_image_dsc_t` 无 radius 字段）→ 磨砂贴图恒为方形；圆钮 frost 只给 bg_img 且 bg_opa 透明，圆外四角直接露方角（即便叠 20% 磨砂也照样在圆外留方角）。
- `LV_RADIUS_CIRCLE`（0x7FFF）下 LVGL SW 盒阴影不向外扩散；带 `offset_y` 下坠会在圆底留一道“平底”假边（P262 假色实测确认）。

### 改法（精确指引）

对每个待修 site：

```c
/* 旧 */
dm_theme_frost_bg_opa(x, LV_OPA_80);
lv_obj_set_style_border_width(x, 0, 0);   /* ❌ 删：会覆盖玻璃白边高光 */
/* 新 */
dm_theme_glass_round_opa(x, LV_OPA_80);
```

- 圆钮**一律禁用 frost（bg_img）**；改用 `dm_theme_glass_round_opa`（走 fill 渐变，radius 生效）。
- 删掉紧随的 `border_width(0)` 覆盖，让玻璃白边高光保留。
- 阴影必须对称：`shadow_offset_y = 0`、小 width（helper 内已设 6）。**不要**给圆钮加下坠阴影。
- 若 site 后有显式 `shadow_width(x, N, 0)` 覆盖且 offset 仍 0 → 可保留（无平底）；若有 `offset_y != 0` 必须清零。
- 播放/菜单类按下态蓝紫渐变按音乐页同款保留（见 `music_round_btn` / 播放键 PRESSED 样式）。

## 复用配方

`dm_theme_glass_round_opa(obj, opa)`（`dm_theme.c:71`）固定：白→`#D9E6F5` 纵渐变 + 1px 白边 70% + 6px 对称蓝灰柔影 20%。圆钮直接调用即可，无需再设 radius（helper 内已设 `LV_RADIUS_CIRCLE`）。

## 验证方法（goldfish）

1. 构建：`cd /data/dm && ./build.sh vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap --cmake -j$(nproc)`
2. 重启：`bash /data/vela/run-emu.sh restart`；等 ~28s 至 `f0=0x...`（宠物加载）再操作。
3. 触摸注入（端口 5554，token `/home/<user>/.emulator_console_auth_token`）：
   `event mouse <x> <y> 0 1`（落）+ `event mouse <x> <y> 0 0`（起）；单 tap = 单 click（勿连发两次，否则抵消）。
   - 文件页：图标 ~ (946,350)；进子目录 ~ (200,561) 露出上级钮；长按某行 1.3s 进入选择模式（底部 6 圆钮）。
4. 抓屏：`DISPLAY=:99 import -window root /tmp/x.png`；`convert ... -crop 1440x720+0+0` 修边；放大 3~5× 看圆底/四角。
5. 验收：圆钮正圆、无方角、无平底；菜单/选择栏等装饰子对象不吞点击。

## 余下子 APP 同类 site 全量盘点（待各自轮次，本会话未改）

`grep -rn dm_theme_frost_bg_opa` 现存调用（排除 helper 定义与卡片矩形 frost）：

- `ui_files.c:531 / 1493` ← 本任务（文件页）
- `ui_books.c:683 resume / 769 bcard / 1493 bar`（书籍页，待各自核对是否圆钮）
- `ui_home.c:1427 standby_ai_btn`（锁屏迷你播放器，可能圆钮；`music_round_btn` 共享 helper 已修，但此处在 home 自建）
- `ui_calendar.c:890 row` / `ui_alarm.c:366 row`（多为矩形行，frost 安全；仅当对象为 CIRCLE 才需改）
- `deskmate_ui.c:1190 sbar` / `ui_home.c:1302,1339 sbar,w_cont`（状态栏/容器，矩形，frost 安全）

> **判定原则**：仅当对象 `lv_obj_set_style_radius(..., LV_RADIUS_CIRCLE)` 且用了 frost 才需替换；矩形卡用 frost 正常（radius 走 fill/border，frost 仅作背景肌理，不会露方角）。

## 边界 / 风险 / 已知缺口

- 本会话**仅产出该交接文档**，未修改任何 `.c`（用户明确要求）。文件页修复 + 截图验收属下一会话。
- `music_round_btn` 为共享 helper，已修，故 `ui_books` 翻页、`ui_home` 锁屏迷你播放器自动受益（未单独截图）。
- 板端 R528 打包未做（共享代码交付时补 `pack.sh`）。
- `finalize_check` 现状：CODE OK / DOCS 1 项既有 orphan（`10-14part1devlog.md`，历史遗留，非本次引入）/ LOGS MISSING（CodeBuddy 会话采集缺口）。
- 触摸注入方法已存入 memory `reference_emu_interaction.md`。
