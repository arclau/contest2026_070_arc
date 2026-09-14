# OpenVela 官方 AI Skills 使用指南

> 来源：open-vela/.claude 仓库 `dev-ai-contest-2026` 分支（commit `f379db4`）
> 安装位置：`~/.atomcode/skills/<技能名>/`（全局，已装 13 个）
> 许可证：Apache-2.0

## 一、怎么使用（核心机制）

这套技能的使用方式**不是手动敲命令**，而是**让 AI 助手自动发现并调用**：

1. **自动发现**：技能安装后，AI 助手（atomcode / Claude Code）在会话启动时扫描技能目录，加载所有 `SKILL.md`。
2. **描述匹配触发**：每个技能的 `description` 字段写明"何时使用"，当你的任务与描述匹配时，助手自动加载并执行该技能的工作流。
3. **显式指定**：也可以直接点名，例如"用 openvela-build 技能编译"、"加载 driver-code-reviewer 审查这个驱动"。

### 触发示例（自然语言即可）

| 你说什么 | 会触发哪个技能 |
|---|---|
| "帮我编译 openvela / build / make 报错了" | openvela-build |
| "第一次搭 openvela 环境 / repo init 失败" | openvela-quickstart |
| "审查一下这个驱动 / review 这个 driver" | driver-code-reviewer |
| "写一个 NuttX sensor 驱动 / 移植以太网驱动" | nuttx-driver-development |
| "提交 PR 到 openvela / push 到社区" | submit-pr |
| "固件太大了，分析下 code size" | codesize |
| "分析内存泄漏 / 看 memdump 日志" | memdump |
| "改一下 .config，关掉 LTO" | kconfig-tweak |
| "分析音频底噪 / 爆音问题" | pcm-audio |
| "跑 QEMU / NuttX 模拟器 / 交互式调试进程" | executor |
| "控制 tmux 里的 gdb 会话" | tmux |
| "收集这次对话日志到大赛仓库" | contest-log-collector |
| "创建一个新技能" | skill-creator |

## 二、技能清单（13 个）

| 技能 | 用途 | 关键触发词 |
|---|---|---|
| `openvela-build` | openvela 固件编译、配置（menuconfig）、模拟器运行、编译报错修复 | 编译 openvela、build、make、menuconfig、emulator |
| `openvela-quickstart` | 开发环境一键搭建：环境检测、装依赖、智能选源、下载编译、跑模拟器 | 首次搭建、repo sync/init 失败、编译报错、模拟器起不来 |
| `nuttx-driver-development` | 创建/更新/审查 NuttX 设备驱动（sensor/char/network/fb-LCD/USB/audio/电源/MCAL/I2C-SPI 等，820K 最大） | 驱动开发、驱动移植、board bringup |
| `driver-code-reviewer` | 驱动代码质量审查（59 Pattern + 双轮交叉验证 + 量化评分），只读不修改 | 驱动审查、review driver、提交前检查 |
| `submit-pr` | Fork 模式向 openvela（GitHub/Gitee）提 PR，支持单/多仓库批量 | 提交 PR、push 到 openvela、批量提交 |
| `codesize` | 固件二进制大小分析（bin/elf/map），多核多架构 | 优化固件体积、版本对比、找大模块 |
| `memdump` | 分析 NuttX/Vela memdump 日志，检测内存泄漏与高消耗模块 | 内存问题、堆分配、内存对比 |
| `kconfig-tweak` | 命令行改 NuttX/Linux .config，免交互式 menuconfig | 开关配置项、批量改配置、CI 场景 |
| `pcm-audio` | PCM 音频质量问题分析（削波/静音/爆音/底噪/周期失真） | 录音/播放问题、音频杂音调试 |
| `executor` | 管理持久化交互式 CLI 进程（REPL、调试器、QEMU、NuttX 模拟器） | 有状态进程、模拟器、交互式调试 |
| `tmux` | 远程控制 tmux 会话（发按键 + 抓取输出），配合 python/gdb 等交互 CLI | tmux 会话控制 |
| `contest-log-collector` | 大赛 AI Coding 日志自动归集（仅 openvela 工作区内采集，写入选手仓 logs/） | 归档会话、export session、保存对话 |
| `skill-creator` | 创建/更新新技能的指南 | 创建技能、扩展能力 |

## 三、技能结构

```
skills/<技能名>/
├── SKILL.md              # 技能定义（必需）：YAML front matter（name + description）+ 工作流正文
├── scripts/              # 辅助脚本（可选，如 submit-pr/detect-repos.sh）
├── references/           # 领域知识参考（可选，如 driver-code-reviewer 的 59 Pattern 规格）
└── LICENSE               # 许可证（可选）
```

`description` 字段是触发开关，写得越具体，自动匹配越准：

```yaml
---
name: openvela-build
description: "openvela 固件编译、配置和模拟器运行。Use when: 编译 openvela、build、make、构建、menuconfig、运行模拟器、emulator、编译报错修复。"
---
```

## 四、Agent（可选进阶）

仓库还带一个端到端 Agent：`agents/driver-workflow.agent.md`
- NuttX 驱动开发全流程（新驱动 / 改进现有 / 代码审查 / 测试生成，6 步 3 次交互，从需求到提交）
- 会自动按路径 `.claude/skills/{name}/SKILL.md` 加载 `nuttx-driver-development`、`driver-code-reviewer` 等技能执行

## 五、注意事项

1. **新会话生效**：技能目录在会话启动时扫描，刚装的新技能下一次会话才出现在可用列表。
2. **安装位置对应关系**：官方文档说克隆到项目根 `.claude/`；对 atomcode 来说等价目录是全局 `~/.atomcode/skills/`（已装好）或项目级 `./.atomcode/skills/`。
3. **contest-log-collector 是特殊技能**：只在 openvela 工作区内（向上能找到 `.repo/`）自动采集对话日志，写入选手仓 `logs/<github_login>/`，不会自动 commit，需自己 git push。
4. **来源仓库**：`github.com/open-vela/.claude`（gitee 镜像：`ssh://git@gitee.com/open-vela/.claude`），如需更新可重新拉取 `dev-ai-contest-2026` 分支覆盖。
