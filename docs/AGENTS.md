# AGENTS.md — AI 会话交接协议（DesktopMate 项目）

> **这是什么**：本项目 3 周 AI Coding 全程使用的「AI 会话记忆体系」核心文件——每次开发会话开始，AI 先读本文件（环境/纪律/当前焦点），再读 `devlog.md`（217 节点索引）定位进度，最后按需读 `devlogs/` 子日志。整个项目的跨会话连续性靠这套机制维持，从未丢失上下文。
> **评委导读**：`〇 思考方式` = 给 AI 立的规矩；`一/一.5` = 环境与源码地图；`二` = 编译打包固化流程；`三` = 每日滚动的当前焦点；`四` = 血泪坑清单。与本仓 `skills/ai-devlog-system/` 是同一方法论的「实例 ↔ 抽象」关系。
> 下文为原文，未作内容删改（仅按提交纪律做过脱敏）。

---

# AGENTS.md — R528 openvela 项目交接文档（AI 每次会话必读）

> **文档读取协议（每次会话固定顺序，勿跳层）**：
> ① 先读 `AGENTS.md`（环境/命令/当前焦点/已知坑/纪律）
> ② 再读根目录 `devlog.md`（目录索引：每行 = 一节点）；**语音/声音/编排需求先读 `project_docs/specs/voice/VOICEDESIGN.md`**（流程指导+进度锚点）
> ③ 要排障细节 → 按 devlog.md 行内链接读对应 `project_docs/devlogs/<M-D>partNdevlog.md`（详情唯一源）
> ④ 禁止在主 devlog.md 里找排障过程——那里只有目录，没有细节
> 改本文件前先与 `project_docs/archive/AGENTS.md.bak-20260807` 对比确认无信息流失；新增内容必须「删一条旧的」。

## 〇、思考方式（每次会话必读，先于一切动手）

1. **先加载思考协议**：官方 openvela skill 已安装（`~/.atomcode/skills/` → `~/.config/opencode/skills/`，13 个，清单见 §五），**处理需求前先用 skill 工具加载匹配的 skill**；旧 `openvela-contest`（MiMo Thinker 旧上下文）已归档废弃，勿再加载。
2. **复杂问题先想后做**：按「现象→多假设→最小验证→结论」推进；能读代码定位就先把调用链/寄存器差异查清楚，再决定改不改（A3 教训：勿擅改驱动）。
3. **分层思维**：UI 层（`deskmate_ui.c`）→ 服务层 → 驱动层问题分开排；先确认问题发生在哪一层再动手，不要从上到下全查。
4. **子 APP 改造/DEBUG 固定流程**：读代码 → 分析根因 → 评估可行方案（先查 LVGL 是否支持）→ **先问用户确认** → 再改代码 → 汇总报告；详见 `project_docs/methodology.md`「子 APP 改造 SOP」。**未读完代码 / 未确认就动手 = 违规**。

## 一、环境（写死，勿改）

| 项 | 值 |
|----|-----|
| 工程根 | `/data/dm`（openvela，R528） |
| 目标配置 | `vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/deskmate`（提交配置；`nsh/` 保持上游干净勿动） |
| 工具链 | SDK 自带 GCC 13.4.0（`./build.sh` 自动注入；勿 `make -C nuttx`） |
| 旧驱动备份 | `/data/vela-backup/{screen-driver,touch-driver}` |
| 硬件 | BOE 1200x1920 MIPI DSI 屏（横屏 1920x1200 使用）+ GT9271 触摸 |

## 一.5、源码结构索引（Phase 1 拆分后，改 UI 直接定位，勿全仓搜索）

**唯一 UI 工程：`vendor/allwinnertech/apps/luncher_dm/`**（`apps/luncher_dm` 是另一个仓的路径，别混）：

