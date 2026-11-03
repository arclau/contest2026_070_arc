# DesktopMate 开发流程（P254 现行）

适用于 `/data/dm`。本文件负责构建、验证和交付；实验方法见 [methodology](../../methodology.md)。命令按本地脚本核对，本次文档审计未重新构建固件。

## 1. 明确交付结果

工作卡写在任务 devlog，简单任务可简写：

```text
目标 / 用户可见验收：
平台：DOC / SIM / BOARD / 共享代码
相关文件与所属仓：
开始状态：HEAD + 已有差异；需保留的临时补丁
本轮改动边界：
验证命令与证据文件：
停止条件 / 下一步：
```

根目录不是 Git 仓。对涉及文件用 `git -C <所在目录> rev-parse --show-toplevel` 找归属，再记 `status --short`、`diff --stat`、HEAD。源码通过软链出现时用 `readlink -f` 核对，不能凭相似路径断言是两个副本或硬链接。

优先使用 [只读工作区检查器](workspace-check.md) 获取仓、配置、ELF 身份及缺口。默认只查六个关键仓与 SIM，其他源码用 `--path`，全量用 `--all-repos`；报告不是差异备份或运行验收。

关注 `vendor/allwinnertech`（应用/板端）、`nuttx`（内核）、`apps`（框架）、`vendor/openvela`（goldfish 配置）、`prebuilts/emulator/tools`（启动）、`contest2026_070_arc`（导出/skills/日志）。按本轮实际影响增减，不能假定“四仓”完整。

## 2. 验证矩阵

| 改动 | 开发中最小验证 | 交付门槛 |
|---|---|---|
| Markdown / skill 指令 | 链接、路径、事实、冲突、长度；可用 [文档一致性检查](doc-check.md) | docs 导出一致；无须固件构建 |
| 宿主开发工具 | 隔离行为测试、实际目标行为检查 | 验证只读/作用范围与错误处理；未改固件代码/配置无需打包 |
| 仅 goldfish 启动/配置 | goldfish 编译或对应脚本检查 + 启动证据 | 对应故障消失；不能写“板端通过” |
| 共享 UI/服务 C 代码 | goldfish 编译 + 目标操作 | 再过板端 `pack_deskmate.sh` + `check_deskmate.py`；板测未做单列 |
| 板端驱动/配置/资源 | 对应目标编译，核对有效配置 | `pack.sh` + `check_res` + 所需硬件验证；无硬件标待验 |
| 实验补丁 | 对应平台同一基线 A/B 对照 | 只记录实验结果，不产生“已修复/稳点” |

验证分开写：**静态检查 / 编译 / 打包资源 / 模拟器运行 / 板端运行**。NSH 只证明系统启动；截图非黑只证明有像素；均不能证明应用功能通过。

## 3. goldfish

工作目录 `/data/dm`；配置在 `vendor/openvela`，输出为 `cmake_out/vela_goldfish-arm64-v8a-ap`。

```bash
./build.sh vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap --cmake -j"$(nproc)"
```

板配置在 `--cmake` **前面**。P252 的 `./build.sh --cmake <路径>` 失败不能证明正规入口不可用；本地 `build.sh` 先取首参为配置，再解析 `--cmake`。不要因那次失败永久改成裸 `ninja`。

- 当前 goldfish defconfig 编 **V1 `luncher_dm`（1920×1200）**。切 V2：把 `CONFIG_LUNCHER_DM_APP/SIM` 改成 `CONFIG_LUNCHER_DM_V2_APP/SIM`，entrypoint 改 `luncher_dm_v2_main`，重编。
- 分辨率由 `run-emu.sh` 写 `config.ini` / `hardware-qemu.ini` 并起 Xvfb（V1：1920×1200 / Xvfb 2100×1400）。改分辨率必须重启 Xvfb；`stop` 已一并杀 Xvfb。
- **CMake 前置**：源码树不能有 `nuttx/include/nuttx/config.h` 和 `nuttx/include/arch`（板端 make 生成物，会让 arm64 命中 arm32 头）。先备份再移走；不要直接删。

