# UI 设计一致性审计（P272）— 验收清单

目标：从根上止住卡片/按钮边缘打地鼠（P262 圆钮方角、P271 frost 接缝、P271 后健康卡底缘暗边、AI 子页硬编码）。方法：以 dm_theme.h/.c 原语为唯一规范，全页面扫 inline 样式，逐条判定应路由原语还是一次性合法。

平台：共享 LVGL 源码（vendor/allwinnertech/apps/luncher_dm/）。goldfish 验收 + R528 打包。

## 审计基准（dm_theme 原语规范）

| 维度 | 规范值 | 出处 |
|---|---|---|
| 圆角 | 卡/面板/条 RAD_CARD=26；圆钮 LV_RADIUS_CIRCLE；入口 ICON_TILE_RAD=34 | dm_theme.h |
| 阴影 | 对称（offset_x=offset_y=0，无 spread）；色 0x3A4A6B，opa LV_OPA_20；宽 卡 DM(16)/圆钮 6 | dm_theme_card_apply / glass_round_opa |
| 边框 | 玻璃 1px 白（DM(1)）opa 50~70 | glass_*/card_apply |
| 内边距 | CARD_PAD=DM(12)；其余走 DM() | dm_theme.h |
| 玻璃底 | 经 frost_bg_opa/glass_*（bg_color 渐变，不用 bg_img 磨砂） | dm_theme.c P271 |
| 尺寸 | DM() / lv_pct / LV_SIZE_CONTENT；字体 FONT_*_PX 为物理字号豁免 | dm_theme.h |

## 分级结论

- BUG（本次修）：破坏对称阴影根因或明显违背原语。
- 缺口（建原语）：系统缺对应原语，现各页面 ad-hoc，建议补 dm_theme_accent_btn。
- 合法一次性 / 分辨率缩放：设计上合理，仅建议 DM() 化，不改观感。

## BUG — 本次修复

| 位置 | 对象 | 当前 | 应改为 | 根因 |
|---|---|---|---|---|
| deskmate_ui.c:585 | style_card（music 播放列表卡 extern） | shadow_offset_y=DM(3) | 0 | 与 dm_theme_card_apply 旧 bug 同因——半透白卡底缘暗带；原语已修、此副本漏改 |
| deskmate_ui.c:3472 | ai_voice_btn | shadow_spread=4 | 0 | 非对称 halo，圆钮规约禁用 spread |
| deskmate_ui.c:3513 | chat_cont(AI 对话框) | shadow_spread=4 | 0 | 同上 |
| deskmate_ui.c:3549 | ai_chips[i] | radius=16 | RAD_CARD | AI 子页未收口（P271 后漏） |
| deskmate_ui.c:3550 | ai_chips[i] | border_width=2 | 1(DM(1)) | 同上 |
| deskmate_ui.c:3514 | chat_cont | pad_all=12 | DM(12) | 同上 |
| deskmate_ui.c:3504 | chat_cont | width=DM_SCREEN_W*70/100 | lv_pct(100) | 与日历一致的相对宽 |

注：B1 是本轮审计最大收获——卡片阴影不对称被 dm_theme_card_apply 修了，但 style_card 这个平行副本没同步，正是打地鼠的结构性原因。建议长期把 style_card 直接复用 dm_theme_card_apply 语义（列 ROADMAP 重构项，不在本轮范围）。

## 缺口 — 建议补原语（不改观感，仅统一）

系统只有白玻璃原语，缺彩色强调按钮原语。以下均为半透彩底 + 同色边框 + 同色影，属同一组件族却各写一份，应抽 dm_theme_accent_btn(obj, color, opa)：

| 位置 | 对象 | 现状 |
|---|---|---|
| deskmate_ui.c:720-731 | 状态/操作彩钮（红/蓝/绿） | border 2、彩底 opa20~40、彩影 DM(6) |
| ui_alarm.c:248-256 | daily 重复日按钮 | border 3、灰底、蓝边 |
| ui_uart_dbg.c:374-382 | 主题色按钮 | border 2、彩底、彩边 |
| ui_home.c:1475 | standby_ai_btn(按下) | 影 14 |
| ui_music.c:145-166 | 播放/选中等彩钮(按下/选中) | 影 8/14 |

这些不是玻璃违规，是组件族无原语导致的值漂移（边框 2 vs 3、影 6 vs 8 vs 14）。抽原语后数值才能统一收敛。本轮不动（避免改观感与回归），列为设计系统补强项。

## 合法一次性 / 分辨率缩放（仅建议 DM()，不改）

| 位置 | 对象 | 现状 | 建议 |
|---|---|---|---|
| ui_books.c:675/769/780/1463 | 书卡/条影 | 12/8/20/8 | 非对称已 ok；宽建议 DM() 化 |
| ui_calendar.c:911/1205、ui_files.c:472/1594、ui_home.c:533、ui_settings.c:573 | 显式 shadow_width=0 | 关闭阴影 | 合法，无需改 |
| ui_files.c:1099/1496 | 弹窗卡 bg_img_src=NULL+bg_grad_dir=NONE | 实色白卡 | 合法（popup 去渐变）；注：先 glass_rect_opa 又覆写，可简化 |
| ui_music.c:331/332/409/465、ui_pet.c:348、ui_wifi.c:742 | 小内边距 2~12 | 裸字面 | 建议 DM() 化（1440 下正确，仅影响换屏缩放） |
| ui_music.c:414 | 进度点 set_size(dot,6,6) | 裸字面 | DM(3) |
| deskmate_ui.c:1344/1417/1483 | 角标影 4 | 小元素 | 合法；建议 DM() 化 |
| deskmate_ui.c:3660 | sysmon pad 5 | 裸字面 | DM() 化 |

## 验证（2026-10-17）

- B1-B7 已落地 deskmate_ui.c，goldfish 构建通过（ELF 15:48:56，ui_home.c/deskmate_ui.c 重编）。
- 同轮修 home UART 图标不跳转 bug（见 P273），已一并 goldfish 验证：点 UART 磁贴(795,553)
  屏帧整体变化（diff bbox 全帧），确认进入 UART 调试工具子页。
- 待做：bash /data/vela/pack.sh 把 B1-B7 + UART 修复编入 R528 镜像并 check_res（尚未跑）。
- 缺口/合法项不强制本轮改；作设计系统补强跟踪（建议补 dm_theme_accent_btn 原语）。

## 代码边界

本轮动 deskmate_ui.c（style_card 对称、AI 子页收口 B2-B7）+ ui_home.c（B1 同源的
launch_app_after_fade 缺 uart_dbg 分支 → P273）。不动 dm_theme.c（原语已规约）、
不动各 accent 按钮与 pads（留作原语补强）。B1 与 card_apply 同源，长期建议 style_card
直接复用 dm_theme_card_apply，避免再分裂副本。

