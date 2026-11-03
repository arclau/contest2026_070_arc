# P326 — V1 开机提速调查与 DeepSeek 交接

2026-10-31。用户要求先回答猫睡眠规律，再优化开机；随后要求停止实施，交给 DeepSeek。
目标：V1 `ynm3000` 缩短开机等待；验收分别记录桌面可操作、联网可用、AI 可用。
**本轮只有只读调查，没有修改启动代码、配置或重新构建；以下优化均尚未实施。**

## 睡眠问题已答复

当前夜眠从 22:00 后随机延迟 0～29 分钟开始，至次日 06:00；
刚才板端显示在晚间 22 点多，符合频繁回睡的时段。
白天闲置 30～60 分钟后打盹 8～13 分钟，上午、下午各最多两次。
本轮未改宠物睡眠策略。

## 接手基线与约束

- 先读根 `AGENTS.md`、`devlog.md` 与 P325。实际 V1 源码根是
  `vendor/allwinnertech/apps/luncher_dm/`；根 `apps/luncher_dm/` 不存在。
- 当前包 `output/ynm3000/ynm3000.img`，MD5 `12d02c0a70f38a643a6fecdc3ca84e43`。
  含 P323 松手 3 秒结束逗猫、P324 停 BT 并归还 UART1、P325 Wi-Fi 长注释修复。
  三项已编译打包，尚无该包冷启动运行证据；不能把旧日志当成新包效果。
- 串口常驻 `/dev/ttyUSB0`，1.5M；工具 `contest2026_070_arc/tools/dm_serial_log.py`。
  日志 `logs/serial/20261031-050733_p317-pet-read.log`。
  多个 BOOT 特征标记不等于多个物理启动；P322 marker 后也包含后来的启动。
- 保留所有已有未提交改动；不 commit/push/tag。V2 冻结，不改。
- 使用官方 `pack_deskmate.sh ynm3000`；禁用 `/data/vela/pack.sh`。
  sandbox 内 dragon 曾报 Bad system call，需按既有审批规则在外层构建。
- 用户准备收尾，避免扩大为蓝牙、NAND 或 UI 重构。NAND 偶发坏字节根因仍未知，禁止继续应用层补丁。

## 已取到的旧包冷启动耗时

同一串口文件，2026-10-31 **主机 UTC 时间**；板端时钟会被恢复值/NTP 调整，不可混用。

| 事件 | 时间 | 与起点关系 |
|---|---|---|
| BOOT0 starting | 15:25:16.011 | 基准；不等于精确上电电气时刻 |
| U-Boot | 15:25:16.097 | +0.086s |
| NAND U-Boot 初始化结束 | 15:25:16.668 | 初始化约 0.55s |
| USB probe ok | 15:25:16.929 | 后续约 2s 等待 |
| update partinfo | 15:25:18.931 | +2.920s |
| bootloader 分区信息 | 15:25:18.935 | start=0x44400、size=0x19000 |
| dst_addr / jump to rtos | 15:25:27.654 / .656 | 前一阶段约 8.72s，具体读量待核 |
| NuttX 日志开始 | 15:25:28.251 | +12.240s |
| RTK BT 初始化开始 | 15:25:28.728 | 旧包仍有 BT 初始化 |
| late_initialize 结束 | 15:25:31.085 | 随后挂载 resource/data |
| start_wifi | 15:25:32.776 | UI 已并行启动 |
| BT open timeout | 15:25:32.953 | P324 已在源码停用，效果待刷验 |
| ui_home_create | 15:25:32.971 | 仅创建日志，不是可操作验收 |
| lv_nuttx_uv_fb_init 成功 | 15:25:33.553 | +17.542s；仅显示事件循环代理指标 |
| wifi startup done | 15:26:04.345 | Wi-Fi 脚本约 31.57s |
| Boot nsh ok | 15:26:04.347 | +48.336s；不代表 AI 已就绪 |

`15:25:42.318` 的宠物 revision 检查可能来自之后进入锁屏，不能据此认定它阻塞首屏。
SGP30/TWI2 报错在此日志仅数毫秒范围；不要因为报错醒目就当主要启动瓶颈。

## 至多三个候选方向（尚未修改）

### 1. Wi-Fi 固定等待与无条件重复 DHCP：优先做小改

