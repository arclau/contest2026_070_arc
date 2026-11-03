# 收尾检查器（三段式）

入口主源：`/data/dm/contest2026_070_arc/tools/finalize_check.py`。**只读**编排，不写文件、不联网、不构建。把收尾拆成三段**分别报告**，缺一段就明确标 `MISSING`，不用一句“OK”掩盖。

## 日常用法

```bash
# 默认：读 CODEBUDDY_SESSION_ID / CLAUDE_SESSION_ID 作为本次 session
python3 /data/dm/contest2026_070_arc/tools/finalize_check.py

# 指定 session（无环境变量时）
python3 /data/dm/contest2026_070_arc/tools/finalize_check.py --session <id>

# 机器可读
python3 /data/dm/contest2026_070_arc/tools/finalize_check.py --json > /tmp/finalize.json
```

`--root <工程根>` 覆盖自动定位。`--workspace-check/--doc-check/--sync-docs` 覆盖被委派脚本路径（默认取同目录 `workspace_check.py`、`doc_check.py` 与 `/data/vela/sync_docs.sh`）。

## 三段与状态

| 段 | 证据来源 | `OK` | `WARN` | `MISSING` |
|---|---|---|---|---|
| `CODE` | 委派 `workspace_check.py --json` | 无静态问题 | 有需核对项（dirty/配置较新等） | 脚本缺失 |
| `DOCS` | 委派 `sync_docs.sh --check` + `doc_check.py` | 镜像一致且 doc_check 干净 | 镜像有差异或 doc_check 有 finding | 镜像检查脚本缺失 |
| `LOGS` | `logs/*/manifest.json` + `*__<session>.jsonl` | 文件存在、事件数非零、schema 字段齐全、`seq` 严格递增、与 manifest 一致 | 文件未登记/字段缺失/`seq` 非单调/事件数不符 | 找不到该 session 或 session id 未知 |

`ERROR` 仅用于被委派工具无法运行（如 Git 失败、脚本非 JSON 输出）。

## 怎么读结果

- `CODE` 只证明**环境/基线身份**（仓脏否、配置与 ELF 是否在），**不构成编译/打包/运行结论**；`build/pack/run NOT checked here` 是固定声明。
- `DOCS` 只做只读核对，不导出；导出仍用 `sync_docs.sh`。
- `LOGS` 只**报告**本次 session 是否入仓，不代采集。`MISSING` 表示该 session 未写入 `logs/`（例如所用工具未挂 Stop/SessionEnd hook），需人工补导或接受缺口，不能当成“已收尾”。
- 退出码：`0` 三段全 OK；`1` 任一段 `WARN`/`MISSING`；`2` 任一段 `ERROR` 或工具自身失败。

## 验证与维护

```bash
python3 -B /data/dm/contest2026_070_arc/tools/tests/test_finalize_check.py
```

测试用 `/tmp` 桩脚本与桩 `logs/manifest`，覆盖 logs 段（缺失/未登记/0 事件/字段缺失/seq 非单调/事件数不符）、docs 段（一致/DIFF/脚本缺失）、code 段（exit 1/2）、聚合退出码、只读（读前后文件树一致）、跨目录 CLI 与环境变量取 session。

宿主开发工具，不进入固件；改工具后先跑测试，再对真实工作区实测。它**替代** `wrap_session.sh` 里默认的 MiMo 导出步骤（后者未限定工作区，见开发流程）。
