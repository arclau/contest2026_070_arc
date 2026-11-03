# P365 会话日志补采与 CodeBuddy 可行性评估（2026-11-02）

## 目标与范围

- 目标：补采 `/data/dm` 的真实 AI Coding 会话；核对来源、事件数、顺序和隐私。
- 平台：本机 Codex rollout、CodeBuddy CLI 存档；目的地为专属仓 `logs/arclau/`。
- 验收：manifest 与 JSONL 一致，当前 session 含用户/助手/工具记录且序号连续。

## Codex 事实与处理

- 补采前 manifest 有 188 份会话，其中 Codex 94 份，最近采集时间为 10-16。
- 本机识别到 49 份 cwd 属于 `/data/dm` 的 Codex rollout；另 22 份属于其他工作区。
- 共享转换器只认 Claude 风格 `.message.content`，不能解析 Codex `response_item`。
- 新脚本 `skills/contest-log-collector/scripts/export_codex_rollouts.py` 按源行顺序转换。
- 工具输入/结果一律隐去；敏感消息整块遮盖，保留源行号，manifest 标 `degraded`。
- 首次写入 39 份、6,550 条事件；5 份缺用户/助手/工具角色，4 份含多 session ID 而跳过。
- 本次 session ID `01a19c5c-4c0a-7ce2-9632-34610f3ebe01` 已入仓，cwd 为 `/data/dm`。
- `validate-log.py` 对全仓 227 份、49,478 条事件通过；凭据格式扫描命中 0。
- 当前会话仍在进行，终轮 `--refresh --session` 后事件数以 manifest 为准。

## CodeBuddy 补采

- 本机 `~/.codebuddy/projects/data-dm/` 有 43 份 JSONL，约 108 MB；session ID 与文件名一致。
- 43 份记录中显式 cwd 均为 `/data/dm`；合计消息 2,897、工具调用 7,915、结果 7,900。
- 格式是 `message / function_call / function_call_result`，可确定性转换；现行共享转换器不支持。
- 本仓 schema 已扩展 `codebuddy` 工具名和路径规则；不冒用其他来源，官方兼容性未确认。
- 原文筛查有大量凭据/地址/个人信息样式命中；10 份零命中也不能单凭正则证明安全。
- 本轮入仓 41 份、18,712 条事件，敏感消息 420 块遮盖，工具入参/结果一律隐去。
- 2 份无 cwd 证据跳过；268 份/68,241 条事件通过本仓校验器，凭据扫描命中 0。
- CodeBuddy 原始 JSONL 未复制。官方 [`/export`](https://www.codebuddy.cn/docs/cli/slash-commands)
  可导出单会话，归档仍需隐私审查。

## 后续

- 上述 268 份在 P367 全部恢复；后续公开前筛查见 [P368](11-02-p368-public-session-review.md)。
- 专属仓远端当前为公开仓；日志仅写入本地工作树，未推送。官方 schema 兼容性待确认。
- 本轮没有 commit、push、上传，也没有改动冻结的产品源码。
