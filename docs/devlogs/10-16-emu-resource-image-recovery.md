# P266 — 用户重启后资源缺失，保护 userdata 与离线恢复

目标：恢复 goldfish 字体/图标/宠物资源，避免构建再次破坏活动 userdata；随后按真实画面审查 UI。
平台：goldfish ARM64；本轮改动是宿主镜像工具与 Make/CMake 接入，没有修改固件 C 代码或板端配置。
用户已重启并看到 P265 新布局，同时报告资源未挂上、界面难看；这不构成完整运行验收。

## 会话停止交接（2026-10-16）

用户决定结束本轮，明天换 AI 接手。本次仅完成以下记录，不再继续构建、恢复或调整 UI。

- **用户没有认可新界面**，原话是“res分区没挂上。好难看”。资源问题与布局/材质评价分别处理；
  不能假定恢复字体后就满足了美观要求，也不能把 P265 离屏图当作实际画面验收。
- **活动 userdata 尚未恢复**。只验证了新的候选副本；宿主恢复脚本未实际成功执行，不能写资源已恢复。
- **没有拿到资源正常后的实际截图**。当前会话无法连接宿主 X，也看不到宿主模拟器进程；
  宿主脚本仅验证了隔离环境拒绝执行的分支。不要在相同权限条件下反复尝试或据空进程列表判定停机。
- **下一步只有一个**：读 [模拟器资源规范](../specs/misc/emulator-resources.md)，在能控制宿主的环境中，
  核对实例后执行恢复入口。资源正常则查看实际文件页并重审观感；失败则保存首个失败证据，先处理资源。
- 现有源码/配置/镜像全部保留，未提交、未 push、未打稳定 tag。根目录不是 Git 仓，不做全仓清理或恢复。

接手文件：P265 的 UI 实现在 `vendor/allwinnertech/apps/luncher_dm/`；本轮新工具与测试在
`vendor/openvela/boards/vela/scripts/prepare_data_image.py`、`scripts/tests/test_prepare_data_image.py`，
镜像接入在同板 `CMakeLists.txt`、`scripts/Make.defs`；宿主入口在
`contest2026_070_arc/tools/repair_emu_resources.sh`。这些分属不同仓，包含尚未跟踪的新文件。

## 事实与证据

- 新 `/tmp/emu.log` 的宠物首帧 `f0=0`；本会话依然无法连接宿主 X 显示。
- 当前 userdata 根只有 `dm_health.cfg` 和 `pet_state.json`，`data/fonts`、`data/pet` 均不存在。
- ELF 反汇编确认 `board_init_mmio` 的两个 `nx_mount` 调用仍在；资源源目录也仍在。
- FAT 只读检查发现 6250 个无目录入口簇（25,600,000 字节）。
- P265 构建日志含成功复制字体、宠物等记录，而旧模拟器未停机。
- 当时 Make/CMake 每次构建都 dd + mkfs 同一路径，然后 mcopy；这是活动镜像被重建的直接证据。
- 已证实的直接故障是资源目录丢失与 FAT 孤立簇。运行中重建、随后旧文件系统写回是符合证据的机制；
  没有把“所有资源问题”都归因为挂载函数，也没有关闭检查器或添加重复挂载掩盖现象。

原始证据位于 `/tmp/dm-res-p266/`：`emu-resources-excerpt.log`（仅资源相关摘录）、`live-fsck.log`、`stage-repair.log`。
P265 在不能停止宿主旧实例时仍执行了重建数据镜像，这是本轮发现并纠正的执行遗漏。

## 改动

- 新增 `vendor/openvela/boards/vela/scripts/prepare_data_image.py`。
- 默认只初次生成 FAT；已有镜像不写入。新镜像经暂存、资源逐文件回读校验后发布，失败不留半成品。
- `--repair` 必须宿主停机：原镜像复制到暂存副本，FAT 修复、资源覆盖、既有用户文件内容不变校验后，
  留下原镜像备份并原子替换。隔离会话、仍有 PID 文件、镜像仍被 QEMU 打开时拒绝安装。
