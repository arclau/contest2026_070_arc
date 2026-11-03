# logs/ — AI Coding 会话日志

本目录存放开发过程中与 AI 工具的对话会话，随作品代码一并提交。

## 会话来源（2026-11-02；以 `arclau/manifest.json` 为准）

| 工具 | 会话数 |
|---|---:|
| Codex | 51 |
| CodeBuddy | 40 |
| OpenCode | 62 |
| Claude Code | 6 |
| **合计** | **159**（33,720 条事件） |

日志按本仓 schema 存储，具体会话以 manifest 为准。Codex 转换记录源行号，
工具事件仅保留元信息；对应会话在 manifest 中标为 `degraded`。
CodeBuddy 会话使用本仓扩展的 `codebuddy` 类型。部分 MiMo Coding 会话沿用
旧导出器的 `opencode` 标记，可按源 session ID 辨认。

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

159 个文件、33,720 条事件通过本仓 schema / manifest / seq 检查。
