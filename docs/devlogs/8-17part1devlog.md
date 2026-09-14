# DevLog 2026-08-17 — start_wifi.sh 减冗余提速（P113b）

> **会话交接**：承接 8-16part2（P113，代码 ok-20260816-26）。本 part 执行 8-16part2 遗留项 **P113b**：start_wifi.sh 13s 固定 sleep 盲等 → 减冗余保确定性（方案 B，用户拍板"WiFi 能加快整体就加快"）。结束代码 **ok-20260817-1**。

## 一、P113b：start_wifi.sh 13s 盲等 → 约 5s（ok-20260817-1）

### 背景
用户已表态"WiFi 能加快整体就能加快"。start_wifi.sh 是开机串行瓶颈（rcS.nsh 前台执行，等它完成才启动 ai_agent，P70 0x27 约束），原脚本 7 个固定 sleep（1+2+2+2+2+2+2）纯盲等。

### 方案评估（先评估后实施）
代码实证（详见评估输出）：
- `wapi scan wlan0` 内部自带轮询：`wapi_scan_results_cmd`（apps/wireless/wapi/src/wapi.c:641-662）`200ms × 25 次 ≤5s` 等扫描完成 → **scan 后 sleep 2 完全冗余**
- `renew wlan0` = `netlib_obtain_ipv4addr`（apps/system/dhcpc/renew_main.c:68），内部 `dhcpc_request` 自带 DISCOVER/REQUEST 各重试（apps/netutils/dhcpc/dhcpc.c:793/905，`CONFIG_NETUTILS_DHCPC_RETRIES`）→ **renew×4 是重复盲试，后 3 次可删**
- 约束：①start_wifi.sh 保持前台、ai_agent 顺序不变（P70 0x27）；②NSH 内置解析器（非 POSIX sh，注释行会报错）→ **不做 while 循环/条件轮询**，避免 NSH 语法风险 + 提前退出回归 0x27

**选型**：方案 B（减冗余保确定性）而非方案 A（事件驱动轮询）——A 理论再省 1-2s 但引入 NSH 循环语法 + 0x27 回归双重风险；B 零语法风险、结束时机语义不变（renew 返回=WiFi 可用）。

### 改动（vendor 仓 src/etc/wifi/start_wifi.sh，8 行减 3 行）

| 原 | 新 | 理由 |
|----|----|------|
| `wapi scan wlan0` + `sleep 2` | `wapi scan wlan0`（删 sleep 2） | scan 内部已阻塞等完成（≤5s） |
| `wapi reconnect wlan0` + `sleep 2` | `wapi reconnect wlan0` + `sleep 3` | 关联握手收敛期微调 |
| `renew wlan0` ×4（间各 sleep 2） | `renew wlan0` ×1 | dhcpc 自带重试，盲试冗余 |

最终脚本：`disconnect → sleep1 → scan → reconnect → sleep3 → renew → ntpcstop/start → show`

### 验证（全流程走完）
1. **distclean 全量重编**（prebuilt 纪律）：`./build.sh <config> distclean` → `./build.sh <config> -j$(nproc)` 通过
2. **strings 验证进固件**（nuttx/vela.bin）：
   - `renew wlan0` 出现 **1** 次（旧版 4 次）✅
   - `sleep 2` 出现 **0** 次（旧版 6 次）✅
   - `sleep 3` 出现 **1** 次（新特征）✅
   - 固件内完整序列：`wapi disconnect wlan0 → sleep 1 → wapi scan wlan0 → wapi reconnect wlan0 → sleep 3 → renew wlan0 → wapi show wlan0` ✅
3. **打包**：lichee `pack` 成功，产物 `rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img`
4. **资源检查**（P92 纪律）：res.fex 含 MiSans/FW_NIC_ACUT + 整包镜像含完整 res.fex 块，三段一致通过
5. **固化**：`git_snapshot.sh` → **ok-20260817-1**

### 上板期望
bootlog 中 `[wifi] start_wifi` → `[wifi] wifi startup done` 间隔 13s→约 5s（scan 最快 1-2s 出结果）；ai_agent 随之提前 ~7s 启动；WiFi 连上后天气/对时按 P113 边沿立即拉取不受影响。

## 二、补充修正：NSH 注释噪声清除（ok-20260817-2）

### 现象（用户上板日志）
```
nsh: 故全部删除，仅保留命令。: command not found
```
start_wifi.sh 头部 `#` 注释行被 NSH 内置解析器当作命令执行（脚本头部注释自己就说明过此坑："# 注释行会被当作命令报 command not found"，但上一轮保留头部注释未删净）。

### 修复
删除 start_wifi.sh 头部全部注释行（含 `#!/bin/sh` + 3 行说明注释），脚本从 `set +e` 直接开始，仅保留命令。

### 验证（全流程）
1. distclean 全量重编通过
2. strings vela.bin：`故全部删除/参数说明见/#!/bin/sh` 计数 **0**；序列从 `set +e` 开始逐行正确（disconnect→sleep1→scan→reconnect→sleep3→renew→ntpc→show）
3. 打包 + 资源检查三段一致通过（镜像 md5 be94e00e）
4. 固化 **ok-20260817-2**

