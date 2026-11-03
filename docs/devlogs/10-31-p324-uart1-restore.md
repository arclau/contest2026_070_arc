# P324 — 蓝牙暂缓，UART1 归还串口调试

2026-10-31，V1 `ynm3000`。用户授权：蓝牙若不是小范围修复，就停止投入并归还 UART1。

## 判断依据

本次冷启动串口（`logs/serial/20261031-050733_p317-pet-read.log`）：
UTC 14:30:37 已打印关闭硬件流控；14:30:40 仍报
`bluetooth driver open timeout` 和 `filep_h5 open ret:-110`。
当前 `rtk_hci.c` 确实清除 CRTSCTS；P300 文档中的“下一步关闭流控”已不适用。
恢复 PG6～9 后仍未完成底层 H5 握手，继续定位需要模块复位/供电/UART 回帧观测。
不能将超时直接归因于某个硬件故障；本轮按投入边界停止，并未宣称蓝牙无法修复。

有限 GitHub 查询：[060 项目](https://github.com/open-vela/contest2026_060_Breezystudio)
README 声称支持 A2DP Sink，但本轮未复现其板端能力；
[062 项目](https://github.com/open-vela/contest2026_062_PharosTech)
记录另一平台的协议实验及产品集成缺口。不能概括为“所有参赛者都没做成”，
也没有据此取得适用于当前 R528 板的可直接采用的小修方案。

## 实施

- `chips/r528/r528_serial.c`：V1 `CONFIG_LUNCHER_DM_APP` 下，
  UART1 恢复 PD21 TX / PD22 RX、mux4；不配置蓝牙的 PG8/PG9 流控脚。
- `chips/r528/r528_boot.c`：V1 跳过 `rtk8723FS_initialize(0)`。
- `boards/r528/ynm3000/src/etc/init.d/rcS.nsh`：V1 不启动 `bluetoothd`。
- 两个共享驱动文件对非 V1 配置保留原分支，未改 V2 应用或配置。
- UART 工具原有 `/dev/uart1`、波特率选择、打开/关闭及收发路径保留。
- 触碰 `src/etc/init.d/rcS` 的时间戳，触发包含 rcS.nsh 的启动资源重建。
- 保留上一轮 P323 逗猫松手 3 秒退出。

前态备份：`output/p324-uart1-restore/before/`。

## 验证与交付

官方 V1 编译/打包、配置/入口/启动/121 资源/整包校验通过。
有效 config 预处理确认 PD21/PD22 mux4；ELF 无 `rtk8723FS_initialize`；
生成后的 etctmp 启动脚本无 `bluetoothd` 启动命令；`vela.bin == nsh.fex`。
证据：`output/p324-uart1-restore/verification.json`、`build-pack.log`。
`git diff --check` 通过；整文件 checkpatch 仍报告旧驱动缩进、长行和混合大小写 API 等风格问题。

镜像：`output/ynm3000/ynm3000.img`；MD5=`f255e96d4ab44e78289139cc36ffa781`。
未刷机、未做 UART 外部回环；不把静态映射核对写成收发通过。
未运行模拟器：本轮为 R528 板端引脚及启动变更。

源码按 sync_r528 导出；已有 4 个 V2 映射源缺失仍保留，
现有 manifest 未覆盖 r528_serial.c 与 ynm3000 启动源，完整源码镜像仍有缺口。
文档按 sync_docs 导出；对话日志 LOGS=MISSING，不以常驻串口日志替代。
不 commit/push/tag。

下一步：刷该包并冷启动，进入 UART 调试工具，用原 BSN20 外设侧接口验收发/回车/退出重开；
开机日志确认不再启动蓝牙 H5 握手。
