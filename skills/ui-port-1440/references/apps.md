# apps.md — 逐子 APP 迁移清单

源码：`vendor/allwinnertech/apps/luncher_dm/`
主屏定稿：`project_docs/specs/ui/home-layout-1440x720.md`

## 迁移顺序与状态

| # | APP | 源文件 | 优先级 | 状态 |
|---|-----|--------|--------|------|
| 0 | shared 弹窗/子页框 | `deskmate_ui.c` `dm_popup_*` `show_subpage` | 先 | ✅ |
| 1 | **home** | `ui/ui_home.c` | **P0** | ✅ |
| 2 | settings | `ui/ui_settings.c` | 高 | ✅ |
| 3 | wifi | `ui/ui_wifi.c` | 高 | ✅ |
| 4 | bt | `ui/ui_bt.c` | 中 | ✅ |
| 5 | music | `ui/ui_music.c` | 中 | ✅ |
| 6 | files | `ui/ui_files.c` | 中 | ✅ |
| 7 | books | `ui/ui_books.c` | 中 | ✅ |
| 8 | calendar | `ui/ui_calendar.c` | 中 | ✅ |
| 9 | datetime | `ui/ui_datetime.c` | 中 | ✅ |
| 10 | alarm | `ui/ui_alarm.c` | 中 | ✅ |
| 11 | uart_dbg | `ui/ui_uart_dbg.c` | 低 | ✅ |
| 12 | pet | `ui/ui_pet.c` + `dm_pet.c` / `pet_view.c` | 后 | ☐ |

---

## 0. shared（deskmate_ui.c）

**先迁**，子页全站受益。

- [ ] `dm_popup_card/icon/title/text/btns` → `dm_theme_card_apply` + token 内距
- [ ] `show_subpage` / 状态栏 `create_status_bar_ex` → `TOP_BAR_H` 新 token
- [ ] 设置行/开关/滑杆 badge：`DM()` token，`RAD_CARD`
- [ ] 返回键/子页 pad_top 用 `TOP_BAR_H + DM(...)`

## 1. home（唯一主屏，按布局稿重排）

**结构（勿丢）**：状态栏 + 三栏（左信息 / 中入口 / 右宠物）

- [ ] **状态栏 58**：日期居中左栏 372；WiFi/BT/电量整屏居中
- [ ] **hero 时钟卡**（新增，左栏）：142px 时钟，玻璃卡
- [ ] **健康双圈卡**（左栏撑满）：久坐蓝+喝水橙，**上下排**，标签在圈右
- [ ] **中栏**：问候钉顶 + **8 入口 4×4 网格**（音乐/日历/书籍/文件/AI/设置/UART/锁屏）
- [ ] **右栏宠物舞台**：整列常驻，名/等级/气泡/饱食/心情；非主角
- [ ] dock 横排 7 图标布局 → 网格；删除旧 DOCK_H 逻辑
- [ ] 暗光/健康弹窗走 shared 弹窗 token
- [ ] 全部玻璃面 `dm_theme_*`；越界断言用 `dm_theme_res_w/h`

> 改 UI 前必读：`create_app_dock` 的 `apps[]`、`create_home_health_card`（配色/顺序）。

## 2. settings

- [ ] 行高 `SETTING_ROW_H` 新 token；分组卡 `dm_theme_card_apply`
- [ ] 开关/滑杆尺寸 `DM()`；ABOUT 显示 `DM_SCREEN_*` + 版本号

## 3–4. wifi / bt

- [ ] 列表容器/头卡 `dm_theme_glass_opa`
- [ ] 弹窗框（密码/配对）shared 弹窗 token
- [ ] 文本输入区保持可读（磨砂高不透明或实底）
- [ ] 扫描/连接按钮 `dm_theme_btn`；触控目标 ≥10mm

## 5. music

- [ ] 播放控件圆钮 `DM()`；进度条 `BAR_H`
- [ ] 播放列表卡 `style_card`/`dm_theme_frost_bg_opa`
- [ ] 封面尺寸按中栏/子页可用区重定，勿写死 1920 值

## 6–7. files / books

- [ ] 列表/面包屑/工具条 `dm_theme_glass_*`
- [ ] 选择操作条（圆形钮）token 间距
- [ ] 阅读器底栏/滑杆 `BAR_H`；书封面 tile `ICON_TILE` 量级

## 8–10. calendar / datetime / alarm

- [ ] 弹窗盒 `dm_theme_frost_bg` + `RAD_CARD`
- [ ] 滚轮/行高 `SETTING_ROW_H` 量级；按钮行 `DM()`
- [ ] 功能项不减（增删/重复/铃声选择保留）

## 11. uart_dbg

- [ ] 端口卡/输入条 `dm_theme_glass_*`；终端区高度按新画布
- [ ] 发送钮 `dm_theme_btn`

## 12. pet

- [ ] 舞台矩形跟右栏 372×~600；PNG 帧 res 管线（P5）另走
- [ ] 名/进度条 `FONT_CAPTION` / `BAR_H`；不抢左栏

---

## 单 APP 迁移笔记模板

```markdown
## <app> — 日期
- 构建函数：<file:line>（整段已读 ☐）
- 结构保持：<页面树/导航确认>
- token 替换：...
- Theme/G2D：...
- pack.sh：☐ md5 ...
- 遗留：...
```