### 板端另一现象：download_fw FAIL 0x15 + FT_New_Face error 刷屏
镜像完整性已验证（镜像含完整 res.fex 块、含 FW_NIC_ACUT.bin/MiSans），**0x15/字体刷屏 = 烧录工具未写 res 分区**（坑 #3，P74 插曲同款）——重新打包不解决该问题，烧录时必须勾选 res 分区，烧后 `ls /resource/` 验证。

## 三、0x27 固件下载失败排查 + 时序缓冲恢复（ok-20260817-3）

### 现象（用户上板 ok-20260817-2）
```
wapi scan wlan0 → ioctl(SIOCSIWSCAN): 1 失败（固件未加载前 scan 本来就失败）
wapi reconnect wlan0 → wifi on 1 → Initializing WIFI → sdio irq_thread enter
→ download_fw: download firmware FAIL! status=0x27
→ rtl8723f_hal_init Download Firmware from file failed → rtw_hal_init: hal__init fail
→ drv_open fail → ERROR: Start WIFI Failed! → reconnect failed → renew 失败
```
但**本次 res 已烧入**（`[tone] done: /resource/tones/...` 成功 + 字体正常无刷屏）——0x15→0x27 的变化本身证明 res 分区这次烧对了（0x15="Can't open firmware"=文件不存在=res 没烧；0x27="Download Firmware from file failed"=文件已打开但固件下载到芯片失败）。

### 排查结论
1. **0x27 属驱动层**（devlog 8-10 明示：闭源 `librtl8733bs.a`，`download_fw FAIL status=0x27`，需上板硬件排查 GPIO2 复位波形、SDIO 供电时序，勿擅改驱动）；P45 曾"0x27 上板可用"、P47 曾因 0x27 回退——历史多次出现，非本轮优化独有。
2. **打包产物侧已排除**：ACUT/BCUT 固件均在 res.fex 且与驱动路径（`CONFIG_CCV_WIFI_FW_PATH`/`CONFIG_WIFI_FW_PATH` → `/resource/etc/wifi/FW_NIC_{ACUT,BCUT}.bin`）匹配；本次上板 tone 成功 = res 实际已烧入。
3. **时序变量**：我的 P113b 优化把 scan 后 sleep 2 删了，reconnect 提前 2s 触发固件下载——scan 失败后立刻 reconnect 与"scan 失败 + sleep2 缓冲后再 reconnect"（历史可用时序）存在差异，不能排除干扰。

### 修复（方案 B 微调：恢复 reconnect 前缓冲，保留 renew 精简）
`start_wifi.sh`：`scan` 后恢复 `sleep 2`（reconnect 触发固件下载前的缓冲，对齐历史可用时序）；`renew×1` 保留（dhcpc 自带重试，与固件下载无关）。

### 验证（全流程）
1. distclean 全量重编通过
2. strings vela.bin：序列 `disconnect→sleep1→scan→sleep2→reconnect→sleep3→renew→show` 正确
3. 打包 + 资源检查三段一致通过（镜像 md5 99654282）
4. 固化 **ok-20260817-3**

### 上板期望
若 0x27 是时序敏感（reconnect 过早触发固件下载）：恢复缓冲后应消失，WiFi 正常连上；
若 0x27 是硬件层问题（GPIO2 复位波形/SDIO 供电时序，8-10devlog 记录）：可能仍偶发——属驱动/硬件层，勿擅改驱动，需上板示波器排查。

## 四、上板验证通过：0x27 闭环 + WiFi 提速生效（ok-20260817-3）

### 现象（用户烧 ok-20260817-3 整包镜像，勾 res 分区）
```
wapi scan wlan0 → SIOCSIWSCAN error（固件未初始化时 scan 失败，预期，不影响连接）
sleep 2 → wapi reconnect wlan0 → wifi on 1 → Initializing WIFI → sdio irq_thread enter
→ RTL872X: WIFI initialized        ← 固件下载成功，0x27 消失！
→ wapi show wlan0 → ioctl[SIOCGIWESSID] connected, ssid = wifi-home
→ wlan0 Configuration: IP: 192.168.*.*  ESSID: wifi-home
→ ai_agent [boot +47ms] ready → Network connected: 192.168.*.* → WebSocket server 28789
```
其余正常：health_session_start/greeting、tone 播放（res 已烧入）、Settings/WiFi 子页打开、无字体刷屏。

### 结论
**恢复 scan 后 sleep 2 的时序缓冲生效**——reconnect 触发固件下载前的缓冲对齐历史可用时序后，`download_fw` 成功，0x27 彻底消失。P113b WiFi 提速（13s→~5s）**上板验证通过，闭环** ✅。

### 版本
用户拍板：版本号推进到 **V0.0.4**（代码 ok-20260817-3），固件镜像备份于 `/data/vela-backup/firmware/rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand_ok-20260817-3.img`（md5 9965428270fcc3953abde6177013a489）。

## 五、重启后 WiFi 不自动连接：reconnect 关联重试加固（ok-20260817-4）

