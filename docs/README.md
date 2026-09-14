# project_docs — R528 openvela 项目文档库

> 工程：openvela (R528) — nsh 配置｜工作目录：`/data/dm`
> **AI 交接：先读根目录 `AGENTS.md`（环境/焦点/纪律）→ `devlog.md`（节点总览）→ 本目录具体文档。**
> 根目录只保留 `AGENTS.md`/`devlog.md`；本文档库是唯一项目文档存放处，与 SDK 官方 `docs/` 隔离。

## 目录结构

| 目录 | 内容 | 命名规范 |
|------|------|----------|
| `devlogs/` | 每日详细开发日志（保持扁平，勿分子目录） | `<M-D>devlog.md`（当天首篇）、`<M-D>partNdevlog.md`（当天第 N 段，无连字符）。如 `8-8part4devlog.md` |
| `specs/pet/` | 电子宠物规格 | 产品/实现/精灵图三件套 |
| `specs/voice/` | 声音设计（VOICEDESIGN.md 为总纲） | Phase 1/2/3 交付物 |
| `specs/wireless/` | WiFi/蓝牙需求+方案 | — |
| `specs/misc/` | Books/健康助理/源码速查/参考 | — |
| `datasheets/` | 硬件模组手册+协议（LD2410B） | 原厂 PDF |
| `logs/` | 原始调试日志（串口抓取等） | `<日期><主题>log.txt` |
| `archive/` | 过期备份/旧版本 | 原文件名 + 日期后缀（原 `origin-docs-20260909/` 有用内容已并入 `specs/misc/`） |
| `submission/deliverables/` | 交付物：技术报告 / 开发功能-分层 / 开发历程逐篇时间线 | — |
| `原始需求.txt` | 项目原始需求（health-assistant-design 的溯源） | — |

## devlogs/ 索引（共 102 篇，2026-07-24 ~ 09-11；节点/状态见根 `devlog.md`）

