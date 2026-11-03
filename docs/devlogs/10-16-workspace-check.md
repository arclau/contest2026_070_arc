# P256 — 只读工作区检查器落地

> 用户授权：优先完成工作区只读检查器，减少 AI 接手时自行拼凑环境状态。
> 范围：宿主工具、隔离行为测试、使用说明与入口引用。未进行模拟器修复，也未改变 SDK 配置或固件源码。

## 交付

- 主源 `contest2026_070_arc/tools/workspace_check.py`，处于比赛 Git 仓内；本轮未提交，不自动 commit/tag/push。
- 默认六个关键仓，支持 manifest 全量检查、按文件/软链定位 Git 归属。
- sim/board/all 目标：源/生成配置与 ELF 身份、SHA-256、时间、关键宏、ELF 头部架构；缺失/失败/未知明确分开。
- 文本便于人工阅读，JSON 供后续 AI/脚本使用；状态与报告不冒充构建/运行验收。
- [使用说明](../specs/misc/workspace-check.md) 为操作唯一说明；无需安装新依赖或手动设置 PATH。

## 验证

1. `python3 -B contest2026_070_arc/tools/tests/test_workspace_check.py`：10 项测试通过，覆盖实际错误场景和只读约束。
2. 从 `/tmp` 运行 `--all-repos --target all --path vendor/allwinnertech/apps/luncher_dm/luncher_dm.c --json`：本机约 1 秒完成，返回 1（提示需核对事项，非工具执行失败）。
3. 检查前后六个关键仓 Git index/HEAD、SIM `.config` 与 ELF 的 mtime/SHA-256 完全一致。隔离测试还比较整个测试工作区文件树，未发现写入。

本次实测快照（不是永久状态）：

| 项 | 观察 |
|---|---|
| 范围 | manifest 249 项 + 比赛仓，合计 250 项 |
| 仓状态 | 249 项观察成功，`.claude` 项目路径缺失，0 检查错误 |
| 已有改动 | 27 个仓 dirty，说明固定四仓检查会遗漏；dirty 不代表损坏 |
| 模拟器 | 生成 `.config` 的 mtime 晚于 ELF；需核对是否同一构建，未据此断定崩溃原因 |
| 板端 | `nuttx/.config`、`nuttx/nuttx` 缺失；不凭历史打包记录认定当前产物可用 |
| 文件归属 | `luncher_dm.c` 正确解析为 `vendor/allwinnertech` |

完整实测报告临时保存在 `/tmp/dm-workspace-check.json`；事实可能随工作继续变化，下次应重新执行。没有修复/覆盖这些已有状态。

## 边界与后续

本工具不负责启动、修复、镜像同步、凭据扫描、会话日志完整性或多仓快照。当前源码与产物的构建关联仍为 unknown，运行验证为 not_checked。后续模拟器工作按 [P255](10-16-emu-repair-handoff.md) 进行，先用本工具取得环境事实，再保存差异与复现。

宿主工具已通过行为测试与实测，不进入固件，未运行 pack/check_res。文档按单向同步规则导出；日志按当前 session 更新并扫描。
