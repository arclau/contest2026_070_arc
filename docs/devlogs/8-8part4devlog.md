# DevLog 2026-08-08 (Part 4) — WiFi SSID 列表空白修复 + Books 点击修复 + 蓝牙 H4 110 定位（-24 续）

> 工程：openvela (R528) — nsh 配置｜工作目录：`/data/dm`｜日期：2026-08-08
> 前置：`8-8part3devlog.md`（-16~-19 音乐控件）、`devlog.md` P18~P23（WiFi/蓝牙接线 + Books 子 APP）
> 状态：固化 **ok-20260808-26**（md5 1b781f0c），本轮为「用户上板反馈三连 → 修复 + 驱动层定位」收尾

---

## 一、背景

用户刷 -25（Books 阅读器版）上板，实测反馈三件事：
1. **Settings 里点 WiFi 栏**：子页能进（`Open subpage: WiFi` 出现，-24 嵌套导航修复生效 ✅）但 **SSID 列表空白**；
2. **蓝牙**：`H4: Failed to open /dev/ttyHCI0: 110`（ETIMEDOUT），蓝牙 enable 失败（`BtLeEnableFail`）；
3. **Books 子 APP**：书架只显示一个"三体"（静态 hero 卡），**点不进去**。

---

## 二、修复与定位（按「现象→根因→方案→验证」）

### 2.1 WiFi SSID 列表空白（✅ 已修复，固化 -26）

| 项 | 内容 |
|----|------|
| **现象** | 进 WiFi 子页后状态行永远"扫描中…"，SSID 清单空白 |
| **根因** | `dm_net_wifi_scan_start()` 不检查 `wifi_scan_networks()` 返回值——驱动扫描忙时（`scan_result_handler_ptr.scan_running` 残留 / 开机自动连接流程占用）`wifi_scan_networks` 内部等 2s 后返回 `RTW_TIMEOUT`，**扫描回调永不触发、`g_scan_done` 恒 0** → UI 轮询永远等不到结果 |
| **方案** | ①`dm_net.c`：`g_scan_fail` 标志 + `dm_net_wifi_scan_start()` 检查返回值（非 `RTW_SUCCESS` 置 fail 返回 -1）+ 新增 `dm_net_wifi_scan_failed()` ②`deskmate_ui.c`：`wifi_poll_cb` 失败分支提示"扫描失败，请重试"；`wifi_scan_btn_cb` 与 `create_wifi_subpage` 进入自动扫描都检查返回值（"WiFi 忙，请稍后再试"） |
| **验证** | 编译/打包通过（-26）；待上板：进子页应自动出 SSID 清单或提示失败重试，不再死卡"扫描中…" |

### 2.2 Books 点不进去（✅ 已修复，固化 -26）

| 项 | 内容 |
|----|------|
| **现象** | Books 子页只看到一个"三体"卡片（静态 hero 卡 Currently Reading），点击无反应 |
| **根因** | 书架网格是扫描 `/sdcard/book` 结果；用户 TF 卡可能无书（或目录为空）→ 网格空，只剩**静态 hero 卡**（硬编码 "The Three-Body Problem"）——hero 卡**从未挂点击事件**，点了当然没反应；另外书架空时无任何提示 |
| **方案** | ①hero 卡加 `LV_OBJ_FLAG_CLICKABLE` + `book_hero_click_cb`（有书则 `book_open_reader(0)` 打开第一本）②书架空时显示提示"把 .txt 书放到 /sdcard/book 后重新进入" ③（前向声明补 book_hero_click_cb） |
| **验证** | 编译/打包通过（-26）；待上板：有书时点 hero/书卡进阅读器；无书时显示提示 |

### 2.3 蓝牙 H4 open 110（🔍 驱动层定位完成，非 UI 可修，另案）

