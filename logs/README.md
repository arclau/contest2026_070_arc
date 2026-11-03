# logs/ — AI Coding 会话日志

本目录存放开发过程中与 AI 工具的对话会话，随作品代码一并提交。

## 当前来源（2026-11-02；以 `arclau/manifest.json` 为准）

| 工具 | 会话数 | 状态 |
|---|---:|---|
| Codex | 51 | 公开前逐份筛查；隐去旧工具载荷 |
| CodeBuddy | 40 | 隐去工具载荷和敏感消息块 |
| OpenCode | 62 | 含旧脚本误标来源的 MiMo 会话 |
| Claude Code | 6 | 隐去旧工具载荷 |
| **合计** | **159** | 33,720 条事件；本仓校验器通过 |

Codex 原有共享转换器无法解析 `response_item`。补采脚本按原始顺序保留用户、
助手和工具事件，记录源行号；工具入参/结果一律隐去，敏感消息整块遮盖。
因此补采的 Codex 日志在 manifest 中标为 `degraded`，不能代表完整工具输入输出。

本机有 43 份 CodeBuddy 本工作区会话（约 108 MB）；41 份按原始顺序转换，
2 份没有 cwd 证据而跳过。工具载荷隐去，敏感消息整块遮盖。
本仓 schema 已扩展 `codebuddy`，不代表大赛官方 schema 已支持该工具。
公开前从 268 份中整份移出 109 份：敏感信息、明确的用户操作失误，
以及本次日志管理会话。旧日志的 4,992 条工具载荷、2,234 条思考字段已隐去；
另遮盖 273 个敏感消息块、23 个私人目录路径消息块和 24 个归责表述消息块。
原件与逐份理由、哈希保存在仓外本地工作区（不随仓公开）。
移出的 109 份中有 102 份曾由本地旧分支跟踪；本 PR 候选从上游基点新建。
其中 8 份已存在于上游历史；当前文件删除不清除旧提交。
MiMo Coding 旧导出器曾将来源误标为 `opencode`，需按源 session ID 辨认。
过去的 AtomCode 提取脚本仍保留。

## 目录结构

```text
logs/
└── arclau/                        # GitHub 用户名
    ├── manifest.json              # 会话清单（含每个会话的 tool/日期/file_path/event_count）
    └── <YYYY-MM-DD>/
        ├── codex__<sid>.jsonl     # Codex 会话
        ├── codebuddy__<sid>.jsonl # CodeBuddy 会话
        ├── opencode__<sid>.jsonl  # OpenCode 会话
        └── claude-code__<sid>.jsonl
```

事件 schema：每行一个事件，字段 `schema_version / session_id / team_id / github_login / tool / seq / ts / role / text|thinking|tool_*`。

## 提取与核验

- `skills/contest-log-collector/scripts/export_codex_rollouts.py`：仅收集 session/turn cwd 属于
  本工作区的 Codex rollout；支持 `--dry-run`、`--session <ID>` 和 `--refresh`。
- `skills/contest-log-collector/scripts/export_codebuddy_sessions.py`：仅收集本工作区
  下有 cwd 证据的 CodeBuddy 会话；工具输入/结果不入仓。
- `skills/contest-log-collector/tools/export-session.py`：已有 OpenCode / Claude Code
  staging 会话的官方导出入口；不得用跨工作区的 `--all` 代替定向采集。
- `skills/contest-log-collector/scripts/backfill_atomcode.py`：历史工具，当前 manifest
  未包含其产物。

## 校验

```bash
python3 skills/contest-log-collector/tools/validate-log.py logs/
```

公开前筛查后 159 个文件、33,720 条事件通过本仓 schema / manifest / seq 检查；
本次日志管理会话有意不纳入交付目录。自动扫描无命中仍需在推送前人工复核。
入仓前还需按 `project_docs/specs/misc/development-workflow.md` 扫描凭据；
扫描无命中也不等于可以跳过来源与工作区核对。
