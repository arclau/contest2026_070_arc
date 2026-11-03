# 只读工作区检查器

入口主源：`/data/dm/contest2026_070_arc/tools/workspace_check.py`。只依赖 Python 3.9+ 与 Git；脚本从自身位置寻找含 `.repo` 的工程根，支持从其他目录调用。

## 日常用法

```bash
# 快速检查六个关键仓、goldfish 配置与 ELF
python3 /data/dm/contest2026_070_arc/tools/workspace_check.py

# 明确本任务涉及的源码归属，支持多次 --path
python3 /data/dm/contest2026_070_arc/tools/workspace_check.py --path vendor/allwinnertech/apps/luncher_dm/luncher_dm.c

# 全量调查：manifest 项目 + 关键仓，并检查两个目标
python3 /data/dm/contest2026_070_arc/tools/workspace_check.py --all-repos --target all

# 机器可读；需要留证据时由调用者显式重定向
python3 /data/dm/contest2026_070_arc/tools/workspace_check.py --json > /tmp/workspace-report.json
```

`--target sim|board|all`，默认 sim。`--root <工程根>` 可覆盖自动定位；`--path` 相对工程根解析，真实路径不能越出工作区。符号链接按真实文件查所属仓。没有 Git 归属的根文档会显示 unknown，不伪装成比赛仓文件。

默认六仓：`vendor/allwinnertech`、`nuttx`、`apps`、`vendor/openvela`、`prebuilts/emulator/tools`、`contest2026_070_arc`。**默认检查范围不是整个 SDK**；框架/第三方库任务使用 `--path` 或 `--all-repos`。全量模式读取 `.repo/project.list`，报告缺失项目，不自动 sync；manifest 之外且不在六仓/指定路径内的其他独立仓不自动发现。

## 怎么读结果

| 字段/现象 | 含义与行动 |
|---|---|
| `CLEAN` / `DIRTY`、HEAD、changes | 当前 Git 状态；dirty 是已有工作，不能据此 reset/提交。文本每仓最多展示 12 条，JSON 保留全部条目 |
| `OWNER` | 指定文件的真实 Git 仓；别在工程根执行 git 代表所有仓 |
| source/generated config | 配置路径、SHA-256、大小、mtime 与限定关键字段；只比较显式出现的字段，不声称完整 Kconfig 等价 |
| ELF | SHA-256、大小、mtime、头部位数与机器类型；只证明观察到的文件身份，不证明它能启动 |
| config newer than ELF | 配置修改时间较晚，提示核对构建身份；mtime 只是线索，不是配置污染/固件失效的证据 |
| provenance=UNKNOWN | 没有构建清单将当前源码/配置绑定到 ELF；哈希并列也不能证明同一次构建 |
| runtime=NOT_CHECKED | 没启动模拟器，也没做 UI/板端运行验证 |
| missing/error | 文件、仓缺失或读取失败；不会当作干净/通过 |

输出 JSON `schema_version=1`。退出码：

- `0`：所选范围未发现静态问题；**不表示构建、打包、运行通过**。dirty 本身不导致失败。
- `1`：存在缺失或需核对事项，例如配置缺失、目标不匹配、配置时间较新。
- `2`：检查不完整/错误，例如 Git 失败或超时、越界路径、无工作区。

每个 Git 调用最多等待 20 秒，全量仓检查最多 4 个并发。部分失败仍保留其他检查结果。检查不是跨仓原子快照，做复现基线时应停止并发修改；读取文件期间检测到变化会报错。

## 只读范围与限制

- 不调用 build、pack、模拟器、同步/固化脚本；不启动或终止进程，不修改文件、不暂存、不提交、不访问远端。
- Git 关闭 optional locks、fsmonitor、untracked cache 和 lazy fetch，清除继承的 `GIT_*` 仓定位变量，避免读错仓或触发索引刷新/文件监控 hook。
- 只输出选定的非敏感配置值；不输出完整配置、Git remote、diff 正文、凭据或日志正文。报告仍包含文件路径，按本地工程资料使用。
- Git status 不计算 dirty 内容哈希，未跟踪目录可能折叠成一项；不能把 status 条目数量当作文件总数，报告不能替代备份 patch。
- ELF 只读头部识别架构，未核验可执行内容、资源包、实际编译 include 顺序。
- 不检查镜像一致性、凭据内容或会话日志完整性，这些仍按 [开发流程](development-workflow.md) 的对应步骤执行。需要旧 doctor 检查镜像时可另行运行，不把它替代本工具的仓/产物识别。

## 验证与维护

```bash
python3 -B /data/dm/contest2026_070_arc/tools/tests/test_workspace_check.py
```

测试使用 `/tmp` 下独立 Git 仓，验证读前读后文件及 Git index/HEAD 一致、staged rename/特殊文件名、脏状态、manifest/软链归属、缺产物/错架构、较新配置、板端误开 SIM、配置值不泄露、越界路径、Git 超时、fsmonitor 不执行及跨目录 CLI。

修改工具后先运行测试，再对实际目标做只读实测。它是宿主开发工具，不进入固件；无需为工具改动编译/打包 R528。
