# P301 — WiFi 下远程读日志（adbd TCP + syslog）

日期：2026-10-29｜目标平台：R528 ynm3000（V1）｜状态：编译打包通过，板端已验证（2026-10-29）

## 结论

板子里本来就有 adbd，且 `CONFIG_ADBD_LOGCAT_SERVICE/SHELL_SERVICE/FILE_SERVICE` 全开，只差网络传输。打开 `CONFIG_ADBD_TCP_SERVER` 后，WiFi 连上即可：

```bash
adb connect <板子IP>:5555
adb logcat          # 实时 syslog（驱动/协议栈/应用经 /dev/log 的输出）
adb shell           # 交互 NSH，等价串口控制台
adb pull /data/xxx  # 取文件
```

不需要另写 TCP 日志服务，也不需要 telnetd。

## 为什么 logcat 读的是日志而不是串口

- 本 fork 的 UART 注册名是 `/dev/uart0..3`（`r528_serial.c:arm_serialinit`），没有 `/dev/ttyS*`。
- `CONFIG_RAMLOG_SYSLOG=y` 把 ramlog 注册在 `CONFIG_SYSLOG_DEVPATH="/dev/ttyS1"`（`ramlog.c:ramlog_syslog_register`），路径空闲，注册成功。
- syslog 通道数组 = 默认通道（`up_putc` → 串口）+ ramlog，两份同时写。
- `adb logcat` 打开 `CONFIG_SYSLOG_DEVPATH` 并 `PIPEIOC_POLLINTHRD`（`logcat_service.c`），ramlog 支持该 ioctl（`ramlog.c:635`），读到即推 ADB 帧。

## 改动

| 文件 | 改动 |
|---|---|
| `boards/r528/ynm3000/configs/deskmate/defconfig` | `CONFIG_ADBD_TCP_SERVER=y`、`CONFIG_ADBD_TCP_SERVER_PORT=5555`、`CONFIG_SYSLOG_CHARDEV=y` |
| `boards/r528/ynm3000/src/etc/init.d/rcS.nsh` | `luncher_dm >/dev/log 2>&1 &`（`CONFIG_SYSLOG_CHARDEV` 未开时退回原写法） |

- `/dev/log` 是 syslog 字符设备（0222，只写），应用 printf 经它进 syslog，再分发到串口和 ramlog，两边都能看。
- `bluetoothd` 不需要改：BT 协议栈走 `debug.h` 的 `wlinfo/wlerr`，本来就在 syslog 里。
- 改 `rcS.nsh` 后必须 `touch src/etc/init.d/rcS` 再编，否则 etctmp 不重建（P300 同坑）。

## 证据

- `./build.sh …/deskmate -j$(nproc)` rc=0；`nm nuttx.elf` 有 `adb_uv_tcp_setup` / `syslog_register` / `ramlog_syslog_register`。
- 镜像内字符串：`luncher_dm >/dev/log 2>&1 &`、`tcp server listen error`、`Starting bluetoothd` 各 1 处。
- 镜像 `output/p301-adb-log/ynm3000-adb-log.img`，54,448,128 B，MD5 `519d05e0702032183c9e64e19b4c110d`；`vela.bin == nsh.fex`（`fc46253c…`）。
- 清单：`output/p301-adb-log/verification.json`。

## 链路检查（2026-10-29，不刷机）

按 `nuttx-driver-development` 的审查流程逐跳核对 rcS → adbd → ADB 帧 → logcat_service → ramlog，并在宿主用**真实 microADB 源码**跑通协议。

宿主装置：`output/p301-adb-log/host-test/`（`build.sh` 用宿主 libuv 编译 `adb_client.c`/`hal_uv*.c`/`logcat_service.c`/`shell_service_uv.c`，`stub/nuttx/config.h` 复刻板端宏，syslog 源用 FIFO）。

| 检查点 | 结论 | 证据 |
|---|---|---|
| 宿主 adb 1.0.41 发什么服务名 | `shell:export ANDROID_LOG_TAGS...; exec logcat ...` | 用假 adbd 抓包；命中 `adb_client.c:370` 的 `exec logcat` |
| TCP 传输 + 握手 + 服务分发 | 通 | `adb connect 127.0.0.1:15555` → connected |
| `adb logcat` 流式 | 通 | 3 行输入 3 行到达；300 行突发 300/300、顺序正确、0 丢失 |
| `adb shell` | 通 | `adb shell 'echo shell-works; uname -s'` → 两行正常返回 |
| 无接口时 bind 是否失败 | 不失败 | `tcp_ipv4_bind()` 对 `INADDR_ANY` 跳过设备查找（`tcp_conn.c:329`），adbd 可先于 WiFi 启动 |
| `/dev/ttyS1` 是否被串口占用 | 没占用 | 本 fork 串口注册为 `/dev/uartN`；ramlog 独占该路径 |
| `/bin/sh` 是否存在 | 存在（内建） | `CONFIG_BUILTIN=y`、`sh_main` 已链接；`builtin_loadbinary` 按 basename 查表，不要求文件存在 |
| NSH 重定向 `>/dev/log 2>&1 &` | 安全 | 后台分支不 redirect 父 shell（`nsh_parse.c:582`）；`O_TRUNC` 在 `fs_open.c:305` 被清掉，不作用于字符设备 |