| 文件 | 职责 |
|------|------|
| `deskmate_ui.h` | **跨页声明头 + 全部宏**（DM/COL/FONT/TOP_BAR_H/**DM_VERSION/DM_BUILD_TAG 版本号**）——改字号/间距/版本号先看这里 |
| `deskmate_ui.c` | 共享构建器：`make_clean_cont`/`subpage_big_title`/`settings_*` 卡片体系/`show_subpage`/`close_subpage`/`dm_net_status_refresh`/音乐播放核心（dm_sound_*） |
| `ui/ui_home.c` | 主屏（状态栏 132px + hero 时钟 + 卡片 + dock）+ 锁屏 standby |
| `ui/ui_wifi.c` | WiFi 子页（iPad 风格：状态卡/主开关/已连接/SSID 分组/其他网络/已保存） |
| `ui/ui_bt.c` | 蓝牙子页（iPad 风格：状态卡/主开关/Discoverable/MY DEVICES/OTHER DEVICES） |
| `ui/ui_files.c` | 文件管理器子页 | 
| `ui/ui_books.c` | 电子书阅读器子页（含 reader 覆盖层） |
| `ui/ui_settings.c` | Settings 子页（**ABOUT 区显示版本号 = deskmate_ui.h 的 DM_VERSION/DM_BUILD_TAG**） |
| `ui/ui_music.c` | 音乐子页 + HOME/锁屏播放控件 |
| `ui/ui_uart_dbg.c` | UART 调试工具子页（FLOATING 工具栏 + 日志窗口 + 3 配色主题） |
| `ui/ui_manager.c` | Phase 3 预留（未接入，勿调用） |
| `dm_net.c/h` | **WiFi/蓝牙驱动对接**（扫描/连接状态机 g_conn_gen 代数；WiFi 扫描后台线程 + g_scan_busy 防重入） |
| `dm_ai.c/h` | **AI 子页 WS client**（RFC6455 短连接：ai_worker 发问/dm_ai_voice 语音控制/dm_ai_poll 轮询） |
| `dm_weather.c/h` | 天气获取（后台 worker + DNS 重试；UI 在 ui_home.c 天气卡） |
| `dm_uart_dbg.c/h` | **UART 调试工具接收层**（/dev/uart1=PD21/22 mux4 后台线程，外部设备接 BSN20 电平转换外设侧 + 8KB 环形缓冲 + 5 档波特率 9600~115200（1500000 已移除）+ uartdbg_test 注入接口） |
| `music_service.c/h` | 音乐服务层（XPlayer/dm_sound 唯一持有者，**架构目标未接入，勿调用**） |
| `service/bt_service.c` | 蓝牙服务层（未接入 UI 的独立逻辑，勿直接调用） |
| `service/wifi_service.c` | WiFi 服务层（未接入 UI 的独立逻辑，勿直接调用） |
| `luncher_dm.c` | 应用主入口（自启/开机切 luncher_dm） |

改 UI 流程：先定位到上表对应文件 → 查 `deskmate_ui.h` 宏/构建器 → 改完走 §二 编译打包固化。

## 二、必做动作（编译/打包/固化，改代码后必须全部完成）

```bash
# 1. 编译
cd /data/dm && ./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/deskmate -j$(nproc)
# 2. 打包
cd /data/dm/vendor/allwinnertech/lichee && source envsetup.sh && lunch_nuttx 2 && pack
# 3. 确认产物一致
ls -la /data/dm/vendor/allwinnertech/lichee/out/r528s3/gemini-s1_nand/image/nsh.fex /data/dm/nuttx/vela.bin
# 4. 🔴 打包后资源/产物自证（P82/P92 写死纪律；已升级为 pack 末尾自动跑）
#    post-dragon 钩子自动调 check_res.sh，失败即中断打包。三段硬断言：
#    res.fex(romfs 路径级+字节==源+新鲜度) / nsh.fex==vela.bin / 镜像含完整 res+nsh 块。
bash /data/vela/check_res.sh   # 手动复跑；一键全流程： bash /data/vela/pack.sh
# 5. 固化（🔴 编译打包成功必须立即跑，不等用户提醒）
bash /data/vela/git_snapshot.sh          # 自动 tag ok-YYYYMMDD-序号，三仓 add/commit/tag 一步完成
```

- 只改代码不编译打包 = 任务未完成；**产物没过 check_res = 任务未完成**（P92 写死；pack 末尾自动跑，见第 4 步）。
- 恢复稳点：`bash /data/vela/git_snapshot.sh -r <tag>`；查稳点 `-l`；单文件还原 `git -C <仓> checkout -- <路径>`。
- 镜像：`lichee/out/r528s3/gemini-s1_nand/rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img`。

## 三、当前状态 / 下一步（唯一焦点）

**📌 当前焦点（2026-09-11，交作业收尾 / 发布 V1.0.0）**：基线 **ok-20260911-21 / V1.0.0-20260911**（YNM-3000 收尾：版本型号/去假天气/README/日志合规/运行时Skill/报告，编译打包自证过）；本轮见 `9-11part2devlog.md`；**下一步=上板烧录验证 → 视频/照片/PPT → 公共仓 PR（去 P140/P141）→ fork PR → push**；9.20截止。

**✅ 已闭环（tags 见 devlog.md 节点表）**：P73~P217 全量闭环；**本轮 ok-20260911-9~21**（稳定性/死代码/语音时段/宠物联动/开机对时/麦克风POP/**P212 res烧录排障 / P213 打包防呆 / P214 提示音尾音根治 / P217 交作业收尾发布V1.0.0**）。

**待上板/续**：①上板烧录 ok-20260911-21 整包并验证剩余 R/V 清单（`verify_checklist.md`）；②交付物视频/照片/海报/PPT；③公共仓 PR（codec POP/DSI超时/BOE面板/ALS/LVGL三fix，去 P140/P141）；④fork PR+CLA；⑤专属仓 push（待点头）。

**📤 交作业纪律**：只推新增（luncher_dm/deskmate+res新wav）；nsh保持干净；**公共仓改动（驱动+lvgl）须走 PR 到 `dev-ai-contest-2026`**；tests误改禁推；logs/禁手改；赛规见 `project_docs/submission/`。

**📦 版本**：V1.0.0-20260911（YNM-3000）/ V0.0.7-20260905 / V0.0.6-20260904 / V0.0.5（归档 `/data/vela/releases/`，快照 tag 见仓）；版本号由用户决定，AI 只固化 tag + 归档。

**⚠️ 写死**：`POWER_ANA_CTL@0x348` 只能 `update_bits()` 局部操作、**禁止整写**（-7 屏闪根因）。

> 详细：节点/里程碑见 `devlog.md` 索引（P1~P217）。遗留独立问题（退出重进卡死 / shtc3 HPWORK / bt_recv 空转 / Settings 其余开关无回调 / 蓝牙断开动作）按 devlog.md 链接查子 devlog，**本文件不维护备选清单**。

## 四、已知坑（仅 2026-08 仍生效 + 高频，完整历史见 .bak）

1. **UI 唯一源是板端 `vendor/allwinnertech/apps/luncher_dm/deskmate_ui.c`；模拟器同步已暂停**——只改板端，**不要跑 sync_deskmate.sh**（默认方向模拟器旧副本会覆盖板端，2026-08-06 曾丢修复致 MP3 无声）。恢复同步时先跑 `-r`（板端→模拟器）。
2. **mipi_config 二选一**：`CONFIG_T070S140B_MIPI` 与 `CONFIG_BOE_1200X1920_MIPI` 必须互斥编译，否则全局符号 `g_lcd0_config` 重复定义（BOE 开机不加载的根源）。
3. **资源必须进固件**：①prebuilt 改后须 `distclean` 全量重编 + `strings vela.bin` 验证；②**烧录必须整包镜像**（res.fex 含 WiFi 固件+字体+提示音，源目录=`lichee/board/common/data/res/`，勿查 `/vendor/openvela/boards/vela/resource`）；③打包自动跑 `check_res.sh`（§二第 4 步，post-dragon 钩子，失败中断打包）；④烧后板端 `ls /resource/` 验证（烧录工具可能跳过 res 分区）。
4. **`lv_color_t` 只有 3B**（LVGL 9.1 XRGB8888）：手动分配 canvas/图像 buffer 按 cf 字节数算（ARGB8888=4B/px），勿用 `sizeof(lv_color_t)`（曾越界 16KB 破坏相邻内存致 Data Abort）。
5. **LVGL 矩阵旋转死机**：`LV_DRAW_TRANSFORM_USE_MATRIX` + `lv_display_set_rotation` 与 DIRECT 渲染不兼容 → Data Abort。横屏必须走驱动层 `degree0` 旋转。

## 五、Skill 选择（真实可用清单，勿引用未安装 skill）

- **官方 openvela skill 已安装**（2026-08-09 从 `~/.atomcode/skills/` 挂入 `~/.config/opencode/skills/`，13 个）：`openvela-build`（编译/构建/模拟器/报错修复）· `openvela-quickstart`（环境搭建）· `nuttx-driver-development`（NuttX 驱动开发/审查/审计）· `pcm-audio`（PCM 音频质量）· `codesize`（固件体积分析）· `memdump`（堆内存泄漏）· `kconfig-tweak`（改 Kconfig/.config）· `executor`（持久交互进程）· `tmux`（tmux 远程控制）· `submit-pr`（提 PR）· `skill-creator`（创建 skill）· `driver-code-reviewer`（驱动专项审查）· `contest-log-collector`（大赛日志归集）——**按需求语义匹配加载，勿全部加载**。
- **与本项目无关（勿用）**：Rockchip/Armbian 系列（`armbian-build`/`dtbbuild`/`firefly-sdk`/`fndtbbuild`/`rk3588-emmc-boot-repair`/`sf32-skill`/`toybrick-d0`/`yuzuki-eink`）+ `customize-opencode`（改 opencode 自身配置时用）。
- **已废弃归档**：`openvela-contest`（MiMo Thinker + 6FHD 旧上下文，移入 `/data/.claude/skills/_deprecated-20260809/`）。完整历史清单见 `project_docs/archive/AGENTS.md.bak-20260807`。

## 六、每日 MD 写作（用户说「写 md」时两步）

1. 写子文档：`project_docs/devlogs/<月-日>partNdevlog.md`（存在则末尾追加）
2. 更新 `devlog.md` 节点表首行（新 P 号）+ 本文件「三」概要（≤10 行）

## 七、维护纪律（硬限，违反即回退）

> 🔴 以下为**可量化硬限**，AI 每次写入前自查，超限必须先删再加。

| 指标 | 硬限 | 超限动作 |
|------|------|----------|
| AGENTS.md 总字节 | **≤ 14KB** | 删§四旧坑或§三旧闭环项 |
| AGENTS.md §三概要 | **≤ 10 行** | 压缩或下沉 devlog |
| devlog.md 节点表行数 | **≤ 80 行** | 最早 10 行移入 `project_docs/archive/devlog_archive_*.md` |
| 节点表单行字符数 | **≤ 120 字符** | 砍排障过程，只留结论 |
| §四单条坑描述 | **≤ 200 字符** | 在讲故事不是写规则，精简 |
| devlog.md 格式 | **纯表格，禁散文段落** | 「当前节点」只有一行指针 |

- **每新增一条坑/规则，必须删一条旧的**；坑按日期淘汰。
- 当前目标永远只留 1 个最高优先。
- **会话结束只改两处**：devlog.md 节点表首行 + AGENTS.md「三」概要。

## 八、排障方法论

> 10 条方法论已迁至 `project_docs/methodology.md`，排障时按需读取，非每次会话必读。

---
*AGENTS.md maintained by AtomCode · 2026-08-28 文档瘦身（§四坑3 压缩/§八迁出/§七硬限表/devlog 归档，总读取量 -45%）*
