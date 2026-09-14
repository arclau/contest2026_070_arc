# DevLog 2026-08-10 part2 — 蓝牙 H4 open 110 深度排查全景（供高级 AI 分析）

> 本文档定位：**给高级 AI 做完整技术交接**——蓝牙驱动背景、代码结构、接口调用链、
> 硬件接线、试过的方法与失败原因、全部日志证据、剩余嫌疑。所有结论均基于源码
> 阅读 + 上板日志验证，标注了确定/推断/待验证三档。

## 一、背景

- 平台：R528 (openvela/NuttX)，目标配置 `r528s3-gemini-s1/nsh`，1920x1200 BOE 屏
- 无线芯片：**RTL8733BS 系 WiFi+BT 二合一**；板上模块为 **BL-M8723DS1（RTL8723DS-CG）**
  - WiFi：SDIO 接口，驱动 `vendor/allwinnertech/boards/r528/drivers/realtek_ieee80211/`（闭源 `librtl8733bs.a`）
  - 蓝牙：**HS-UART 接口（H5 协议）**，驱动 `vendor/allwinnertech/chips/r528/components/bluetooth/`（源码可见）
- 症状：`bluetoothd` 打开 `/dev/ttyHCI0` 失败，errno=**110 (ETIMEDOUT)**，蓝牙完全不可用
- 硬件接线（用户查原理图确认，均为板上真实连接）：

| 模块引脚 | 功能 | R528 引脚 | 说明 |
|---------|------|-----------|------|
| Pin1/34 BT_DIS# | 蓝牙使能（低有效） | **PG18** = `/dev/gpio2` | 驱动 `rtkbt_board_reset/poweroff` 操作它 |
| Pin6 BT_WAKE | HOST 唤醒蓝牙 | PG17 | 驱动未使用 |
| Pin7 BT_WAKE_HOST | 蓝牙唤醒 HOST | PD16 | 驱动未使用 |
| Pin42 UART_TX | 蓝牙数据输出 | **PG7** = UART1_RX | 交叉接线 |
| Pin43 UART_RX | 蓝牙数据输入 | **PG6** = UART1_TX | 交叉接线 |
| Pin44 UART_CTS | CTS 输入 | **PG8** = UART1_RTS | 硬件流控 |
| 模块 RTS | RTS 输出 | **PG9** = UART1_CTS | 硬件流控 |
| Pin25-28 PCM_* | PCM 音频 | PG12-15 | 与 UART 无关 |
| — | 系统 LED | PB10 | 非 UART1_RTS（曾误判） |

## 二、代码结构与调用链（全部源码路径）

### 2.1 服务层 → 内核驱动链路

```
bluetoothd（用户态，frameworks/connectivity/bluetooth/service/）
  └─ stacks/zephyr/hci_h4.c:346  h4_open → open("/dev/ttyHCI0") → errno 110
       └─ nuttx/drivers/serial/uart_bth5.c  uart_bth5_register(path, bt_slip_register(drv))
            = uart_bth4_register(path, bt_slip_register(drv))   // H5(3-wire) 包 H4
            ├─ nuttx/drivers/wireless/bluetooth/bt_slip.c        // H5 协议层
            │   └─ bt_slip_open(): 发 g_sync_req → 等 3s sync_rsp → 超时 -ETIMEDOUT(110)
            └─ vendor/allwinnertech/chips/r528/components/bluetooth/rtk_hci.c
                └─ bthci_open()（只打日志）→ bthci_send() → file_write(filep_uart)
```

### 2.2 关键文件

