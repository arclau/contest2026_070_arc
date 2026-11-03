# P275 · 子 APP 设计语言与 UART 修复 · 中途交接

日期：2026-10-18。状态：**WIP，用户因 5 小时额度剩余约 26% 要求先交接；停止继续实现。**
下一位 AI 先读本文件，按第 7 节续接；不要重新从头全量读取历史，也不要把旧 P274 包当成本轮交付。

> **2026-10-19 更新：本文件第 5 节“UART 键盘回车事件与字形”已完成**，vendor `4c122d89` 已提交，
> 离屏双套与 goldfish 实跑均通过；R528 包也包含该提交。请改看 [P275 源码盘查](10-19-p275-source-inventory.md)，
> 当前剩余项是设置右栏定稿、真实内联详情、r528 镜像同步和板测。

## 1. 用户已确认的需求与边界

- 用户最满意的子 APP 只有音乐播放器、日历；文件浏览器一般，尤其右侧文件项风格别扭。
- 设置右侧子项像奇怪的玻璃块。UART 最差：元素冲突、功能缺失，发送命令不能用。
- 用户明确允许移除 UART 的「注入」「暂停」按钮。
- 先讨论整个 APP 的设计语言；随后明确：**主屏不动，保留玻璃；子 APP 可以优化，兼顾效率与资源消耗。**
- 音乐、日历作为布局和信息层级参考，本轮没有修改二者，也没有修改主屏源码。
- 最后一条指令是把计划和想法写入最新 devlog，留给下一位 AI；本轮不继续构建或补代码。

目标平台：共享 LVGL UI，1440×720，goldfish ARM64 与 R528 Gemini-S1 共用。
验收现象：文件列表清楚、设置分组平整、UART 日志/输入/键盘不冲突；发送结果真实可见。

## 2. 设计思路（已与用户讨论，作为续接依据）

整体方向：轻盈、清楚的桌面工具。统一字体、间距、颜色语义、操作态和导航；不要求所有页面都是卡片。

| 层级 | 做法 |
|---|---|
| 主屏 | 保持现有玻璃、主题函数和观感 |
| 导航/浮层 | 可适量保留玻璃；内容区不再反复套玻璃 |
| 工具内容面 | 平整实色，无渐变、阴影；用留白、分组标题建立层级 |
| 文件/设备列表 | 普通行、类型图标、细分隔；只在按下/选中时显色 |
| 设置 | 分组表单，名称左、值或控件右；去彩色图标块和组卡阴影 |
| UART | 参数栏 → 日志 → 状态反馈 → 宽命令行；键盘在同一布局占位 |
| 性能 | 先减少实际冗余对象/渲染工作、重复文字刷新；不为性能关闭日志、检查器或断言 |

新增材质必须显式采用，不能全局替换旧玻璃函数，否则会改变主屏和用户满意页面。
旧 P272 主要审计「是否使用同一主题函数」，不足以证明设计合理；同样的玻璃函数堆满页面仍会乱。

## 3. 本轮已经写入的代码（尚未最终交付）

唯一应用源：`vendor/allwinnertech/apps/luncher_dm/`。

> **2026-10-27 校正**：V2 实际构建 `apps/luncher_dm_v2/`，与 `luncher_dm` 是两棵独立树；本文“唯一应用源”的结论对 V2 不成立，保留原文仅作历史。
进入本轮时 vendor 仓已有大量未提交改动；HEAD 为 `20b1e1dc612d2a8788871cd207820267f53324dd`。
**不能用 git reset/checkout 整文件回退，会丢掉之前用户工作。**
本轮开始备份：`/tmp/dm-ui-p275/baseline/`，含有关原文件及 `vendor.patch`；临时目录可能被清理。