文件：`vendor/allwinnertech/boards/r528/ynm3000/src/etc/wifi/start_wifi.sh`。
当前顺序：disconnect → sleep1 → reconnect → sleep5 → scan → sleep3 →
reconnect → sleep5 → renew → sleep3 → renew → ntpcstop/start → show。
固定 sleep 合计 17s，还有驱动调用/DHCP 等待。

已读 `apps/wireless/wapi/src/wapi.c` 的完整 scan/results/reconnect 函数：
scan 会轮询完成状态，200ms × 最多25次，再收集结果；reconnect 加载配置后调用 associate。
**仍需核对 RTL 驱动 scan 完成语义、associate 返回时机，以及 renew 的实现/返回码。**
不能仅凭调用名就认为可删除 sleep；scan 轮询到限也不一定显式报超时。
可优先评估“首次 DHCP 成功即跳过第二次 renew 和前置 3s；失败保留重试”，
随后再单独评估 scan 后的 3s。一次一个变量，分别验证正常、慢连接/失败情况。
尚未找到/读到 renew 完整实现；不要把本交接当成已经确认可安全删等待。

`src/etc/init.d/rcS.nsh` 已完整读过：UI 在 Wi-Fi 前后台启动，Wi-Fi 脚本前台执行，
ai_agent 在其后启动。历史有 ai_agent 与 Wi-Fi 固件下载并发触发 0x27 的证据，
**保持 AI 在 Wi-Fi 初始化之后，不要为了启动脚本早结束把两者一起后台化。**
P325 已修 NSH `CONFIG_NSH_LINELEN=64` 截断注释问题，新脚本每行仍须小于63字节。
改 rcS.nsh 时注意预处理依赖；此前需 touch 同目录 rcS 才确保重生成，核对 etctmp 实物。

### 2. UI 事件循环前 LED 闪灯测试：约 0.4s 固定阻塞

已读 `luncher_dm.c` 完整 main、LED init/on/off/diagnose/deinit 路径。
main 在进入 LVGL 循环前执行 on → usleep(200000) → off → usleep(200000)。
可评估去掉开机闪灯演示或改为非阻塞；保留初始化、错误处理、诊断和默认关灯状态。
需要确认实际灯状态与销毁路径；不要把删诊断或吞错误算提速修复。
另有 lv_nuttx_init 后 100ms 注释为初始化时序需要，未经实验不要顺手删除。
这是共享应用代码，若修改按开发流程补对应编译/运行验证。

### 3. Bootloader 约 8.72s 区间：收益可能较大，当前证据不足

`lichee/board/r528s3/ynm3000_nand/configs/env.cfg` 中 bootdelay 已是0；
boot_normal 写 `sunxi_flash read 44000000 ${boot_partition} 2000000;boot_rtos 44000000`。
但实机日志提示找不到 env 分区、使用默认环境，运行地址却为 0x41400000。
**不能认定该 env.cfg 生效，更不能直接把 0x2000000 读取长度缩短。**
当前 vela.bin/nsh.fex 均为 8,385,992 字节；这也不证明实际引导只需读同样大小。
已读 `lichee/brandy-2.0/u-boot-2018/cmd/sunxi_flash.c` 的 do_sunxi_flash，
它解析可选 size 后交给 sunxi_flash_read_part；后者还没追完。
已读同目录 sunxi_rtos.c：源码 normal image 分支，不能直接假定压缩镜像启动支持已启用。
需先确认当前预编译 U-Boot、有效默认环境、实际读取长度和镜像头，再决定是否值得动。
USB probe 后约 2s 可能是烧录检测窗口，先查清用途，收尾时不要贸然损坏刷机入口。

## 下一步与交付边界

先获取 P325 包冷启动基线，量出停 BT 后真实收益；若用户不方便刷，继续只读核实 Wi-Fi 返回语义。
再选一个最小改动，保留原文件/差异，官方构建并核 `vela.bin == nsh.fex`，
以同一路由/网络条件冷启动对比。设备 reboot 已知可能挂死，用断电重上电。
只把实测结果写“提速”，固定等待减少量与预计收益单独标注。

本轮交接为文档任务：不构建，不出新固件。文档检查有19项既有告警；
本次 Codex session `01a19255-c227-7db3-9182-3ab37edc31c0` 对话日志尚未入仓，
不得拿串口采集文件或手写总结充当对话原始日志。
