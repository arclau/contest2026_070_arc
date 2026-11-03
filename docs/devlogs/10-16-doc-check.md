# P257 — 只读文档一致性检查器落地

> 用户授权：继续上一架构师规划，第二项「文档一致性检查」。范围：宿主工具、隔离测试、使用说明与入口引用。未修模拟器、未改 SDK 配置或固件源码、未提交。

## 交付

- 主源 `contest2026_070_arc/tools/doc_check.py`，位于比赛仓；本轮未 commit/tag/push。
- 三类检查：失效相对链接、超预算索引、交接文件缺口（引用缺失/悬空、孤立 devlog）。
- 扫描范围 = 根 `AGENTS.md`/`devlog.md` + `project_docs/`（排除 `archive/`、`submission/`），与 `sync_docs.sh` 的导出集一致；预算取自 AGENTS.md §四。
- 文本便于人工阅读，JSON 供后续 AI/脚本使用；只读，不写文件、不用 Git、不联网。
- [使用说明](../specs/misc/doc-check.md) 为唯一操作说明；`README.md`、开发流程、AGENTS §三 已最小接线。

## 验证

1. `python3 -B contest2026_070_arc/tools/tests/test_doc_check.py`：10 项测试通过（含行内代码忽略、围栏后行号仍准确）。
2. 实测 `/data/dm`：27 份文档，发现 1 项真实问题——`project_docs/devlogs/10-14part1devlog.md` 未被 `devlog.md` 索引（见下）。
3. 只读核验：运行前后 27 份文档的大小、mtime、SHA-256 完全一致；隔离测试另比较整棵测试文档树。

## 实测发现（未擅自改）

`project_docs/devlogs/10-14part1devlog.md` 自述「索引见 devlog.md（P243）」，但 V2 索引表只到 P244，且 `specs/ui/home-layout-1440x720.md`、`10-15-goldfish-luncher-dm.md` 都引用「devlog.md P243」。该文件是 V1→V2 过渡期遗留：直接补进 V2 表会混入 V1，移动到 `archive/v1/` 属内容取舍。**保持原状，标记为待用户决定**；工具退出码 1 即提示此项。

## 边界与后续

不做镜像同步（另用 `sync_docs.sh --check`）、不渲染/预览、不校验外部 URL 与锚点、不检查凭据或会话日志完整性。第三项「收尾检查改造」（分别报告代码验证/文档同步/会话日志）尚未开始。宿主工具不进入固件，未运行 pack/check_res；文档按单向同步规则导出。
