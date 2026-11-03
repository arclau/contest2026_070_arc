---
name: devlog
description: 查询或维护 DesktopMate 的开发节点、实验交接和验证证据；用于多会话续接、进度查询、会话收尾。
---

# 开发节点与交接

工程 `/data/dm`。先读 `AGENTS.md` → `devlog.md`，再按链接读当前任务；不要加载全部历史。

- `devlog.md` 只作事件索引，最多 40 个节点，每行最多 120 字符。P 编号递增，不与 ROADMAP 的阶段编号混淆。
- 详情写 `project_docs/devlogs/<M-D>-<主题>.md`；只记可追溯的目标、改动、观察、验证和下一步。实验用 `project_docs/methodology.md` 模板。
- 事实、推断、待验证分开。编译、打包、模拟器、板测分别写状态；不得因编译成功写“功能完成”。前人报告未经复现需标明。
- 新会话在首行新增或更新节点；工程焦点变化才同步 AGENTS §五，里程碑变化才同步 ROADMAP。用户要求修改规范时可修改相关正文，不受“两处收尾”限制。
- 失败实验记补丁去留和复测条件，不写“永远不要再查某模块”。下一步只给一个可执行动作及结果分支。
- 超过 40 节点将最早 10 条移到 `project_docs/archive/v2/` 并保留索引；V2 不混入 V1。
- 固化/导出/日志核对按 `project_docs/specs/misc/development-workflow.md`，不以 tag 数量评价进展，不自动全仓提交。