| 文件 | 日期 | 主题 |
|------|------|------|
| `7-24part1devlog.md` | 2026-07-24 | Vela Desktop Mate — 开发日志 (DevLog) |
| `7-24part2devlog.md` | 2026-07-24 | GT9271 触摸驱动调试 |
| `7-25part1devlog.md` | 2026-07-25 | GT9271 触摸驱动调试复盘 |
| `7-25part2devlog.md` | 2026-07-25 | 推理框架：GT9271 I2C 调试专家思维模型 |
| `7-25part3devlog.md` | 2026-07-25 | R528/OpenVela SDK 能力手册 — 给 AI 助手的工具箱 |
| `7-25part4devlog.md` | 2026-07-25 | GT9271 TWI 中断问题 — Gemini 靶向引导 |
| `7-25part5devlog.md` | 2026-07-25 | GT9271 触摸 I2C 调试 — 接力 Prompt |
| `7-25part6devlog.md` | 2026-07-25 | MiuMiu 施工日志 — 2026-07-25 |
| `7-25part7devlog.md` | 2026-07-25 | MiuMiu 施工逻辑 — GT9271修复 + LVGL Demo + 音乐播放 |
| `7-25part8devlog.md` | 2026-07-25 | MiuMiu 接力 Prompt — GT9271修复 + LVGL Demo + 音乐播放 |
| `7-25part9devlog.md` | 2026-07-25 | MiuMiu 思考方式 — GT9271 触摸修复 + LVGL Demo 调试方法论 |
| `7-25part10devlog.md` | 2026-07-25 | MiuMiu 开发日志 |
| `7-26part1devlog.md` | 2026-07-26 | LVGL Demo 触摸修复全记录 |
| `7-26part2devlog.md` | 2026-07-26 | geminifix.md — LVGL Demo 触摸点击与 180° 屏幕显示修复记录 |
| `7-26part3devlog.md` | 2026-07-26 | velafix-0726.md — OpenVela R528 音频/触摸/稳定性修复记录 |
| `7-26part4devlog.md` | 2026-07-26 | velafix-0726t2.md — OpenVela R528 第二阶段修复记录 |
| `7-26part5devlog.md` | 2026-07-26 | LVGL Demo 触摸修复 |
| `7-26part6devlog.md` | 2026-07-26 | LVGL Demo 触摸修复与当前问题 |
| `7-26part7devlog.md` | 2026-07-26 | LVGL Demo 触摸调试方法论 |
| `7-26part8devlog.md` | 2026-07-26 | VelaDevPower™ — 最强底层开发工程师思维框架 |
| `7-27part1devlog.md` | 2026-07-27 | tfdevlog727.md — TF 卡挂载问题全记录 |
| `7-28devlog.md` | 2026-07-28 | R528 GEMINI-S1 BOE 1200x1920 屏幕移植 |
| `8-3devlog.md` | 2026-08-03 | R528 BOE 屏 + GT9271 触摸驱动移植与优化 |
| `8-4devlog.md` | 2026-08-04 | luncher 独立注册 + 1920x1200 横屏适配 + 触摸测试子 APP |
| `8-4part2devlog.md` | 2026-08-04 | Desktop Mate UI 移植 + 模拟器 Web 预览免刷机 |
| `8-4part3devlog.md` | 2026-08-04 | Desktop Mate UI 视觉修整 + 实时天气 + FontAwesome 图标 |
| `8-4part4devlog.md` | 2026-08-04 | (P4) — Dock 图标换用 Flyme 高清 + 交互优化 + 一键锁屏 |
| `8-5devlog.md` | 2026-08-05 | 子 APP UI 统一修整 + Music 播放器移植重写 + Settings 重设计 + 崩溃修复 |
| `8-5part2devlog.md` | 2026-08-05 | 全 APP 圆角统一 40px + 子页 iOS 大标题 |
| `8-5part3devlog.md` | 2026-08-05 | (P3) — 上板开机启动切换 + Data Abort 死机根因定位与修复 |
| `8-5part4devlog.md` | 2026-08-05 | (P4~P6) — 上板差异全面修复（字体/符号/主题）+ 状态栏 WiFi/蓝牙符号 |
| `8-5part8devlog.md` | 2026-08-05 | (P8) — 音乐播放器 XPlayer 集成（mp3/flac/ogg/aac/wav）+ 真实进度 + 清单 UI 改造 |
| `8-6part1devlog.md` | 2026-08-06 | XPlayer 音乐播放器收尾：编译/UI/日志 + 无声问题排查 |
| `8-6part2devlog.md` | 2026-08-06 | 无声问题根治：writei 字节账目 + 24bit 位深传递 |
| `8-6part3devlog.md` | 2026-08-06 | MP3 无声根治：44100→48000 重采样 + fd:// seek 误报修复 |
| `8-6part4devlog.md` | 2026-08-06 | MP3 出声验证 + 卡顿/切歌无声排查收尾（交接下个会话） |
| `8-6part5devlog.md` | 2026-08-06 | 卡顿根治排查：重采样移线程否决 + SDMMC DMA 深挖 + 音频线程优先级提升 |
| `8-6part6devlog.md` | 2026-08-06 | 切歌无声根因重大突破：-16 EBUSY（声卡设备未释放） |
| `8-6part7devlog.md` | 2026-08-06 | 切歌无声排查：驱动层全线排除 + 音量新线索 |
| `8-7devlog.md` | 2026-08-07 | 切歌无声收尾排查（音量链路排除→死机修复→codec 输出深挖→字体修复→上层解码审查） |
| `8-7part2devlog.md` | 2026-08-07 | 切歌/连播无声：歌曲级差异定位（供外部 AI 独立分析） |
| `8-7part3devlog.md` | 2026-08-07 | 切歌无声决战：数字层铁证排除 → 歌曲级 → XPlayerReset → 最后收敛 |
| `8-8devlog.md` | 2026-08-08 | 切歌无声：软件侧最终排查 + 硬件验证指导 |
| `8-8part1devlog.md` | 2026-08-08 | 切歌无声终极排查：A2/A3 证伪 → codec 回滚 → 首播恢复 → 双 free 修复 → RDEN/bit30 链路 |
| `8-8part2devlog.md` | 2026-08-08 | 切歌无声终极排查报告（供高级 AI 独立分析） |
| `8-8part3devlog.md` | 2026-08-08 | 音乐控件系列修复 + Beta V0.0.1 版本归档（-16~-19） |
| `8-8part4devlog.md` | 2026-08-08 | WiFi SSID 列表空白修复 + Books 点击修复 + 蓝牙 H4 110 定位（-24 续） |
| `8-9devlog.md` | 2026-08-09 | Books 内置书/进度持久化 + Files 真·文件管理器 + UI 交互修复 |
| `8-9part2devlog.md` | 2026-08-09 | 子 APP UI 审核整改（Files/Books/Games）+ 布局统一 |
| `8-10devlog.md` | 2026-08-10 | 状态栏恢复 + 子页修复 + WiFi/蓝牙 iPad 风格重设计（V0.0.2） |
| `8-10part2devlog.md` | 2026-08-10 | 蓝牙 H4 open 110 深度排查全景（供高级 AI 分析） |
| `8-10part3devlog.md` | 2026-08-10 | 状态栏根治（子页/锁屏与 Home 同款）+ 文件管理器图标修复 |
| `8-10part4devlog.md` | 2026-08-10 | 天气链路修复全景（WiFi 重连 → 天气/时间/温湿度 → 天气卡交互与 API debug） |
| `8-11part1devlog.md` | 2026-08-11 | 时间对时链路闭环 → 状态栏三件套 → 回退 |
| `8-11part2devlog.md` | 2026-08-11 | AI 子 APP 全链路落地笔记（P48~P55 全景） |
| `8-11part3devlog.md` | 2026-08-11 | MIC 上板修复 + 控制台恢复 + 串口降噪全景（P56~P62） |
| `8-11part4devlog.md` | 2026-08-11 | P63 MIC 硬件上板验证闭环（arecord 命令行录放成功） |
| `8-12devlog.md` | 2026-08-12 | AI 语音子 APP：UI 重构 + 语音链路全打通（P64~P69） |
| `8-14devlog.md` | 2026-08-14 | 语音对话闭环：双向流式 TTS 打通 + WiFi 预配置 + Bug Review（P72） |
| `8-15part1devlog.md` | 2026-08-15 | 语音对话全链路闭环：TTS/ASR 上板排障 P73~P82 + V0.0.3 归档 |
| `8-15part2devlog.md` | 2026-08-15 | 新会话入口（part2） |
| `8-15part3devlog.md` | 2026-08-15 | 健康助理第①层 + 启动优化 + prox 排障（part3） |
| `8-16part1devlog.md` | 2026-08-16 | 距离校准/离开暂停/本地提示音/通知时序/语音链路修复/UI 配色/HOME 双小圈 |
| `8-16part2devlog.md` | 2026-08-16 | 弹窗排队/提示音语音优先 + shtc3 并发锁 + WiFi 连上即拉天气 |
| `8-17part1devlog.md` | 2026-08-17 | start_wifi.sh 减冗余提速（P113b） |
| `8-23part1devlog.md` | 2026-08-23 | 功能开发：天气优化 + AI 语音控制音量/亮度/音乐（P114~P116） |
| `8-23part2devlog.md` | 2026-08-23 | 审查修复与打磨：review-4~review-9 |
| `8-23part3devlog.md` | 2026-08-23 | 音频仲裁层（review-10，治本） |
| `8-23part4devlog.md` | 2026-08-23 | UI 全量汉化收尾（review-13） |
| `8-23part5devlog.md` | 2026-08-23 | LD2410B 毫米波雷达替代接近传感器（P117） |
| `8-26part1devlog.md` | 2026-08-26 | LD2410B 上板调试：设备路径/配置下发/波特率/零字节排查（P118） |
| `8-27part1devlog.md` | 2026-08-27 | UART 调试工具 + UART0 与 TF 卡冲突真相 + 换口 UART1 |
| `8-27part2devlog.md` | 2026-08-27 | UART1 回环自证全通 + 波特率链路调查 + 1500000 遗留（换会话专挖） |
| `8-27part3devlog.md` | 2026-08-27 | 1500000 档 FAIL 收尾：24M 时钟 dl=1 硬件极限实证 + 用户拍板回归 115200 |
| `8-27part4devlog.md` | 2026-08-27 | P125：锁屏人体距离显示 + 在座距离阈值定稿（1.5m 配置化） |
| `8-28part1devlog.md` | 2026-08-28 | 锁屏人体距离显示修复全链路（P126） |
| `8-31part1devlog.md` | 2026-08-31 | （part1）— P129~P135 上板验证 + P137 AI 语音尾音修复 |
| `9-1part1devlog.md` | 2026-09-01 | （part1）— AI 语音尾音收尾（P141）+ 背景音乐开场（P142）+ 欢迎语去称呼（P143）+ 锁屏乱码修复（P144） |
| `9-4part1devlog.md` | 2026-09-04 | （part1）— 文件复制进度条（P145）+ 音乐联动（P146）+ 状态栏室内标注（P147）+ WiFi 图标三态（P148~P150） |
| `9-4part2devlog.md` | 2026-09-04 | （part2）— 亮度显示问题交接（下会话 AI 处理） |
| `9-4part3devlog.md` | 2026-09-04 | （part3）— 光感/暗光弹窗全链路闭环 |
| `9-5part1devlog.md` | 2026-09-05 | UART 调试工具键盘统一 + 共享 dm_kb_* 重构 |
| `9-5part2devlog.md` | 2026-09-05 | dm_kb_* 审查修复 + UART 调试工具体验优化 |
| `9-5part3devlog.md` | 2026-09-05 | V0.0.7 上板验证 + P135/P133/蓝牙现状评估 |
| `9-5part4devlog.md` | 2026-09-05 | Voice Director 听感修复（回来按时段说话 + 回来冷却独立 + 清空票） |
| `9-5part5devlog.md` | 2026-09-05 | 全量 BUG 修复（14 项：栈溢出/注入/竞态/资源泄漏） |
| `9-6part2devlog.md` | 2026-09-06 | 电子宠物系统修复（P182~P184） |
| `9-7part1devlog.md` | 2026-09-07 | 模拟器移植尝试（P188，✅ 已闭环） |
| `9-7part2devlog.md` | 2026-09-07 | 胖橘 96 帧整链 + 黑猫根因排障（P189/P190，✅ 已闭环） |
| `9-08part1devlog.md` | 2026-09-08 | P197 精灵网格对齐 + 删 PET_STATE_SLEEPY |
| `9-08part2devlog.md` | 2026-09-08 | P198 开机随机卡 LOGO 修复（NuttX 显示链路 DSI 死等） |
| `9-08part3devlog.md` | 2026-09-08 | 用户三问题一次性修复 + 重新打包 |
| `9-09part1devlog.md` | 2026-09-09 | cat12.png 精灵替换（未闭环） |
| `9-09part2devlog.md` | 2026-09-09 | pet 极简重构日（P201~P205） |
| `9-09part3devlog.md` | 2026-09-09 | P206 交作业专项启动：SDK清理 + deskmate配置 + 专属仓070 |
| `9-10part1devlog.md` | 2026-09-10 | P207 全应用审计+修复包：快问/城市/蓝牙WiFi/设置/音乐/UART/文件/Games/报告/README/MiMo/A2DP交接 |
| `9-10part2devlog.md` | 2026-09-10 | P208：验证收绿 + 日历 + 闹钟 + 全量rev（ok-25~38） |
| `9-10part3devlog.md` | 2026-09-10 | P209：WiFi包自证 + 日历导航返工（ok-39~41） |
| `9-10part4devlog.md` | 2026-09-10 | P210：闹钟返工 + 日历升级（ok-42~43） |
| `9-10part5devlog.md` | 2026-09-10 | P211：日期与时间独立子页（ok-44） |
| `9-11part1devlog.md` | 2026-09-11 | P212~P215：res 烧录排障 / 打包防呆 / 提示音尾音根治 / 公共 PR 决策 |
| `9-11part2devlog.md` | 2026-09-11 | 交作业收尾与发布 V1.0.0（YNM-3000） |