- 常规构建只初次创建 userdata；已有镜像保留。资源恢复必须宿主停机并校验，见 [模拟器资源](emulator-resources.md)。
- 重建数据镜像前确认占用该输出目录的模拟器已停止，核对 PID/命令，不用跨工作区 `pkill -f`。当前 `run-emu.sh stop` 仍有宽泛 kill，使用前确认没有其他实例。
- 保留当前 `.config` 实验而用 CMake/Ninja 缓存时，记录工具链 PATH、缓存目录和生成配置；这是有前提的增量路径，不是另一套默认入口。
- 改配置先读 `kconfig-tweak`，再生成配置并核对有效 config/autoconf 与真实编译命令；避免重复追加 `CONFIG_*`。
- **先证实配置污染再清理**：查架构、SIM、LVGL 配置及实际 include 顺序。历史上源码树 config.h 曾抢先命中；P254 审计时 `nuttx/include/nuttx/config.h` 不存在，不能把历史状态当现状。手工 `-D` 不能证明所有翻译单元配置一致。
- 不把 `distclean`、删除整个输出目录作为普通重试。先保存 `.config`、ELF、最早异常日志、差异及哈希，再按证据清理最小产物。
- 启动入口 `./emulator.sh cmake_out/vela_goldfish-arm64-v8a-ap/`；`/data/vela/run-emu.sh` 管理 Xvfb/VNC，但 GPU 修复未证实，不能把“started”当验收。
- **远程看屏**：首页 `/deskmate/` → nginx `vela-nav` 302 → `/vnc.html?host=<LAN-IP>&port=8080`。nginx 把 `/vnc.html`、`/app/`、`/core/`、`/vendor/`、`/websockify` 反代到 `127.0.0.1:8080`，所以 `websockify` 必须监听 **8080**（写 6080 会让首页一点就白屏）；换端口要同步改 `run-emu.sh` 与 nginx 站点配置。
- `config.ini` / `hardware-qemu.ini` 属于生成/运行状态。查最终启动参数、实际日志及模板；不反复改生成文件，不假定运行中改 ini 或 HUP 能热重载。

运行证据对应同次构建的 ELF 哈希、配置、启动命令和首个异常。地址反解只用该 ELF；`??` 表示符号证据不足，不自动等于函数指针损坏。

## 4. R528

固件统一走 SDK 官方流程（P296，2026-10-28 起）：

```bash
cd /data/dm
./build.sh vendor/allwinnertech/boards/r528/<产品>/configs/deskmate distclean   # 全量重编时加
./build.sh vendor/allwinnertech/boards/r528/<产品>/configs/deskmate -j$(nproc)
cd vendor/allwinnertech/lichee
bash tools/scripts/pack_img.sh -c sun8iw20p1 -p rtos -b <PACK_BOARD> \
  -o nuttx -d uart0 -s none -m normal -w none -v none -i none \
  -t "$PWD" -f r528s3/<产品>_nand -g r528s3/<产品>_nand
```

| 产品 | config 目录 | PACK_BOARD |
|---|---|---|
| V1 主线 | `ynm3000`（1200×1920 + GT9271） | `r528s3-ynm3000` |
| V2 冻结 | `ynm3000_v2`（1440×720 + HX83102B） | `r528s3-ynm3000-v2` |

- 产物 `lichee/out/r528s3/<产品>_nand/rtos_nuttx_<PACK_BOARD>_uart0_128Mnand.img`；核 `vela.bin` 与 `nsh.fex` md5 一致、`res.fex` 现做自 `board/r528s3/<产品>_nand/data/res`。
- **`-b` 必须显式带 `r528s3-` 前缀**：`pack_img.sh` 的 128M nand 特判只认该名；`lunch_nuttx` 菜单里项目名是 `ynm3000`，走 `pack()` 会传成 256M 布局，项目路径也缺 `r528s3/`。
- `distclean` 后 `.config` 由 defconfig 重建，此前手改的选项会丢。
- `pack_deskmate.sh` 保留为一键包装（同一 `pack_img.sh` + `output/deskmate-build.lock` + `manifest.json` + `check_deskmate.py`）：需要哈希清单或两产品串行时用它，不是另一套打包逻辑。

**禁用 `/data/vela/pack.sh`**：它硬编码 `r528s3-gemini-s1/configs/deskmate`——1440×720 + V2 app + 无触摸驱动的漂移配置，产物没有 `/dev/input0`。`lunch_nuttx 5` 同样取错 V2 项目路径。