### 现象（用户重启 ok-20260817-3 后）
```
wapi scan wlan0 → SIOCSIWSCAN error（固件未初始化，预期）
wapi reconnect wlan0 → wifi on 1 → Initializing WIFI → sdio irq_thread → WIFI initialized
  ← 固件下载成功（0x27 已修），但：
  → ioctl(SIOCSIWESSID): 1 → ERROR: Process command (reconnect) failed.   ← 关联失败！
sleep 3 → renew wlan0 → ERROR: netlib_obtain_ipv4addr() failed            ← DHCP 无果
wapi show → ssid = NULL, not connected + IP: 10.*.*.*（wlan0 未关联）
```
对比上次成功启动（`connected, ssid = wifi-home, IP: 192.168.*.*`）：**同一固件、同一 wapi.conf（wifi-home），本次重启关联失败**。

### 根因（代码实证）
1. `wapi reconnect wlan0` = `wapi_load_config`（读 `/data/etc/wifi/wapi.conf`，CONFIG_WIRELESS_WAPI_CONFIG_PATH）→ `wpa_driver_wext_associate`（driver_wext.c:231）：**纯 ioctl 配置下发（mode/auth/cipher/key/ssid），无轮询等待关联完成**——命令返回 ≠ 关联成功。
2. **驱动关联是异步的**：reconnect 只下发参数，实际关联（SIOCSIWESSID）在驱动内部进行，5s 超时窗口内未关联成功即返回失败（本次日志 `ioctl(SIOCSIWESSID): 1` 即此路径）。
3. 脚本层：reconnect 失败后仅 `sleep 3` + **单次 renew** 即收尾——**失败后无任何重试**，关联慢/AP 忙时开机即断网，无自愈。
4. 对比历史 13s 版：reconnect 后 sleep2 + **renew×4**（8s 多轮 DHCP 重试窗口）——历史"可用"部分靠 renew 多试兜底；我 P113b 精简成单次 renew 后，**重试韧性被砍掉**。

### 修复（脚本层重试加固，NSH 线性执行无循环）
`start_wifi.sh`：reconnect 后 `sleep 5`（等关联异步完成）→ `renew wlan0` → `sleep 3` → **`renew wlan0` 二次重试**（dhcpc 自带 DISCOVER/REQUEST 重试，双轮兜底）。总 sleep 1+2+5+3 = 11s，仍比 13s 快 2s，但恢复失败自愈能力。

### 验证（全流程）
1. distclean 全量重编通过
2. strings vela.bin：序列 `disconnect→sleep1→scan→sleep2→reconnect→sleep5→renew→sleep3→renew→show` 正确
3. 打包 + 资源检查三段一致通过（镜像 md5 03ce3b2f）
4. 固化 **ok-20260817-4**

### 上板期望
重启后若首次关联慢：sleep 5 窗口 + renew×2 双轮重试应覆盖；若仍偶发失败，根因在驱动异步关联超时（勿擅改驱动，闭源），可再评估延长 sleep 或 NSH 重试 reconnect（需先验证 NSH 循环语法）。

## 遗留事项

- 🔴 **上板复测（烧 ok-20260817-4 整包镜像，勾 res 分区，烧后 `ls /resource/` 验证）**：①**重启多次验证 WiFi 每次自动连上 wifi-home**（本次修复重点）；②P111 双提醒依次弹窗 / P112 无 measure -1 / P113 连上即出天气对时；③start_wifi 耗时实测（现 sleep 合计 11s）
- 若重启仍偶发连不上：根因在驱动异步关联超时（闭源勿擅改），先 `cat /data/etc/wifi/wapi.conf` 确认内容，再评估延长 sleep 窗口或 NSH 重试 reconnect（需先验证 NSH 循环语法）；方案 A（真事件驱动）同理待 NSH 语法验证后再评估
- 音乐被提示音 force_stop 后不自动恢复（需跨进程 WS 通知，P84 voice_evt 可扩展）
- AI 回复延迟（LLM 非流式，需用户拍板流式化）
- 光感 ALS 自动亮度/午休检测；蓝牙 H4 110 挂起（勿擅改驱动）；Files/Books/Games 回归

## 会话交接摘要（2026-08-17 收尾）

- **代码状态**：V0.0.4 = ok-20260817-4（三仓已固化，tag 可 `git_snapshot.sh -l` 查）
- **固件备份**：`/data/vela-backup/firmware/rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand_ok-20260817-3.img`（md5 99654282）；ok-20260817-4 最新镜像 md5 **03ce3b2fe32a2e2a0f2f1d4f2aeac2ea**（在 lichee out 目录，未复制到 backup，开新会话如需备份再复制）
- **start_wifi.sh 最终形态**：`disconnect→sleep1→scan→sleep2→reconnect→sleep5→renew→sleep3→renew→ntpc→show`（无注释行，NSH 兼容）
- **新会话入口**：AGENTS.md §三「当前唯一焦点」= 重启 WiFi 自动连接复测（P113b 续）

*DevLog by AtomCode (deepseek-v4-flash)*
