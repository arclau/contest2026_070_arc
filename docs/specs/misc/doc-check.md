# 文档一致性检查器

入口主源：`/data/dm/contest2026_070_arc/tools/doc_check.py`。只依赖 Python 3.9+；**只读**，不写文件、不用 Git、不联网、不构建。从自身位置向上寻找含 `project_docs/` 的工程根，支持从其他目录调用。

## 日常用法

```bash
# 默认：根 AGENTS.md / devlog.md + project_docs/（不含 archive/、submission/）
python3 /data/dm/contest2026_070_arc/tools/doc_check.py

# 机器可读；需要留证据时由调用者显式重定向
python3 /data/dm/contest2026_070_arc/tools/doc_check.py --json > /tmp/doc-report.json
```

`--root <工程根>` 可覆盖自动定位。

## 检查内容

| 类别 | 含义 |
|---|---|
| `link` | 相对 Markdown 链接目标不存在，或链接越出工作区 |
| `handoff` | devlog 索引引用的交接文件缺失；或 `devlogs/` 下有文件未被 `devlog.md` 引用 |
| `reference` | `[详情][pX]` 等引用使用没有对应的 `[pX]:` 定义 |
| `budget` | 索引超预算：`AGENTS.md` ≤ 8KB；`devlog.md` ≤ 40 行、每行 ≤ 120 字符 |

预算取自 AGENTS.md §四，是唯一规则源；超限先在主源整理，再导出。

## 怎么读结果

- 只查根 `AGENTS.md` / `devlog.md` 与 `project_docs/` 下的 Markdown；`archive/`（历史）与 `submission/`（第三方官方资料）不在维护范围，其失效链接不报。
- 不跟随外部 URL、不校验锚点、忽略围栏代码块与行内代码中的类链接文本。
- 只做静态文本检查，不渲染、不预览，不证明文档语义正确或镜像已同步。
- 输出 JSON `schema_version=1`。退出码：
  - `0`：所选范围未发现问题。
  - `1`：有需处理项（失效链接、超预算索引、交接缺口）。孤立 devlog 是提示，需人工判断是补索引还是归档。
  - `2`：检查不完整（无 `project_docs`、文件读取失败、链接越界）。

## 验证与维护

```bash
python3 -B /data/dm/contest2026_070_arc/tools/tests/test_doc_check.py
```

测试用 `/tmp` 独立文档树，覆盖失效/外部/锚点/越界链接、围栏与行内代码忽略、行号准确、预算超限、引用缺失与悬空、孤立 devlog、`archive`/`submission` 排除、只读（读前后文件树一致）、跨目录 CLI。

它是宿主开发工具，不进入固件；改工具后先跑测试，再对真实文档实测，无需编译/打包 R528。