| 文件（相对应用源） | 本轮改动 |
|---|---|
| `dm_theme.h/.c` | 新增平整内容面/列表行/操作按钮：`dm_theme_surface`、`dm_theme_list_row`、`dm_theme_action`；新 token 独立，原玻璃 API 不改 |
| `ui/ui_files.c` | 右侧实色底；行 112→88px；去图标徽章容器，去独立分隔对象，改行底边；选中访问索引同步从 `i*2` 改 `i`；文件名显式 flex 宽度 0 |
| `deskmate_ui.c` | `settings_group_card` 去玻璃/阴影；三类 `settings_row_*` 去徽章容器、行高 88px；无箭头只读值行取消点击；未改全局 `style_card`、共享键盘或导航生命周期 |
| `ui/ui_settings.c` | 右面板实色+内边距；相同分类重复点击不重建；内联详情返回触摸区 32→64px |
| `ui/ui_uart_dbg.c` | 重写为顶部参数、全宽日志、状态、底部输入；删除 UI 注入/暂停；ASCII/HEX、三主题、清屏、重连、换行选项 CRLF/LF/CR/无换行 |
| 同上 | 键盘首次点击输入时创建，和日志/输入共处 flex 列；发送采用非阻塞短写偏移与 2 秒超时，失败保留命令、显示已写字节数；切波特率失败回滚 |
| 同上 | RX 每 100ms 最多消费 2048B；日志静态 6000B 缓冲供 label 引用；状态文本变化才 set_text；用户上滚时不强制滚底 |
| `dm_uart_dbg.c/.h` | `write` 无设备时设 ENODEV，非法参数设 EINVAL；补 stdint include、修正只读的过期接口说明；未重构接收线程/环形缓冲 |
| `tests/files_offscreen/` | 原测试漏提取 `dm_popup_btn_flex` 导致链接失败，已补；适配新行索引与行高 |
| `tests/tools_offscreen/` | 新增真实 LVGL + 真实 UART/设置页面的离屏验证；仅平台/串口服务替身；尚无 README |

当前 UART 也删掉了原独立「回车」按钮（用户只明确要求删注入/暂停）。换行后缀已支持，但空输入不发送。
**接手时审视这个产品行为：若需保留单发回车，可放回轻量操作，不要静默认为用户取消了这项功能。**

## 4. 已得到的证据与边界

| 验证项 | 结果 / 证据 |
|---|---|
| 文件离屏交互 | 通过：多选、长按/释放、复制冲突改名、剪切粘贴、新建、删除确认、深路径、跨文件系统移动、复制中关闭重开、内容 SHA-256；[检查输出](../specs/ui/tool-pages-p275/files-checks.txt) |
| UART 离屏 | 通过：断开保留输入、重连、键盘布局、短写顺序、EAGAIN 重试、永久错误/超时保留命令、波特率回滚、RX 预算、HEX、超长文本边界、关闭重开；[检查输出](../specs/ui/tool-pages-p275/tools-checks.txt) |
| 设置离屏 | 七分类切换、重复选择保留对象树、关闭重开通过；Wi-Fi/日期/宠物等内联详情是桩，**没有验证真实详情** |
| goldfish 构建 | 通过，SDK `build.sh`，11 秒；日志 `/tmp/dm-ui-p275/goldfish-build.log`；[ELF/配置哈希](../specs/ui/tool-pages-p275/sim-artifact-sha256.txt) |
| goldfish 运行 | 隔离副本无窗口启动，日志见主屏/宠物初始化；**没有目标页面截图或输入验收**；不能写 UART/文件/设置在 goldfish 已通过 |
| R528 构建/打包/check_res | **本轮未做**；旧 P274 镜像不含本轮修改 |
| 板端触摸、串口实际 TX/RX、G2D | **未验证** |
| 资源收益 | 源码可证：每个文件项少 1 个徽章容器、相邻行少 1 个分隔对象（N 项共少 2N−1 对象）；设置每行少 1 容器；UART 懒建键盘、静态文本、减少无变化刷新；未测 CPU/FPS/堆峰值，不能报百分比 |

下列是**宿主离屏截图，状态栏/服务为测试替身，不是板端或 goldfish 截图**：

- [文件列表](../specs/ui/tool-pages-p275/files.png)
- [设置显示分类](../specs/ui/tool-pages-p275/settings.png)
- [UART 常规布局](../specs/ui/tool-pages-p275/uart.png)
- [UART 键盘打开](../specs/ui/tool-pages-p275/uart-keyboard.png)

复跑入口（每次 `--output` 用不存在的新目录）：