### 审查发现（无 FAIL，3 条 WARN）

1. **adbd 会因 TCP 起不来整体退出**：`adb_hal_create_context()` 里 TCP 失败直接返回 NULL（`hal_uv.c:39`），USB adb 一起没有。端口 5555 正常空闲，接受该风险。板端未做占用端口实验。
2. ~~**logcat 会消费 ramlog**~~：板端复测不成立。ramlog 每个打开者有独立读游标（`ramlog.c` `ramlog_file_open` 把 `rl_tail` 初始化为最旧数据），logcat 读取不推进其他读者。实测 logcat 之后 `adb shell dmesg` 仍返回完整 386 行开机历史。
3. ~~**logcat 只推连接之后的新日志**~~：板端复测不成立。连上即回灌 ramlog 中保留的全部历史（实测首行为 00:00:54 开机日志），随后持续流式；无数据时 poll 不触发仍成立。

已覆盖：NuttX 侧 libuv 套接字与 ramlog poll 的实机行为由 2026-10-29 板端实测确认（见下节）。

## 板端验证（2026-10-29）

用户板 <private-ip>（wlan0，MAC <device-mac>），宿主 adb 1.0.41，未重刷，直接连现有镜像（`uname -a` 构建时间 Oct 29 2026 04:34，与 P301 镜像构建时序一致；镜像 md5 未从板端核对）。

| 项 | 命令 | 结果 |
|---|---|---|
| 连接 | `adb connect <private-ip>:5555` | connected；`adb devices -l` 状态 `device` |
| 重连 | `adb disconnect` → `adb connect` | 再次 connected，`adb shell` 正常 |
| shell | `uname -s` / `uname -a` | `NuttX` / `NuttX 0.0.0 … arm nsh` |
| 进程 | `adb shell ps` | `adbd` 任务 20 在跑 |
| logcat | 8 s 抓取（timeout 结束） | 377 行；首行为 00:00:54 开机 syslog，含 `luncher_dm` 的 LVGL/health/perf 输出；exit 124 为 timeout 正常终止 |
| dmesg | logcat 之后 `adb shell dmesg` | 386 行，仍含完整开机历史（证实 WARN 2 不成立） |
| file | `adb pull /data/dm_time.state` | 1 file pulled，11 B，内容可读 |

补充：`adb shell 'ls /bin/sh'` 报 stat 失败属预期（sh 为 builtin，无 binfs），不影响 shell 服务。

## 设备名来源（待用户确认是否改）

- **adb 侧**：`adb devices -l` 显示 `product:adb dev model:adb_board device:NuttX device`。defconfig 未设 `CONFIG_ADBD_PRODUCT_NAME/MODEL/DEVICE`，取 `apps/system/adb/Kconfig` 默认值（"adb dev" / "adb board" / "NuttX device"），banner 由 `adb_banner.c:72-106` 拼装。
- **DHCP/路由器侧**：路由器设备列表里的 `nuttx` 来自 DHCP hostname。`CONFIG_NETUTILS_DHCPC_HOST_NAME="nuttx"`（`nuttx/.config:4891`，Kconfig 默认），`gethostname()` 为空时由 `dhcpc.c:258-260` 回退发送。
- 改法（需重编 + 重刷）：defconfig 增加 `CONFIG_NETUTILS_DHCPC_HOST_NAME="YNM3000"` 与三条 `CONFIG_ADBD_PRODUCT_*=...`。

## 风险与边界

- `adb_hal_create_context()` 里 TCP bind 失败会让 adbd 整体返回失败（USB 也一起没有）；端口固定 5555，正常不会被占。
- ramlog 100KB 环形，未连主机时旧日志会被覆盖；连上后是持续流，不丢。
- 应用输出多一层 syslog 锁，量级远低于刷屏本身，未做吞吐量测。
- 打包命令必须带 `-t "$PWD"`：否则 `PACK_TOPDIR=none`，PATH 找不到 Allwinner 的 `script` 工具，会误用 util-linux 的 `script` 并挂起（本次踩到）。
