# DesktopMate 文档入口（V1 主线）

本工程。AI 先读根 `AGENTS.md` → `devlog.md` → 当前任务详情。默认只加载相关规范，不把下表全部读取。

| 需求 | 资料 |
|---|---|
| 当前里程碑/工程阻塞 | [ROADMAP](ROADMAP.md) |
| V1 转主线与 UI 改造 | [P289](devlogs/10-27-p289-v1-mainline-ui-rework.md) |
| 构建、验证、固化、日志收尾 | [开发流程](specs/misc/development-workflow.md) |
| 接手时检查仓/配置/ELF 身份 | [只读工作区检查器](specs/misc/workspace-check.md) |
| 收尾时查文档失效链接/超长索引/交接缺口 | [文档一致性检查](specs/misc/doc-check.md) |
| 收尾时分别核对代码/文档/会话日志 | [收尾检查器](specs/misc/finalize-check.md) |
| 找源码与理解边界 | [源码地图](specs/misc/architecture-map.md) |
| 排障、实验停止条件 | [methodology](methodology.md) |
| V1 主屏设计 | `specs/ui/`（V1 规格待补） |
| V2 主屏设计（冻结） | [1440×720 规格](specs/ui/home-layout-1440x720.md) |
| 历史坑与适用条件 | [known-pitfalls](specs/misc/known-pitfalls.md) |
| 宠物 | `specs/pet/`；先按具体任务选择产品、架构或资源管线文档 |
| 健康/声音 | `specs/misc/health-assistant-design.md`、`specs/voice/VOICEDESIGN.md` |
| 交付/比赛 | `submission/`，只在提交任务读取 |
| 历史 | `archive/v1/` 为 P1～P243；`archive/v2/` 为 V2 被替代资料 |

主源是本目录和根 AGENTS/devlog；`contest2026_070_arc/docs` 由 `sync_docs.sh` 单向导出，禁止手改。根入口中的 `project_docs/...` 路径按工程根解析；镜像浏览时相应文件在 `docs/...`，镜像不是第二开发入口。

新增规范放在现有导出清单中的 `specs/`；任务详情放 `devlogs/<M-D>-<主题>.md`。历史保留原文但明确失效范围，不能让旧交接命令覆盖现行流程。
