# AGENTS.md — R528 DesktopMate 工作入口

> 顺序：本文件 → [devlog.md](devlog.md) → 当前任务详情。V1 历史（P1～P243）与 V2 资料默认不加载。
> 先读 `.agents/skills/contest-log-collector/SKILL.md`，再按任务读匹配 skill；无 skill 工具时直接读文件。
> 用户本轮需求决定任务。§五是默认续接点，不得把文档审计等新请求擅自换成修模拟器。

## 〇、执行纪律

1. 开始写清：**目标、目标平台、验收现象**。需求已明确就执行；只在产品取舍、缺失外部信息或破坏性动作前询问。已有授权不重复问。
2. 修改前读完整相关函数与调用/销毁路径。布局修改必读该屏完整构建函数；无需通读整个 SDK。
3. 排障按 **现象 → 至多 3 个假设 → 一次一个可区分的实验 → 记录结果**。连续 2 次没有缩小范围，停止叠加补丁，检查基线/观测方法。
4. **“怀疑”不得升级成“根因”。** “改后仍崩溃”只否定该实验预测，不能排除整个模块。证据不足写“未知”。
5. 每个实验记录唯一变量、预期、构建/运行证据、补丁去留。失败只撤销本轮实验改动；禁止靠关闭日志、断言、检查器或吞错误宣称修复。
6. 先完成最小可验证改动。排障不夹带美化、分层重构、性能优化。

## 一、固定环境与事实源

| 项 | 当前约定 |
|---|---|
| 工程根 | `/data/dm`，repo 多仓；根目录不是 Git 仓 |
| **V1 主线板** | `boards/r528/ynm3000/configs/deskmate`：BOE **1200×1920** 面板（横屏 1920×1200 使用）+ GT9271 + `luncher_dm`，DM_SCALE 2.4 |
| **V2 副产品板** | `boards/r528/ynm3000_v2/configs/deskmate`：1440×720 + HX83102B + `luncher_dm_v2`；显示可用，触摸拖动断触，**已冻结** |
| 模拟器 | goldfish ARM64；`vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap`，当前编 V1 `luncher_dm`（1920×1200）；切 V2 需改 defconfig 三项 |
| 工具链 | SDK `./build.sh` 注入；不直接 `make -C nuttx` |
| 串口 | `/dev/ttyUSB0` @1.5M 常驻采集；`contest2026_070_arc/tools/dm_serial_log.py` send/read；`send` stdout 是未过滤原始串口，可当板端 NSH（命令须单引号） |
| UI 源 | V1 `apps/luncher_dm/`，V2 `apps/luncher_dm_v2/`；**两树独立，不互相同步** |
| 文档源 | 根 `AGENTS.md`、`devlog.md` + `project_docs/` |
| 导出镜像 | `contest2026_070_arc/docs`、`r528/`；只由 `/data/vela/sync_docs.sh`、`sync_r528.sh` 单向导出 |
| 项目 skill | `.agents/skills/`；项目专属项软链到 `contest2026_070_arc/skills/`，后者是主源 |

**冲突处理**：用户当前要求优先；项目约定以本文件及链接的现行规范为准。旧日志、旧 README、旧 skill 示例仅作历史参考。技术结论核对当前源码/有效配置/对应产物；冲突记入任务详情，不能只取最新一句话。

## 二、按任务定位

| 要做什么 | 先读什么 |
|---|---|
| 构建、配置、打包、固化 | [开发流程](project_docs/specs/misc/development-workflow.md) + `openvela-build` |
| 找模块、评估架构 | [源码与边界](project_docs/specs/misc/architecture-map.md) |
| 改 V1 UI | `deskmate-ui`（V1 段）+ `apps/luncher_dm/ui/ui_*.c`；宏在 `deskmate_ui.h` |
| 改 V2 UI（冻结，仅维护） | `deskmate-ui`（V2 段）+ `ui-port-1440` + `dm_theme.h/.c` |
| 查启动/首刷 | `luncher_dm.c` / `luncher_dm_v2.c` → 实际调用路径；先区分 guest 崩溃与 host 抓屏 |
| 查驱动 | `nuttx-driver-development`；审查另用 `driver-code-reviewer` |
| 排障 | [methodology.md](project_docs/methodology.md) + [已知坑](project_docs/specs/misc/known-pitfalls.md) + [串口日志](project_docs/specs/misc/serial-log.md) |
| 维护记忆/文档 | `ai-devlog-system` |
| 提交/PR | [PR提交步骤](project_docs/submission/PR提交步骤.md) + [检查清单](project_docs/submission/提交检查清单.md) + `submit-pr` |
| V2 触摸（已停止） | [HX83102 流程](project_docs/specs/misc/hx83102-touch-fix-process.md)，只作证据 |

