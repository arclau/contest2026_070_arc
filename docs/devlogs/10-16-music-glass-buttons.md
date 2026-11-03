# P262 — 音乐子 APP 玻璃圆钮修复（子 APP 逐页第一项）

- 日期：2026-10-16
- 平台：goldfish ARM64 模拟器（1440×720）
- 结论：**音乐页圆钮恢复玻璃质感且无方角；顺带修掉播放列表卡方角与菜单键中心点不动**。构建 + 运行截图双证。
- 前置：P261 里程碑（模拟器运行路径打通）。

## 一、目标与现象

用户反馈（原文）：“那些按钮被改得不像玻璃，而且我们的按钮是圆形的，但是粗糙的表现
方法，把圆形的四个方角都露出来了。”

现象：音乐页 7 个圆形按钮（返回 / 三菜单 / 音量± / 上一首 / 播放 / 下一首）在圆外
露出**方形磨砂贴图的四个直角**；观感发脏，不像玻璃。

## 二、根因（源码级，非“怀疑”）

LVGL 把一次 `lv_draw_rect` 拆成四个绘制 task。读 `apps/graphics/lvgl/lvgl/src/draw/lv_draw_rect.c`：

| task | 是否带 radius |
|---|---|
| box_shadow | 带（行 152 `shadow_dsc->radius = dsc->radius`） |
| fill（bg_color/grad） | 带（行 180） |
| border | 带（行 264） |
| **bg_img（磨砂贴图）** | **不带**（行 207/212 只用 `coords`；`lv_draw_image_dsc_t` 无 radius 字段） |

`dm_theme_frost_bg_opa()` 的实现是 `bg_opa = TRANSP` + `bg_img_src = 磨砂贴图`（tiled）。
于是圆钮上：**没有任何 radius 裁切的填充**，只剩一张**方形**平铺磨砂图 → 圆外四角露出。

回归来源：commit `006bd194`（ok-20261014-2）把 `music_round_btn` 原本的
`bg_color = COL_CARD`（走 fill task，radius 会被裁）改成 `dm_theme_frost_bg_opa()`。
同批还改了播放键、三菜单键、返回键、播放列表卡。

推论：圆钮上**任何** `bg_img` 都不行——即便只叠 20% 磨砂，圆外仍留方角。故圆钮必须
禁用 `bg_img`，改用走 fill task 的 `bg_color` 渐变。

## 三、改动

### 1. 新增材质 API `dm_theme_glass_round_opa()`（`dm_theme.c` / `dm_theme.h`）
圆钮专用玻璃：白→`#D9E6F5` 纵向渐变（走 fill，radius 裁切）+ 1px 白边高光
（70%）+ 小号**对称**柔影 `0x3A4A6B`（20%，`width=6, offset_y=0`）。与 `style_card` /
`dm_theme_card_apply` 同配方，保证圆钮与卡片是同一套玻璃语言。40~52px 钮上磨砂噪点
本就不可辨，去掉无损。

> **柔影必须对称（offset_y=0）**：用户复验指出“播放按钮和音量减少按钮的圆圈底下有问题”。
> 逐层排查（红/绿假色 + 逐行剪影）确认：**填充本身是正圆**（隔离绿色填充为完美圆），
> 问题在盒阴影。CIRCLE 半径（`LV_RADIUS_CIRCLE`）下 LVGL 软件盒阴影不向外扩散、
> 近似被裁在物体范围内；再叠加 `offset_y` 下坠，圆底就出现一道“平底”假边（底部最靠边的
> 钮最明显，如播放键、音量−）。故圆钮一律 `offset_y=0` 且 `width` 取小（6/8）。
> 验证：隔离填充剪影为正圆；`offset_y=0` 后圆底为平滑弧线。

### 2. 四处圆钮调用点替换 `frost → glass_round`
- `deskmate_ui.c` `music_round_btn()`（上一首/下一首/音量±；ui_books.c、ui_home.c 锁屏共用）
- `deskmate_ui.c` `dm_subpage_back_btn_show()`（返回键）
- `ui_music.c` `music_play_btn_create()`（播放键，保留其蓝光晕，同样 `offset_y=0`）
- `ui_music.c` 三菜单键 `menu_btn`
同时删掉各调用点原有的 `shadow_width/border_width=0` 覆盖行（否则会把新加的白边高光清零）。

### 3. 播放列表卡：`dm_theme_frost_bg_opa(card,80)` → `lv_obj_set_style_bg_opa(card,80)`
同批回归把卡片的 `bg_opa` 抹成 TRANSP（白底没了，只剩方形磨砂）。改回只调不透明度，
保留 `style_card` 的白玻璃底 + 20% 微噪点，卡四角恢复圆角干净。

### 4. 三菜单键“中心点不动”bug（顺带发现）
圆点装饰容器 `dots`（`make_clean_cont`，`lv_obj` 默认可点）**正落在按钮正中**。
LVGL v9 事件冒泡是**选择性**的（子对象需带 `LV_OBJ_FLAG_EVENT_BUBBLE`），所以点在
中心圆点上不会冒泡到 `menu_btn` —— 按钮中心反而点不动，只能点边缘。
修：对 `dots` 与三个 `dot` `lv_obj_clear_flag(..., LV_OBJ_FLAG_CLICKABLE)`；
`lv_obj_hit_test` 对非 CLICKABLE 返回 false（`lv_obj_pos.c:929`），hit-test 跳过圆点命中按钮。

## 四、验收证据

- 构建：`./build.sh ...goldfish-arm64-v8a-ap --cmake` 成功（10s 增量，0 error/warning）。
- 运行：`run-emu.sh restart` 后进入音乐页截图。
  - 7 个按钮均**正圆**、白→浅蓝渐变、白边高光、柔影；放大 3× 无方角。
  - **圆底无“平底”假边**：逐行剪影 + 隔离填充（绿）确认为正圆，圆底为平滑弧线。
  - 播放键按下态：蓝紫渐变 + 蓝光晕 + 微放大，仍为正圆（PRESSED 观感未回归）。
  - 播放列表卡：白玻璃圆角，四角干净无方角；scrim 压暗正常。
  - 菜单键**中心点**可开/关播放列表（修复前仅边缘可点）。
- 稳定性：模拟器无 panic/assert（日志中 4 处 “fault/assert” 命中均为 build.prop / QEMU argv / udev 说明文本，非崩溃）。

## 五、边界与待办

- `music_round_btn` / `music_play_btn_create` 为共享 helper，本次一并修复 **ui_books.c（阅读器翻页）、
  ui_home.c（锁屏迷你播放器）**；二者未单独截图验收，属**待验**。
- `ui_files.c:531` 有自己的圆钮实现（未复用 helper），存在**同类方角 bug**，归入**文件子 APP**
  那一轮修（本轮只做音乐）。
- 板端 R528 打包验收未做（共享代码，交付时补 `pack.sh`）。