切换平台前保存配置及差异；禁止两目标同时修改同一生成状态。模拟器调试中不为每轮实验切回 R528；共享代码最终交付再验板端。资源检查不能替代新面板、触摸、音频实机验证。

## 5. 固化与恢复

**旧 `git_snapshot.sh` 暂停自动调用。** 实读发现：

- 固定四仓 `git add -A`，会夹带用户改动与实验补丁；漏掉 `vendor/openvela`、模拟器工具仓。
- 同步/提交失败未可靠中止，仍可能继续打 tag 并打印完成。
- `ok-*` 未绑定运行证据；恢复仅 reset 四仓，不能恢复外部脚本、主源文档和完整 SDK 状态。

修复前记录 HEAD、文件差异、验证状态即可，不强制提交。需提交时仅暂存审查过的本轮文件，核对暂存 diff，逐仓确认成功；未闭环实验用 WIP 描述，不打 `ok-*`。按矩阵验证后才可标对应目标可用。

工具后续修复验收：覆盖实际受影响仓；记录 HEAD/dirty/config/产物哈希/验证范围；失败即停；不收无关文件；恢复先预览备份。多仓中途失败须报告完成与未完成仓，不宣称原子回滚。

## 6. 文档、日志与收尾

收尾先跑 [收尾检查器](finalize-check.md)，分别看 `CODE`/`DOCS`/`LOGS` 三段；任一段 `MISSING` 都要写明缺口，不能以一句“OK”收尾。`LOGS` 只报告本次 session 是否入仓，不代采集。

1. 更新任务详情与 devlog 一行；焦点/里程碑变化才更新 AGENTS §五/ROADMAP。“只改两处”不限制用户要求的规范修订。
2. P370 后 `devlog.md` 与 `project_docs/devlogs/` 仅本地维护，不再导出专属仓。其他公开文档用
   `bash /data/vela/sync_docs.sh` → `bash /data/vela/sync_docs.sh --check`；新规范放进已导出的 `specs/`。
3. 代码交付才执行 `sync_r528.sh` 并核对差异；文档任务记录已有源码镜像漂移，不混入未闭环补丁。rsync 带 `--delete`，先核对目标独有改动。
4. 按当前工具、session ID 导出，核对来源 cwd 属于 `/data/dm`。MiMo 使用 `--directory` / `--session` 限定；不用跨工作区 `--since` / `--all` 代替本次导出。
   当前 Codex `response_item` 用专属仓 `skills/contest-log-collector/scripts/export_codex_rollouts.py --session <ID>` 定向补采；
   该脚本隐去工具输入/结果，manifest 标为 `degraded`，不能宣称完整工具内容已采集。
   CodeBuddy 用同目录的 `export_codebuddy_sessions.py --session <ID>`；本仓 schema 已扩展真实工具名，
   官方兼容性待确认。两种脚本都不自动 commit 或 push。
5. 核对 manifest/JSONL：session ID 一致、事件数非零、序号连续、含用户/助手/工具记录。hook 未验证写“待采集”，不能因历史日志存在写已完成。
6. 导出后扫描；命中时停止后续提交/上传，只报告文件名。正则无命中不能证明覆盖所有隐私，仍须核对采集范围。

```bash
rg -l 'nvapi-[A-Za-z0-9_-]{10,}|AIza[0-9A-Za-z_-]{20,}|sk-[A-Za-z0-9_-]{20,}|ghp_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{20,}|AKIA[A-Z0-9]{16}|LTAI[A-Za-z0-9]{20,}|AKLT[A-Za-z0-9]{20,}|Bearer[[:space:]]+[A-Za-z0-9._+/=-]{20,}|(^|[^a-zA-Z])ss://[A-Za-z0-9+/=_-]{10,}|vmess://[A-Za-z0-9+/=_-]{10,}|trojan://[A-Za-z0-9+/=_:@.-]{10,}|vless://[A-Za-z0-9+/=_:@.?&=-]{10,}' contest2026_070_arc/logs/
```

`rg` 返回 1 是无命中，2 是扫描失败，不可混淆。保留了旧 AGENTS 的凭据类型。

`wrap_session.sh` 第 7 步执行 `export_mimo_db.py --since 当日`，未限定工作区，也不能采集当前其他工具。修复前以上六步等价收尾，记录跳过原因。doctor 的镜像差异、构建故障、凭据命中分别处理。