## 三、验证与收尾

- 开始运行 `python3 /data/dm/contest2026_070_arc/tools/workspace_check.py`，按任务加 `--path` / `--target`；跨仓用 `--all-repos`。不为“全绿”提交/覆盖已有改动。
- **仅文档**：校对事实、链接、长度、指令一致性；跑 `doc_check.py`；导出 docs；不编译固件、不打 `ok-*`。
- **模拟器代码实验**：goldfish 构建 + 运行证据；调试循环中不插 R528 打包。共享代码最终交付须补板端编译打包，未做写明待验。
- **UI 可见性交付**：用 V1 模拟器看 UI 时，每次改都要同步 goldfish 程序与 `/data/data` 资源、更新 userdata 后重启模拟器，并核对客体确实加载了新构建与资源；只刷 noVNC 网页不算更新，板端打包也不等于模拟器已更新。见 [模拟器资源](project_docs/specs/misc/emulator-resources.md)。不截图时用客体日志/文件校验并注明视觉待确认。
- **板端代码/资源交付**：`./build.sh <config> -j$(nproc)` → `pack_deskmate.sh <产品>`（内部 build+pack+校验）；核 `vela.bin==nsh.fex`；`/data/vela/pack.sh` 禁用。
- **`git_snapshot.sh`暂停自动调用**：全量 `git add -A`且只覆盖四仓。恢复脚本`-r`会多仓`reset --hard`，仅用户明确要求且备份后使用。
- 收尾跑 `finalize_check.py`，报告代码/文档/会话日志三段；缺一段标缺失。更新任务详情与 devlog 节点；焦点变化才改 §五。
- P370 后 `devlog.md` 与 `project_docs/devlogs/` 仅本地维护；专属仓 docs 中两项冻结为 P370 快照。
- 日志核对**本次 session ID、事件数、来源工具**并扫描凭据。`wrap_session.sh`当前默认导出当日全部会话，修复前做等价检查。

## 四、维护预算

- 本文件 ≤8KB；devlog 节点 ≤40 行、每行 ≤120 字符。超限把旧节点移到 `project_docs/archive/`，保留索引。
- 日常状态只更新必要位置；用户要求的规范修订不受“两处收尾”限制。
- 一条规则只维护一个规范源；skill 引用流程，不复制构建/验收/固化命令。
- 最多一个正在处理的工程阻塞；其他需求进 ROADMAP，不抢占当前用户任务。

## 五、当前焦点

**收尾模式（2026-11-02 起）**：源码已冻结（[P364](project_docs/devlogs/11-02-p364-freeze.md)，镜像 `ccb08b24`，`vela.bin==nsh.fex`），**不再改代码/UI/参数**，新问题只记录不修。交付顺序：专属仓同步 → 文档与日志导出 → 报告/视频/照片 → 官网 zip。

**专属仓交付（当前主线）**：V1 源码、截至 P370 的 devlog 快照和 159 份 AI Coding 日志已同步到本地专属仓工作树；后续工作区 devlog 不再同步。README 已补完文字，配图待用户在 GitHub 更新。本地改动尚未提交/推送。洁净检出构建受旧板映射及 `ynm3000` 板级/打包资源缺失阻断（本地 P372）。发布前还需处理已删日志的 Git 历史与人工复核。提交步骤见 [PR 提交](project_docs/submission/PR提交步骤.md)。

**未闭环（冻结后不改，仅记录）**：V1 NAND/romfs 偶发坏字节（P317 止血，禁止应用层再打补丁）；`reboot` 挂死需断电；养成 P307 赛后；AI 回复偶发英语根因未知；`xrun` 偶发；P295 刷新埋点未测。

保持用户未提交改动：不 commit/push/tag，除非交付明确授权。

## 六、日志与凭据

- 禁止读取输出或写入日志：token、API key、密码、订阅/节点地址、内网地址、个人敏感信息。日志只落本工作区授权会话，不自动 push。
- 导出后与 push 前扫描 `contest2026_070_arc/logs/`；只输出命中数量/路径，不输出秘密正文。
- 脱敏不能证明安全时，整份不导出并记录缺口；不伪造对话或以手写总结替代原始会话。重写历史须另行明确授权。
