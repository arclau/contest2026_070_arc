# P268 — 设置页主从布局（左分类 + 右内容）实现与 goldfish 验收

目标：设置页从双栏独立滚动改为「左分类导航 + 右内容面板」主从布局，右栏先在面板内
显示入口行，点行在右栏内部下钻详情并带面板内返回；视觉清晰、触控可达。
平台：共享 LVGL 源码；goldfish ARM64 编译与运行验收；R528 编译打包与触摸实机待验。

用户本轮授权采用 B 方案（右栏内二级导航，不弹全屏）、保留全套全屏子页复用、
左栏纯文字分类三项决策。

## 实现

- `ui/ui_settings.c` 重写 `ui_settings_create`：左栏 `FILES_RAIL_W`=296px 定宽分类导航
  （7 项：网络/显示与亮度/声音/存储/通用/宠物/关于，项高 DM(36)=72px），右栏吃剩余宽
  1070px；两层容器显式撑高（P267 根因1：flex 不做交叉轴 stretch）。
- 右栏概览（overview）按分类重建分组卡：网络=Wi-Fi/蓝牙开关行；显示与亮度=亮度滑块+
  自动亮度+自动锁定+夜览；声音=音量滑块；存储=内存卡容量+进度条；通用=日期与时间/
  闹钟/城市/UART；宠物=宠物入口；关于=设备信息。
- 详情内联复用全屏子页构建函数 `ui_wifi_create`/`ui_bt_create`/`ui_datetime_create`/
  `ui_alarm_create`/`ui_pet_create`，宿主容器定高可滚动，行为对齐原全屏 content。
- `deskmate_ui.c/.h` 新增 `dm_subpage_build_inline(bool)`；`subpage_big_title` 在内联期间
  返回 NULL，抑制子页大标题（标题由设置页承载）。仅包住一次构建调用。
- 面板内返回条：圆钮 + 分类标题；返回/切分类先 `settings_detail_cleanup()` 清详情浮层
  与定时器，再重建概览。`ui_settings_close_cleanup` 改调 `settings_detail_cleanup()`。
- UART 例外：`ui_uart_dbg_create` 用全屏 `sw/sh` 绝对坐标直接挂 overlay，无法内联，
  该行仍 push 独立全屏子页。
- 左右风格统一（用户反馈左栏裸文字、与右栏 iOS 卡片断层）：
  左栏每项加分类彩色图标，选中态改为**蓝底 10% + 蓝边 40% 圆角选中框**（对齐
  `ui_files.c` 的 `files_location`，改走 `LV_STATE_CHECKED`）；右栏概览顶部加分类标题
  建立层级；弱化右栏行图标徽章光晕（共享 `settings_row_value`/`settings_row_switch_cb`/
  `settings_row_slider`：`shadow_width 8→4`、`shadow_opa 30→10`，影响所有子页）。
- 日期时间内联溢出（截图复核发现）：自动设置区 hint 标签无宽度约束，内联到更窄的
  右栏后按内容撑宽被右缘裁切；改 `ui_datetime.c` hint 为 `lv_pct(100)` + `LONG_WRAP`，
  全屏一行可容纳不触发换行。其它内联子页复核无同类裁切。

## 验证结果

| 项目 | 结果与证据 |
|---|---|
| goldfish 编译 | 正规 `build.sh ...goldfish-arm64-v8a-ap --cmake -j$(nproc)`，`build completed successfully (12 seconds)` |
| 头文件隔离 | 构建前移开 Make 生成头（config.h/arch/math.h/float.h/setjmp.h/builtin_list/proto），构建后全部恢复 |
| 资源 | 构建打印 `Preserved existing userdata`，未重建数据镜像 |
| goldfish 运行 | 已验：主从布局、四分类概览、BT/日期时间/宠物内联详情、宠物起名浮层、UART 全屏例外、切分类清理，均正常 |
| 日期时间裁切 | 内联 hero 时钟完整，P267 根因1 不复现；hint 改换行后右缘不再裁切（[截图](../specs/ui/settings-master-detail/03-detail-datetime.png)） |
| R528 | **未验**：待板端编译打包与触摸实机 |
| 触摸/观感 | **未验**：goldfish 走鼠标模拟，新屏实际触摸待硬件 |

截图（goldfish 帧缓冲抓取，本次构建）见 [预览](#预览)。

## 代码边界

所属仓 `vendor/allwinnertech`，起点 HEAD `20b1e1dc612d2a8788871cd207820267f53324dd`。
本轮修改 `ui/ui_settings.c`、`ui/ui_datetime.c`（内联 hint 裁切）、`deskmate_ui.c`、
`deskmate_ui.h`（徽章光晕；`dm_popup_btn_flex` 见 P269）。开始即有 8 个 dirty 文件
（books/calendar/music/files/theme/pet 等前序会话改动）完整保留，非本轮新增、未扩大其验收。

## 预览

- [网络概览](../specs/ui/settings-master-detail/01-overview-net.png)
- [蓝牙内联详情](../specs/ui/settings-master-detail/02-detail-bluetooth.png)
- [日期与时间内联详情](../specs/ui/settings-master-detail/03-detail-datetime.png)
- [宠物内联详情](../specs/ui/settings-master-detail/04-detail-pet.png)
- [宠物起名浮层](../specs/ui/settings-master-detail/05-pet-name-panel.png)
- [声音概览](../specs/ui/settings-master-detail/06-overview-sound.png)
- [UART 全屏例外](../specs/ui/settings-master-detail/07-uart-fullscreen.png)
- [显示与亮度概览（左右风格统一后）](../specs/ui/settings-master-detail/08-overview-display.png)

## 未闭环 / 下一步

1. R528：`bash /data/vela/pack.sh` 编译打包并核 `check_res`；新屏实际触摸与观感单列硬件验证。
2. 双按钮弹窗溢出（P251 根因2）本轮已修，见 [P269](10-17-two-button-popup-fix.md)。
3. 资源恢复（P266）本轮已达成：活动镜像资源加载正常，用户不满的观感问题未再评估。

## 收尾

- 本轮 opencode session `ses_eb8bf0409ffeTJuNgRK4XtiB6k`（cwd `/data/dm`）。采集由
  `~/.config/opencode/plugin/contest-collector.js` 在 `session.idle` 时写入
  `logs/arclau/<date>/opencode__<session_id>.jsonl`，会话进行中尚未落盘；本次收尾时
  `contest2026_070_arc/logs/` 中无该 session 文件，LOGS 段标 **待采集**（非缺失）。
- 本轮未跑未限定工作区的 `wrap_session.sh`，未执行手工导出，未提交/未 push。
- 源文档与截图已由 `sync_docs.sh` 单向导出到 `contest2026_070_arc/docs`，`--check` 一致。
- `doc_check` 仅余历史告警：`10-14part1devlog.md` 未被 `devlog.md` 索引（V1→V2 过渡遗留，保持原状）。
