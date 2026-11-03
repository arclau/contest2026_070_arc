# P258 — 三段式收尾检查器落地

> 用户授权：继续上一架构师规划，第三项「收尾检查改造」。范围：宿主只读工具、隔离测试、使用说明与入口引用。未修模拟器、未改 SDK 配置/固件源码、未提交、未代采集日志。

## 交付

- 主源 `contest2026_070_arc/tools/finalize_check.py`，位于比赛仓；本轮未 commit/tag/push。
- 把收尾拆成三段**分别报告**：`CODE`（委派 workspace_check）、`DOCS`（委派 sync_docs.sh --check + doc_check）、`LOGS`（按 session id 核 `logs/` manifest 与 JSONL）。
- 每段状态 `OK/WARN/MISSING/ERROR`，缺一段明确标 `MISSING`；退出码 0/1/2。只读，不写文件、不联网、不构建。
- [收尾检查器使用说明](../specs/misc/finalize-check.md) 为唯一操作说明；`README.md`、开发流程 §6、AGENTS §三 已最小接线。

## 验证

1. `python3 -B contest2026_070_arc/tools/tests/test_finalize_check.py`：12 项测试通过。
2. 实测 `/data/dm`（session `01a1435f-…`）：`CODE WARN`（5/6 仓 dirty，workspace_check exit=1）、`DOCS WARN`（镜像一致但 doc_check 有 1 项孤立 devlog）、`LOGS MISSING`。
3. 只读核验：运行前后 `project_docs/` 与 `logs/` 共 646 个文件的大小、mtime 完全一致。

## 关键发现（本次未修）

`LOGS MISSING` 是真实缺口：本次为 **CodeBuddy** session（`CODEBUDDY_SESSION_ID`），而采集器的 Stop/SessionEnd hook 只装在 `~/.claude/settings.json`；`~/.codebuddy/settings.json` 无 hook，`logs/` 中今日只有 `codex__*` 与 `opencode__*`。即**本次 CodeBuddy 会话未被自动采集**。工具如实标 `MISSING`，不冒充已收尾；是否补导或为 CodeBuddy 挂 hook 属后续决策（本工具不代采集）。

## 边界与后续

不代采集、不导出镜像、不构建。`CODE` 段只证明环境/基线身份，`build/pack/run NOT checked here`。原 `wrap_session.sh` 第 7 步（未限定工作区的 MiMo 导出）仍存在，本工具不调用它，可作为其替代报告；实际修复 wrap 属脚本改动，须独立验证。三项工具（P256 工作区、P257 文档、P258 收尾）均为宿主只读工具，不进固件。
