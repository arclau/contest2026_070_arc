# logs/ — AI Coding 会话日志

本目录存放开发过程中与 AI 工具的对话会话，随作品代码一并提交。

## 为什么是"脚本提取"而不是"插件自动写入"（如实说明）

本作品开发**主要使用的 AI 工具是 AtomCode**（AtomGit 的 AI 编码助手）。

- 比赛官方采集器 `contest-log-collector` 支持的工具有限，**枚举为 claude-code / opencode / codex / kiro / mimocode / cursor，不包含 AtomCode**；
- 因此官方插件 / hooks **无法自动把 AtomCode 的会话写入本仓 `logs/`**——不是我们没装采集器，而是**所用工具不在官方支持范围内**；
- 为让评审能看到完整的 AI Coding 过程，本仓用**一次性归一化脚本**把 AtomCode 的原始会话转换为比赛事件 schema 后入仓，`tool` 字段**如实标注为 `atomcode`**，不冒充任何受支持工具；
- 同期使用过的 **OpenCode / Claude Code 属官方支持工具**，其会话由官方 `export-session.py --backfill` 导出，`tool` 标注为 `opencode` / `claude-code`。

> ⚠️ 说明：官方校验器 `validate-log.py` 的 `tool` 枚举不含 `atomcode`，因此 AtomCode 会话会报"枚举不符"错误。这是**"如实标注真实来源"与"官方枚举未覆盖该工具"之间的冲突，不是篡改**。所有会话的 `seq` 单调、内容未经人工改写，可审计（见下方"重新生成/校验"）。

## 来源与统计（共 149 会话）

| 工具 | 会话数 | 来源方式 | 官方支持 |
|---|---|---|---|
| **AtomCode** | **93** | `~/.atomcode/sessions/` → 本仓脚本归一化 | ❌ 不在支持列表 |
| OpenCode | 47 | 官方 `export-session.py --backfill` | ✅ |
| Claude Code | 9 | 官方 `export-session.py --backfill` | ✅ |
| **合计** | **149** | | |

> AtomCode 仅保留**与本作品相关**的会话（工作目录在 `/data/dm`、`/data/openvela`）；已剔除与该作品无关的其它项目会话（armbian / rk3576 / wifitest 等）。

## 目录结构

```text
logs/
└── arclau/                        # GitHub 用户名
    ├── manifest.json              # 会话清单（含每个会话的 tool/日期/file_path/event_count）
    └── <YYYY-MM-DD>/
        ├── atomcode__<sid>.jsonl  # AtomCode 会话（脚本归一化）
        ├── opencode__<sid>.jsonl  # OpenCode 会话（官方 backfill）
        └── claude-code__<sid>.jsonl
```

事件 schema：每行一个事件，字段 `schema_version / session_id / team_id / github_login / tool / seq / ts / role / text|thinking|tool_*`。

## 提取脚本

- `skills/contest-log-collector/scripts/backfill_atomcode.py`：把 AtomCode 的 turn 记录（`user / reasoning / assistant / tools[]`）映射为比赛事件（`user → thinking → assistant → tool`），并生成 `manifest.json`。
- OpenCode / Claude Code 由官方 `skills/contest-log-collector/tools/export-session.py --backfill` 导出。

## 重新生成

```bash
# AtomCode（本项目所用工具，官方不支持，故用此脚本）
python3 skills/contest-log-collector/scripts/backfill_atomcode.py --repo . --output logs

# OpenCode + Claude Code（官方支持，用官方工具）
python3 <contest-log-collector>/tools/export-session.py --backfill --source all --dest . --confirm
```

## 校验

```bash
python3 <contest-log-collector>/tools/validate-log.py logs/
```

预期：OpenCode / Claude Code 会话全部通过；**AtomCode 会话会因 `tool` 枚举不含 `atomcode` 而报错**——即上文说明的"如实标注来源"所致。