- `--stage-repair` 只生成新的恢复候选，不修改当前镜像。
- Make/CMake 统一调用；system ROMFS 也改为暂存生成后原子替换。
- `contest2026_070_arc/tools/repair_emu_resources.sh` 提供宿主停机→恢复→重启入口，拒绝其他 QEMU 实例。
- 操作规范唯一入口见 [模拟器资源](../specs/misc/emulator-resources.md)，开发流程引用该规范。

## 实验与验证

| 实验 | 唯一变量与结果 |
|---|---|
| 只读定位 | 不改固件，读取 ELF、资源源目录、FAT 目录与 fsck 结果；区分挂载代码存在与资源内容丢失 |
| 独立恢复 | 只处理新副本；FAT 修复后 fsck 干净，174 个资源文件 SHA-256 与源一致，2 个既有状态文件内容不变 |
| 工具测试 | 7 项真实 FAT 测试通过：目录/中文书名、默认不改字节与时间、修复保状态/备份、失败不发布、宿主停机守卫、孤立簇修复 |
| 宿主入口 | `bash -n` 通过；在当前隔离环境中正确拒绝执行，没有停止或写入宿主活动镜像 |
| goldfish 构建 | 最终成功日志 `/tmp/dm-res-p266/build-sim-final.log`；gen_images 明确输出保留已有 userdata。`build-sim.log` 是失败记录，不能引用为成功证据 |

恢复候选 `/tmp/dm-res-p266/vela_data.recovered.bin` 已校验；没有将运行中的 userdata 换成该副本。
宿主恢复入口会在停机后重新复制当时最新状态，不直接套用较早快照。
工具测试入口：`python3 -B vendor/openvela/boards/vela/scripts/tests/test_prepare_data_image.py`。

交接时产物 SHA-256（重建后须重新核对，不以时间戳代替）：

- goldfish `nuttx`：`0b24a53542c51979cdd516729cb633e28508c1c3ea78e1dce9c40d24d243477c`
- 恢复候选：`c44d0ecb52c4a71ae2963aa78ef9fad7ceda97551fbbf71daf3942623656a249`
- `/tmp` 证据与恢复候选可能随重启丢失；恢复工具可重新生成，勿因此覆盖活动镜像或伪造运行记录。

## 当前边界与下一步

- **宿主安装/重启后资源加载待验**；需要执行资源规范里的一个宿主命令。
- **UI 观感尚未重新验收**：先恢复真实字体/图标/宠物，再依据用户实际画面调整，不能以离屏图判定好看。
- 构建需重新生成 CMake：除 P265 的五项生成头外，还需临时移开 Make 的 `nuttx/.config`，因为上游
  CMake 明确拒绝与它并存；应用 `builtin_list.h/builtin_proto.h` 也会优先覆盖 CMake 注册表，
  初次重编报板端应用符号缺失，隔离这两份生成头后复测。完成后全部恢复。没有 distclean 或清除用户差异。失败链接删除了临时 ELF 时，已先用哈希匹配的 `vela_ap.elf` 恢复有效入口。
- 本轮不涉及共享固件 C 改动，无需重复 R528 打包。P265 的板端产物保持其原验证范围。
- 日志仍须按当前 Codex session 核验，不使用 MiMo 当日导出代替；收尾缺口单列。

收尾：最终 goldfish 构建成功（9 秒增量），ARM64 ELF 已更新；userdata 未执行格式化/资源写入。
当前恢复工具尚未在宿主安装，画面验收仍未完成。源码、文档同步与日志缺口以收尾检查器结果为准。
本次 Codex session `01a144d0-82e8-7950-bf83-15f6269f660c` 尚无对应原始会话日志入仓；缺失须保留说明。