## specs/ 内容（2026-09-09 按主题分组）

| 文件 | 类型 | 说明 |
|------|------|------|
| `voice/VOICEDESIGN.md` | 总纲 | 语音/声音/编排流程指导+进度锚点（AGENTS 点名先读） |
| `voice/VOICE_INTERACTION_DESIGN.md` | 产品 | 声音交互设计 Phase 1（人设=银月） |
| `voice/VOICE_ASSET_PLAN.md` | 规划 | 第一批 WAV 资源规划 Phase 2 |
| `voice/VOICE_DIRECTOR_SPEC.md` | 实现 | Voice Director 规格 Phase 3/4 |
| `pet/PET_PRODUCT_SPEC.md` | 产品 | 电子宠物产品设计规格 |
| `pet/PET_IMPLEMENTATION_SPEC.md` | 实现 | 电子宠物实现规格（单源真相） |
| `pet/PET_SPRITE_SPEC.md` | 需求 | 精灵图需求（12 状态） |
| `wireless/wifi-bt-ui-requirement.md` | 需求+交接 | WiFi/蓝牙 UI 接入需求、驱动 API、8-9 上板铁证 |
| `wireless/wifi-bt-bsp-design.md` | 方案 | ChatGPT BSP 方案（-22/-23 已落地） |
| `misc/books-reader-solution.md` | 方案 | Books 电子书方案（-25 已落地） |
| `misc/health-assistant-design.md` | 方案 | 健康助理设计定稿（由 `原始需求.txt` 收敛） |
| `misc/sun8iw20-codec-quickref.md` | 源码速查 | sun8iw20-codec.c 源码快照（ok-20260808-8 基线） |
| `misc/openvela-official-skills-guide.md` | 参考 | openvela 官方 skill 指南 |
| `misc/doubao-voice-integration.md` | 参考 | 豆包语音 ASR/TTS 对接调试手册 |
| `misc/ai-subsystem-audit.md` | 审计 | AI 子 APP 链路审计与调优（原 AIYOUHUA.MD） |

## datasheets/ 内容

| 文件 | 说明 |
|------|------|
| `HLK LD2410B生命存在感应模组说明书 V1.0 9.pdf` | 存在感应模组手册（uart_dbg/LD2410B 参考） |
| `LD2410B串口通信协议V1.08.pdf` | 串口协议原文 |

## logs/ 内容

| 文件 | 说明 |
|------|------|
| `2026-08-06-music-debug-seriallog.txt` | 8-6 音乐无声排查原始串口日志（581KB） |
| `2026-08-06-music-testlog.txt` | 音乐测试日志 |

## 维护纪律

- 新增文档一律入本库，**根目录不放任何新 md/txt**。
- 新建 devlog 按 `<M-D>partNdevlog.md` 无连字符命名；写完在根 `devlog.md` 节点表加一行 + 更新「当前节点」。
- 移动/重命名文件必须同步修正 `AGENTS.md`/`devlog.md` 的引用路径。

---
*project_docs README by AtomCode (deepseek-v4-flash) · 2026-08-09 建立（文档库规范化）· 2026-09-11 更新（devlogs 索引补全至 102 篇 + 清理归档）*
