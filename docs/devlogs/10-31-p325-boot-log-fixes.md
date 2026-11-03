# P325 — 冷启动日志分类与 Wi-Fi 脚本修复

2026-10-31；V1 `ynm3000`。用户要求根据已抓取的 boot logs 修复问题。
基线：`logs/serial/20261031-050733_p317-pet-read.log`，
UTC 14:30:24 与 15:25:16 两次启动；不把 BOOT 多个特征标记当多次物理重启。

## 已定位并修改

现象：两次启动均出现 `nsh: s: command not found`、`P`、`SCII`、`agments).`。
有效配置 `CONFIG_NSH_LINELEN=64`；`nsh_script()` 调用 `readline_fd()`，
`readline_common()` 在读满 63 字节时返回，剩余内容留给下一次读取。
`etc/wifi/start_wifi.sh` 的五行 ASCII 注释长 72～78 字节，
恰好从第 64 字节截出上述命令碎片。历史注释归因于非 ASCII，本次证据支持长度问题。

唯一软件改动：将 V1 `boards/r528/ynm3000/src/etc/wifi/start_wifi.sh` 的长注释
改为三行短注释。可执行命令逐行保持一致，最长行 58 字节。
没有增大 NSH 缓冲、修改 Wi-Fi 时序或关闭日志。
修改前文件：`output/p325-boot-log-fixes/start_wifi.sh.before`。

验证：源码与生成的 `src/etctmp/etc/wifi/start_wifi.sh` 字节一致，
所有行小于 63 字节；旧注释截断尾部与串口错误命令吻合。
官方 V1 编译、打包、配置/入口/启动/121资源/整包校验 PASS。
证据：`output/p325-boot-log-fixes/verification.json`、`build-pack.log`。
新包 `output/ynm3000/ynm3000.img`，MD5=`12d02c0a70f38a643a6fecdc3ca84e43`。
包含 P323 逗猫自然退出、P324 UART1 归还；未刷机，错误消失仍需冷启动复验。

## 其余日志分类

| 日志 | 本轮结论与动作 |
|---|---|
| 蓝牙 H5 open -110 | P324 已停用蓝牙并归还 UART1，等待新包板测；当前旧启动日志仍有错误 |
| SGP30 / TWI2 xfer -5 | 传感器通信失败，已询问硬件是否连接；未获答案，不擅自停用或伪造数据 |
| usb_uv_open -1 2 | USB ADB 打开设备失败，设备节点与 USB 枚举前提待查；未证明 ADB 全部不可用 |
| 天气首次 DNS fail | 14:30 启动中后来请求成功；暂不按持续联网故障改动 |
| RTL872X ioctl error | 启动联网阶段错误，不能仅据天气成功排除所有无线问题，保留观察 |
| echo usrdata mount failed | 是 set -x 脚本回显，未见分支实际输出该错误；不计为挂载失败 |
| not find panic data | 日志说无历史 panic 数据，不是本次发生 panic |
| PMU bus read error | bootloader 阶段访问失败，硬件/板级预期未核实，不改寄存器 |
| secure storage fail / default environment / Failed(-19) | 原始异常保留，尚未证明影响资源挂载或需要擦写；不做破坏性修复 |

下一步：新包冷启动确认命令碎片错误消失；SGP30 按硬件确认结果再定位。
文档已导出；既有文档预算告警、源码镜像映射缺口未混入本轮；对话日志 LOGS=MISSING。
本轮为板端启动脚本修复，未运行模拟器，不 commit/push/tag。