```bash
python3 vendor/allwinnertech/apps/luncher_dm/tests/files_offscreen/run.py --output /tmp/dm-files-next
python3 vendor/allwinnertech/apps/luncher_dm/tests/tools_offscreen/run.py --output /tmp/dm-tools-next
```

`tools_offscreen` 首次运行因生成 helper 缺 stdio / frost 旧初始化警告失败；已修 runner/CMake，
使用 `cmake --build /tmp/dm-ui-p275/tools-after/build -j8` 后执行 `tools_preview` 通过。
**修正后的 runner 尚未从全新目录重跑**，下一位应做一次完整入口验证。

## 5. 已发现但还没修的问题（优先读）

### UART 键盘回车事件与字形：下一步第一项

- 离屏截图中软键盘的 NEW_LINE 图标显示缺字方框。当前键盘 items 使用 `FONT_BODY`。
- 本地 LVGL 的 `LV_SYMBOL_NEW_LINE` 是 U+F8A2，当前正文/FA fallback 不保证包含。
- 旧共享键盘用 `lv_font_montserrat_30`；固件配置已启用它，可核验 glyph 后用于 UART 键盘。
  离屏配置默认没开 30 字号，若改用它，测试生成配置也要启用对应宏。
- 重要事件差异：`lv_keyboard_def_event_cb` 的 NEW_LINE/Enter 只向 **textarea** 发 `LV_EVENT_READY`；
  OK 键先向键盘发 READY，再向 textarea 发 READY。
- 当前新页面只绑定了键盘 READY，**Enter 路径尚未接上发送**。现有测试点的是「发送」按钮，不能证明 Enter 可用。
- 不要直接把键盘与 textarea 的 READY 都绑定同一发送而忽略 OK 的双重事件：需要明确单一路由并测试一次动作只发一次。
- 建议先给测试加真实键盘 Enter/OK 点击（或按钮矩阵事件），覆盖无重发，再最小修正；不要顺便改共享键盘影响日历。

### 其他待复核

- 删除独立回车按钮是否损失必要串口操作，见第 3 节；允许空命令发换行还是保留按钮需选定清楚的行为。
- `uart_tx_enable` 发送中解绑键盘 textarea；检查超时/成功/READY 过程中重新绑定是否存在事件重复。
- 首次成功 write 立即清输入，日志在下一 timer 才刷新；现有测试覆盖，不等于实机收到了字节。
- UI 限制 240 字符，同时检查实际 UTF-8 字节≤240；超出给提示。ASCII 接收仍把非 ASCII 显为点。
- `dm_uart_dbg.c` 原有共享 head/count 用 volatile、无真正互斥；读取/清空/溢出有并发风险。
  本轮未改，不能因 UI 接收预算测试通过就声称底层线程安全。单独做可区分的并发实验后再动服务。
- 共用 settings helper 影响日期时间、宠物、蓝牙分组；这些调用页构建路径已读，但真实内联布局/长值裁切仍需检查。
- `ui_settings_close_cleanup` 与全局嵌套返回生命周期原样保留；不要贸然把 s_right/s_rail 清空，UART 返回设置还要复用父页。
- 设置亮度 VALUE_CHANGED 仍直接落盘，潜在频繁写 flash；音量已有防抖。留作资源审计后续，不混进 UART 修复。

## 6. 全局审计进度与后续方向

这不是「全局 UI 已审完」。已读取主题、导航/键盘、文件、设置、UART 主路径，并参考音乐/日历；
其他页仅做调用/刷新热点定位，未逐页运行验收。

| 页面 | 下一轮建议 |
|---|---|
| 主屏、音乐、日历 | 保持用户认可风格；检查共用 helper 没有间接改变它们 |
| 文件 | 当前右列表先让用户看；补真实目标滚动、多选/长文件名；保留文件操作逻辑 |
| 设置 | 逐一检查真实 Wi-Fi/蓝牙/日期/闹钟/宠物内联宽度、触摸和返回；不要只截图默认网络分类 |
| UART | 先解决键盘 Enter/字形，再测换行选项、单发回车、上下滚动阅读、持续接收与退出 |
| Wi-Fi/蓝牙 | 候选：去重复玻璃组/图标徽章；BT 500ms 重设状态文字、10s 重建配对表可审查变化驱动刷新；先读完整回调 |
| 闹钟、书架/阅读器、AI | 后续逐页按信息结构审计；尚未完成，不批量替换所有 glass 调用 |

