# DevLog 2026-08-08 — 音乐控件系列修复 + Beta V0.0.1 版本归档（-16~-19）

## 背景

音乐后台化（-13）与代码审计（-14）落地后，用户上板回归发现一批 UI/交互遗留问题，本会话集中修复并首次建立正式版本号体系。

## 修复链（按固化顺序）

### -16（ok-20260808-16，md5 9eb352c9）：四个问题一批修

**① HOME/锁屏点播放不默认播第一首/历史歌曲**
- 现象：HOME 小组件 / 锁屏音乐控件点播放无反应。
- 根因：`music_play()` 开头 `if (dm_track_count == 0) return;` 直接返回——歌曲列表扫描 `music_scan_tracks()` 只在播放器页打开时执行（create_music_subpage），HOME/锁屏首次点播放时列表从未扫描 → dm_track_count==0；且 `music_track_id` 未从 state 文件恢复（`music_state_load()` 只在播放器页创建时调用）。
- 修复：`music_play()` 在列表为空时补扫一次 + `music_state_load()` 恢复上次歌曲（无记录默认第一首）；`music_album_next()` 同样补扫（HOME/锁屏 ⏮/⏭ 首次点击同样问题）。

**② 背光方向反了（100% 反而最暗）**
- 现象：Settings 亮度滑块 100% 最暗、0% 最亮。
- 根因：`settings_brightness_cb` 中 `pcfg.polarity = PWM_POLARITY_INVERSED` 方向反。
- 修复：改 `PWM_POLARITY_NORMAL`（devlog P9 当时预留结论"若方向反了改 NORMAL"，本次兑现）。

**③ HOME 点播放按钮跳锁屏**
- 现象：在 HOME 点音乐播放按钮会跳到锁屏（standby）。
- 根因：dock 按钮回调 `app_icon_click_cb` 里有 `reset_idle_timer()`，但音乐控制按钮回调（play_pause/prev/next/vol）全都没有——用户 idle 30s 后点播放，idle 计时器到期触发 `show_standby()`，观感像"点播放跳锁屏"。
- 修复：`music_play_pause_cb` / `music_prev_cb` / `music_next_cb` / `music_vol_cb` 均补 `reset_idle_timer()`。

**④ 三 UI 播放控件按钮点击效果统一**
- 现象：播放器页 play 键是"白玻璃+蓝三角 → 蓝紫玻璃+白符号"，但 prev/next/音量± 未同步该点击效果。
- 修复：
  - `music_round_btn`（prev/next/音量±）图标默认色 `COL_TEXT` → `COL_BLUE`（统一蓝符号）。
  - 新增 `music_play_btn_create()`：白玻璃圆+蓝三角，CHECKED/PRESSED 蓝紫渐变玻璃+白符号+蓝光晕；播放器页/HOME/standby 三处 play 按钮全部改用。
  - `music_resume/music_pause` 同步三处 play 按钮的 CHECKED 状态与图标颜色（原来只处理播放器页）。

### -17（ok-20260808-17，md5 876295b1）：上下曲/音量按钮按下符号不变白

- 现象：prev/next/音量± 按下时圆圈变蓝紫玻璃，但符号不变白，与播放键不一致。
- 根因：`music_round_btn` 里给 icon 设 `LV_STATE_PRESSED` 白色样式**不生效**——icon 是按钮的子 label，子对象不继承父按钮的 PRESSED 状态（播放键白符号是靠 music_resume/pause 显式切换，不走状态样式）。
- 修复：新增 `music_round_btn_press_icon_cb`：`LV_EVENT_PRESSED` → 符号白，`LV_EVENT_RELEASED`/`LV_EVENT_PRESS_LOST` → 恢复蓝；`music_round_btn` 移除无效样式，改挂三个事件回调。

### -18（ok-20260808-18，md5 0e2c3e47）：歌单浮层改版

- 现象：播放器歌单"难看"——整行选择条多余（全靠触摸点击）、行距疏、显示效率低。
- 修复（create_music_playlist_overlay）：
  - 去选择条：整行蓝底背景高亮（LV_OPA_10）删除，当前播放歌曲改为**文字蓝色**标记（music_track_click_cb / music_update_track_info 同步改）。
  - 行距紧凑：min_height DM(30)→DM(24)、pad_top/bottom DM(2)→0、去 RAD_CARD 圆角、去 PRESSED transform 放大（保留按下蓝底反馈）。
  - 去分隔线：行间 `settings_add_separator(list)` 调用删除。
  - 卡片加高：DM(180)×DM(230) → DM(180)×DM(360)，一屏多显示约 5-6 首。

### -19（ok-20260808-19，md5 0e02143e）：歌单玻璃效果

- 歌单卡片背景 `LV_OPA_COVER`(100%) → `LV_OPA_80`(80%) 半透明玻璃感，与白色玻璃卡主题统一；阴影/圆角保留，文字可读性不受影响。

## Beta V0.0.1 版本归档（用户决策）

- **里程碑**：音乐后台化 + 系列修复收敛，进入正式版本号管理。
- 备份目录：`/data/vela/releases/Beta-V0.0.1/rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img`（md5 fd057da1c6bbdf68d61152b99cf8eb16）
- 对应固化 tag：ok-20260808-19（nsh.fex == vela.bin，md5 0e02143e）
- **流程变化**：后续每个版本的备份/版本号由**用户决定**并通知，AI 不再自动起版本；AI 只负责固化 tag + 执行用户指定的版本归档。

## 验证

- 每轮均：`./build.sh <config> -j$(nproc)` → `pack` → 确认 `nsh.fex == vela.bin` → `git_snapshot.sh`。
- 本会话产物 md5：-16=9eb352c9、-17=876295b1、-18=0e2c3e47、-19=0e02143e。
- 待上板验证：背光 100% 最亮 / HOME 锁屏点播放直接播第一首或历史歌曲 / 点播放不跳锁屏 / 三 UI 按钮按压统一白符号 / 歌单紧凑无选择条 + 玻璃效果。

## 遗留

- 退出播放器重进点播放卡死（app 层，另案，close_subpage 只 XPlayerStop 不 destroy → g_xplayer 残留）。
- Settings 其余开关无回调 / shtc3 HPWORK 阻塞 / bt_recv 空转（查 devlog.md）。

*DevLog by AtomCode (deepseek-v4-flash)*