| 文件 | 职责 | 关键函数/宏 |
|------|------|------------|
| `rtk_hci.c` | 蓝牙驱动主逻辑 | `bthci_init`/`bthci_open`/`bthci_send`/`bthci_borad_init`/`btuart_fw_task`/`hci_load_firmware`/`hci_reset`/`hci_update_baudrate`/`rtk8723FS_initialize` |
| `rtk_hci_board.c` | 板级复位/供电/波特率 | `rtkbt_board_reset()`（PG18 低220ms→高220ms，2026-08-10 改为高1000ms）/`rtkbt_board_poweroff()`/`rtkbt_board_get_baudrate`/`rtkbt_board_fetch_command`（固件下发） |
| `include/rtk_hci_board.h` | 板级常量 | `BT_DEFAUT_BAUDRATE=115200`、`BT_CONFIG_BAUDRATE=1500000`、固件文件名 `8723fs_fw_C-cut.bin`/`8723fs_config_C-cut.bin` |
| `nuttx/drivers/wireless/bluetooth/bt_slip.c` | H5 协议 | `bt_slip_open`（sync 3s 超时返回 -ETIMEDOUT）、`g_sync_req={0x01,0x7e}`、`g_sync_rsp={0x02,0x7d}` |
| `nuttx/drivers/serial/uart_bth4.c` | H4 字符设备 | `uart_bth4_open`（refcnt 后调 drv->open） |
| `nuttx/drivers/serial/uart_bth5.c` | H5 注册入口 | `uart_bth5_register` = `uart_bth4_register(path, bt_slip_register(drv))` |
| `vendor/.../r528_serial.c` | NuttX UART 驱动 | `/dev/uart1` 注册、`set_flowctrl`（UART_MCR_AFCE）、`up_set_termios`（CRTSCTS→hwflowctrl） |
| `.../uart/platform/uart-sun8iw20.h` | UART 引脚宏 | **2026-08-10 修改**：UART1_TX/RX/RTS/CTS = GPIOG(6-9)，mux 2 |
| `nuttx/drivers/mmcsd/sdio.c` | SDIO | 2026-08-10 删除 3 条高频噪声 wlinfo（prep read/write/Transaction ends） |

### 2.3 关键时序（rtk8723FS_initialize）

```
r528_boot.c:951  rtk8723FS_initialize(0)  （CONFIG_COMPONENTS_BLUETOOTH 保护，开机早期）
  ├─ bthci_init(): file_open(filep_uart, "/dev/uart1") + 挂 drv 回调
  ├─ bthci_borad_init(): rtkbt_board_reset()（PG18 复位）+ termios（波特率/流控/校验）
  └─ kthread_create("bt_recv") + kthread_create("bt_fw" = btuart_fw_task)

btuart_fw_task（tid 11，开机即跑）：
  file_open(filep_h5, "/dev/ttyHCI0")   ← 触发 bt_slip_open → H5 sync
    ├─ 发 g_sync_req {0x01,0x7e}（经 bthci_send → file_write UART1）
    ├─ nxsem_tickwait(&priv->sem, 3s) 等 g_sync_rsp {0x02,0x7d}
    └─ 超时 → -ETIMEDOUT(110) → errout_open:
          state=RTKHCI_STATE_CLOSED + rtkbt_board_poweroff()（PG18 拉低）
          + file_close(filep_uart)      ← ★此后 filep_uart 永久关闭

之后服务层 bluetoothd 再 open /dev/ttyHCI0：
  bt_slip_open → bthci_open(OK) → 发 sync → bthci_send 发现 filep_uart.f_inode==NULL
  → 返回 -ENODEV → 等 3s → 110
```

## 三、接口（dm_net / 驱动 API）

- `dm_net_bt_is_on()` / `dm_net_bt_set()`：适配器开关（socket IPC 到 bluetoothd）
- `CONFIG_BLUETOOTH_UART_DEV = "/dev/uart1"`（底层 UART）
- `CONFIG_BT_UART_ON_DEV_NAME = "/dev/ttyHCI0"`（H5 设备）
- `CONFIG_BLUETOOTH_SERVICE_HCI_UART_NAME = "/dev/ttyHCI0"`（服务层）
- 波特率：初始化 115200（`BT_DEFAUT_BAUDRATE`）→ 固件下载后切 1500000（`BT_CONFIG_BAUDRATE`，HCI 0xfc17 命令）

## 四、试过的方法与失败原因（按时间顺序，全部固件已固化）

| 版本 | 修改 | 验证方式 | 结果 |
|------|------|---------|------|
| ok-20260810-11~17 | 恢复状态栏/回退等（UI） | — | 与蓝牙无关 |
| ok-20260810-18 | **UART1 引脚 PB8-11 → PG6-9 + mux2**（uart-sun8iw20.h） | 反汇编 hal_uart.o 确认 r0=198-201,r1=2；上板日志 `tx t:15 l:2` sync_req 发出 | **部分成功**：sync_req 能写入了（此前发错引脚），但芯片 3s 无 sync_rsp，仍 110 |
| ok-20260810-19 | **启用 CRTSCTS**（rtk_hci.c 删无条件 `#define CONFIG_X4B_FACTEST` → 走正常分支 `|= CRTSCTS`）+ **8E1→8N1**（去 PARENB） | 上板日志 `For normal BT, enable hw flow control`；sunxi 驱动确认 `hwflowctrl=true` + `UART_MCR_AFCE` | 无效，仍 110 |
| ok-20260810-20 | **复位时序 220ms→1000ms**（rtk_hci_board.c） | 上板日志 bthci_open 推迟 1s 后 sync 仍无响应 | **无效**：复位时序嫌疑排除 |