不要开新图标体系、换大字体包或全量 UI 重写。先完成三处用户点名页面，拿实际画面反馈再扩散。

## 7. 接手时执行顺序（节约额度）

1. **唯一首个动作：补 UART Enter/OK 行为测试并修回车事件/字形。**
   成功：完整跑两套离屏入口并看四张图；失败：按 LVGL 事件路由定位，勿重做布局。
2. 核对 UART 单发回车操作、换行后缀和失败反馈；验证设置真实内联页的布局与生命周期。
3. 用 SDK 构建 goldfish，确保 Make 头隔离/恢复；在可访问宿主图形环境时测真实页面与返回重开。
4. 上述完成后才执行开发流程中的 R528 pack/check_res；板端 TX/RX/触摸另列待验。
5. 更新本节点结果、导出 docs；代码最终交付时才同步 r528 镜像。当前不 commit、不 push、不打稳点 tag。

构建/运行说明：

- 临时脚本 `/tmp/dm-ui-p275/build_sim.py` 用 try/finally 隔离并恢复：
  `nuttx/include/nuttx/config.h`、`include/arch`、`math.h`、`float.h`、`setjmp.h`、
  `apps/builtin/builtin_list.h`、`builtin_proto.h`。备份目录 `/data/dm/.p275-make-headers`，已恢复。
- `/tmp` 和工程不在同一文件系统，rename 曾报 EXDEV，已改为工程内临时备份；当时尚未移动文件。
- ccache 默认缓存只读，实际成功命令使用 `CCACHE_DIR=/tmp/dm-ui-p275/ccache`；没有改全局缓存配置。
- 有生成头并存是已知检查提示，不是本轮新增污染；不要 distclean 或重置用户配置。
- 本会话看不到宿主进程，无法访问 `DISPLAY=:99`；新建 Xvfb 因绑定 socket 被拒绝，未建立图形验收。
- 已复制 ELF/config/system/userdata 到 `/tmp/dm-ui-p275/sim` 启动无窗口模拟器，避免写活动 userdata。
  启动有文件系统扩容/adb/GPU 库警告，之后见主屏/宠物初始化；不能据此认定全部运行正常。
- 启动入口 `./emulator.sh /tmp/dm-ui-p275/sim -no-window -no-audio -no-boot-anim -no-snapshot -gpu swiftshader -port 5580`；
  环境 `ANDROID_EMULATOR_HOME`、`ANDROID_USER_HOME` 指向 `/tmp/dm-ui-p275/android`。
  日志 `/tmp/dm-ui-p275/emulator.log`。工具 session ID `14943` 曾持续运行，用户中断时终态未知。
  下一位先只读确认本工作区实例；不要宽泛 pkill，不要直接重启用户宿主/VNC。

## 8. 收尾边界

- 代码：本轮有 WIP，离屏与 goldfish 编译通过；目标页运行、R528 整包、板测缺失。
- 文档：本文件为当前设计与接手主入口；证据截图/测试输出放 `specs/ui/tool-pages-p275/`。
- 会话日志：本次 Codex session ID 未取得，原始会话采集未核验；不得用本文或手写总结替代原始日志。
- 本轮依用户要求停在交接。后续无需再次询问是否允许完成已授权的子 APP 修复，但不可宣称已有完整交付。

本次交接检查：docs 已导出且 `sync_docs.sh --check` 一致；P275 新相对链接未报错。
`finalize_check.py`：CODE WARN（已有脏仓/双套生成头），DOCS WARN，LOGS MISSING。
`doc_check.py` 共 7 项：索引总行数超预算、P272～P274 三个旧节点行过长、
`10-18-hx83102-touch-skel.md` 的悬空引用、该文与 `10-14part1devlog.md` 未进入索引。
P275 索引行 97 字符、AGENTS 7852 字节；本轮未开展无关文档整治。
新增触摸文档可能来自其他工作，未修改；不将它视作当前默认续接任务。