| 项 | 内容 |
|----|------|
| **现象** | 蓝牙开关 enable → `[346][h4]: H4: Failed to open /dev/ttyHCI0: 110` → `HCI driver open failed (-1)` → `BtLeEnableFail` |
| **链路定位** | bluetoothd（ZBlue 栈 hci_h4.c）open `/dev/ttyHCI0` → `uart_bth4_open` → `dev->drv->open()` = `bt_slip_open`（因 CONFIG_UART_BTH5 走 `uart_bth5_register` = `uart_bth4_register(path, bt_slip_register(drv))`）→ `bthci_open`（rtk_hci，仅 wlinfo+OK）→ **发 sync_req 等 3s sync_rsp 超时 → -ETIMEDOUT(110)** |
| **已排除** | ①设备名匹配：`CONFIG_BT_UART_ON_DEV_NAME="/dev/ttyHCI0"` 与 rtk_hci BTH5 分支注册名一致 ②固件文件存在：`src/etc/bt/8723fs_fw_C-cut.bin`（打包进 res.fex）③UART1 已启用：`CONFIG_R528_UART1=y` + RX/TX DMA ④GPIO 复位：`rtkbt_board_reset()` 操作 `/dev/gpio2`（BT_DIS_PIN_GPIO） |
| **剩余嫌疑** | ①`/dev/gpio2` 复位引脚电平/时序（芯片未真正复位）②UART1 波特率/流控（`BT_DEFAUT_BAUDRATE` vs 芯片默认；bthci_borad_init 里 CRTSCTS 被 `#define CONFIG_X4B_FACTEST` 强制关闭）③固件下载线程 `btuart_fw_task` 是否成功跑完（芯片 ROM 需先响应 sync 才能下载补丁固件） |
| **结论** | 属**驱动层板级 HCI 初始化**（GPIO/波特率/固件下载时序），UI 层无法定论；**擅改驱动风险大**（参考 A3 reset 致 BOOT0 重启教训）→ 另案，需上板抓 `btuart_fw_task` 日志或实测 GPIOD 电平 |

---

## 三、改动文件表（固化 ok-20260808-26，md5 1b781f0c）

| 文件 | 改动 |
|------|------|
| `vendor/allwinnertech/apps/luncher_dm/dm_net.c` | `g_scan_fail` 标志；`dm_net_wifi_scan_start()` 检查返回值；新增 `dm_net_wifi_scan_failed()` |
| `vendor/allwinnertech/apps/luncher_dm/dm_net.h` | 声明 `dm_net_wifi_scan_failed()`；scan_start 注释补充返回语义 |
| `vendor/allwinnertech/apps/luncher_dm/deskmate_ui.c` | `wifi_poll_cb` 失败提示；`wifi_scan_btn_cb`/`create_wifi_subpage` 检查扫描返回值；hero 卡挂点击（`book_hero_click_cb`）+ 前向声明；书架空提示 |

## 四、验证命令与产物

```bash
cd /data/dm && ./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh -j$(nproc)
cd /data/dm/vendor/allwinnertech/lichee && source envsetup.sh && lunch_nuttx 2 && pack
md5sum .../image/nsh.fex /data/dm/nuttx/vela.bin   # 1b781f0c858b0c275dd4d5b2266e8f29 一致
bash /data/vela/git_snapshot.sh                    # 固化 ok-20260808-26
```

## 五、遗留事项

1. **🔴 蓝牙 H4 open 110（另案，驱动层）**：上板抓 `btuart_fw_task` 固件下载日志；实测 `/dev/gpio2`（BT_DIS_PIN_GPIO）复位电平；核对 UART1 波特率与 CRTSCTS（X4B_FACTEST 强制关闭流控是否与板子匹配）
2. WiFi SSID 列表修复（-26）待上板验证：驱动不忙时应正常出清单；忙时提示重试
3. Books 修复（-26）待上板验证：点 hero/书卡进阅读器、空书架提示
4. 既有遗留：退出重进卡死、Settings 其余开关无回调、shtc3 HPWORK 阻塞、bt_recv 空转、Books 阅读进度记忆/护眼持久化

---

*DevLog by AtomCode (deepseek-v4-flash)*