**失败原因归纳（确定）**：
1. 首次失败点是 **bt_fw 线程开机时 H5 sync 握手超时**（芯片不回 g_sync_rsp）
2. 该失败触发 errout_open 关闭 filep_uart + poweroff(PG18) + state=CLOSED → **服务层二次 open 必然 110**（`bthci_send: uart file closed, state:0` 是果不是因）
3. 软件侧四层（引脚/流控/校验/复位时序）全部修改且验证生效，**均不能使芯片响应 sync**

## 五、日志证据（上板实测，关键行）

### 5.1 ok-20260810-18/19/20 开机早期（bt_fw 线程 tid 11）

```
00:00:37 rtk8723FS_initialize
00:00:37 bthci_init
00:00:38 For normal BT, enable hw flow control      ← -19/-20 才有（流控启用）
00:00:38 [11] bthci_open, id:0
00:00:38 [11] tx t:15 l:2 s:0 a:0 checksum:0x0000   ← H5 sync_req 发出
08:00:41 [11] err: bluetooth driver open timeout     ← 3s 无 sync_rsp（时间戳为 NTP 校准跳变）
08:00:41 [11] bthci_close
08:00:41 [11] fail, filep_h5 open ret:-110
```
（无 `h5: initialized` / `h5: active` / `bt download fw takes` —— 芯片全程无响应）

### 5.2 服务层（bluetoothd，tid 95）

```
08:01:02 [95] bthci_open, id:0
08:01:02 [95] tx t:15 l:2 s:0 a:0 checksum:0x0000
08:01:02 [95] bthci_send: uart file closed, state:0   ← filep_uart 已被 bt_fw 失败关闭
08:01:05 [95] err: bluetooth driver open timeout
08:01:05 [95] H4: Failed to open /dev/ttyHCI0: 110
08:01:05 [1003][BT]: [bt_sal_le_enable] return:-1
08:01:05 [281][adapter-stm]: BT_DFX: btOpenError: btLeEnableFail
```

### 5.3 同芯片 WiFi（对照组，证明芯片活着）

```
08:00:42 RTL872X: WIFI initialized
08:00:53 wlan0 IP: 10.*.*.*
[wifi] wifi startup done
```

## 六、剩余嫌疑（需硬件/上板排查，按优先级）

1. **示波器验证 UART 波形**（最高优先级，一次区分三态）：
   - 测 **PG6（UART1_TX）**：开机早期应见 sync_req 帧（115200、8N1、`01 7E` 起始）
   - 测 **芯片侧 UART_RX 引脚**：sync_req 是否真到芯片（PG6→芯片 RX 间线路/焊点）
   - 测 **芯片侧 UART_TX / PG7**：芯片有无回帧
2. **芯片 BT 域使能**：BT_DIS#(PG18) 高电平是否确实使能蓝牙域（WiFi 正常 ≠ 蓝牙域一定使能；RTL8723DS 双域独立）
3. **芯片 ROM 默认波特率**：是否为 115200（若芯片 ROM 默认 38400，sync 收不到——待数据手册确认）
4. **供电**：蓝牙域 3.3V/1.8V 是否独立、上电时序
5. **固件文件**：`8723fs_fw_C-cut.bin`/`8723fs_config_C-cut.bin` 是否存在且 cut 匹配（`/etc/bt/`，板级 src/etc/bt/ 有副本）

## 七、附：同日其他改动（与蓝牙无关，可忽略）

- 子页状态栏修复：`create_status_bar_ex` 共享函数（ui_home.c），Home 与子页同一份代码，132px 全宽（时钟+天气+wifi/bt/battery），返回按钮叠状态栏上；deskmate_ui.h/c、ui_home.c 联动，close_subpage 置空
- Settings ABOUT 版本号接入：`deskmate_ui.h` `DM_VERSION="V0.0.2"`/`DM_BUILD_TAG`，ui_settings.c 显示
- AGENTS.md 新增 §一.5 源码结构索引 + 蓝牙 UART1 引脚修复记录
- sdio.c 噪声日志删除

*DevLog by AtomCode (deepseek-v4-flash)*
