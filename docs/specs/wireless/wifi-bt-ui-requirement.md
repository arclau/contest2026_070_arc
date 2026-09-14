# WiFi / 蓝牙 UI 接入开发需求文档（wifidev.md）

> **文档定位**：面向高级 AI（ChatGPT）的自包含开发指引——需求、背景、驱动、API、现有代码、已知问题全部在此，读者无需本项目历史。供 AI 分析后给出 WiFi/蓝牙 UI 接入的完整开发方案。
> 工程：openvela (R528) — nsh 配置｜工作目录：`/data/dm`｜**最近更新：2026-08-09（上板日志铁证，见 §0 新根因）**
> 相关文件：`/data/dm/AGENTS.md`（项目交接/编译固化规范）、`/data/dm/devlog.md`（节点表 P24 = 2026-08-08 修复记录）、`/data/dm/project_docs/devlogs/8-8part4devlog.md`（WiFi SSID 空白 + 蓝牙 110 定位）
> ⚠️ 注意：本文件为**需求/交接文档**，不是实现。AI 分析后应给出方案；如需改代码，遵循 AGENTS.md 的编译/打包/固化流程。

---

## 〇、🔴 2026-08-09 上板日志铁证（用户最新实测，优先级最高）

> 用户刷 **ok-20260808-26**（md5 1b781f0c，WiFi SSID 空白修复版）上板，串口日志如下（关键行已标注）：

```
[08:01:12] app_icon_click_cb: Launch app: settings
[08:01:13] show_subpage: Open subpage: Settings
[08:01:15] show_subpage: Open subpage: WiFi      ★ 点击 WiFi 行 → 子页成功打开（-24 嵌套导航修复生效）
[08:01:24] bt_client_78 loop running now !!!
[08:01:24] adapter-stm: Process, State=Off, Event=SYS_TURN_ON
[08:01:27] H4: Failed to open /dev/ttyHCI0: 110   ★ 蓝牙 H4 open 110（已知另案）
[08:01:31] RTL871X: download_fw: download firmware FAIL! status=0x27   ★★ WiFi 固件下载失败（真凶）
[08:01:41] rtl8723f_hal_init Download Firmware from file failed
[08:01:41] rtw_hal_init: hal__init fail
[08:01:41] -871x_drv - drv_open fail, bup=0
[08:01:53] Show standby (reused)
```

### 结论 1：UI 点击入口存在、子页能打开（此前 H1 假设证伪）

- 日志 `Open subpage: WiFi`（08:01:15）证明 `settings_wifi_row_open_cb` → `show_subpage("WiFi")` 已被触发，-22 引入、-24 修复的嵌套导航回归**在当前固件不再复现**。
- 用户口头"点击无 LOG"与实际日志矛盾（日志里明确有 Open subpage: WiFi）；**不要再往"点击入口缺失"方向排查**。

### 结论 2：真正根因 = WiFi 固件下载失败（download_fw FAIL status=0x27）

链路：进 WiFi 子页 → `create_wifi_subpage` 自动 `dm_net_wifi_scan_start()` → `wifi_scan_networks()` → 驱动 init → **固件文件打开/下载失败** → 驱动起不来 → 扫描永远失败 → **SSID 列表空白** → 用户感知"WiFi 打不开/没用"。

**固件路径排查结论（2026-08-09 已用 genromfs 实验验证，排除"打包拍平"假设）**：

| 项 | 值 |
|----|-----|
| 驱动期望路径 | `Make.defs`：`CCVWIFIFW_PATH="/resource/etc/wifi/FW_NIC_ACUT.bin"`（cut_version==0）/ `WIFIFW_PATH="/resource/etc/wifi/FW_NIC_BCUT.bin"` |
| 固件源文件 | `lichee/board/common/data/res/etc/wifi/FW_NIC_ACUT.bin`（120424B）、`FW_NIC_BCUT.bin`（120040B）、`wapi.conf`（111B）——**源存在** |
| res.fex（romfs）裸名计数 | `FW_NIC_ACUT.bin`=1、`FW_NIC_BCUT.bin`=1、`wapi.conf`=1、`etc`=6、`wifi`=3、`logo1.bin`=1——与"目录层级正常"的实验镜像模式**一致** |
| ✅ 关键实验 | `genromfs -v` 打包同源目录：输出 `etc→wifi→FW_NIC_ACUT.bin`（深度 3）**层级保留**。romfs 用 inode next 链表达目录，**文件名即裸名、完整路径字符串（如 `etc/wifi/...`）本就不会出现在 romfs 二进制里** → 之前"`etc/wifi/` 计数=0 → 拍平到根目录"的推断**不成立** |
| 挂载 | rcS.nsh：`mount -t romfs /dev/res /resource` |

**🔥 修正后结论**：固件路径**很可能正确**（`/resource/etc/wifi/FW_NIC_ACUT.bin` 存在），`download_fw FAIL status=0x27` **不是打包路径问题**。0x27=39 需按**驱动层固件下载失败**排查：
- **上板验证（必须，本地无法替代）**：`ls -l /resource/etc/wifi/`；`cat /resource/etc/wifi/FW_NIC_ACUT.bin | wc -c`；若文件在且大小=120424 → 路径排除，进入驱动层。
- **驱动层嫌疑排序**：①`rtw_retrieve_from_file`（osdep_service.c:1408）open/read 失败原因（`Can't open firmware`/`Read FW error` 打印是否出现）；②固件下载到芯片的 **SDIO 时序/芯片复位**（与蓝牙 H4 open 110 **同源嫌疑**：`/dev/gpio2` 复位电平、SDIO/UART 供电、波特率）；③固件版本与芯片 cut 不匹配（ACUT vs BCUT 选择）。
- **勿擅改驱动**（A3 reset 致 BOOT0 重启教训），需上板抓 `rtw_retrieve_from_file` 周边完整日志。

### 结论 3：蓝牙 H4 open 110（另案，驱动层）

`bt_slip_open` 发 sync_req 等 3s sync_rsp 超时 → `-ETIMEDOUT(110)` → `BtLeEnableFail`。嫌疑：`/dev/gpio2` 复位电平、UART1 波特率/流控（X4B_FACTEST 强制关 CRTSCTS）、`btuart_fw_task` 时序。**勿擅改驱动**（A3 教训），需上板抓日志。

---

---

## 一、需求（用户原话 + 整理）

### 1.1 WiFi（用户反馈："WIFI只有开和关，点击WIFI本项目进去不能看到SSID列表"）

用户期望的完整 WiFi 管理（手机式）：

```
Settings → 点击 WiFi 项目（不是开关）→ 进入 WiFi 管理子页
  → 子页里有 SSID 清单（扫描结果：SSID / 信号强度 / 加密标记 / 已连接打勾）
  → 点击某个 SSID → 弹出软键盘输入密码
  → 接入驱动验证密码（wifi_connect）
  → 成功：清单打勾、状态栏图标变蓝
  → 失败：提示"密码错误 / 无法连接"
  → 常规手机 WiFi 接入逻辑（记住密码、开机自动重连）
```

**当前已实现（固化 ok-20260808-21，md5 fee23bbf，但用户反馈不可用）**：
- Settings 里 WiFi 行**保留开关**（点开关 = `wifi_on/off`）+ **整行可点击**进子页（`settings_wifi_row_open_cb`）
- `create_wifi_subpage()`：顶部状态行 + "重新扫描"按钮 + SSID 清单容器 + 500ms 轮询 timer
- 点击 AP：OPEN 直连 / 加密弹 `lv_keyboard`+`lv_textarea` 密码层
- `dm_net.c`：异步扫描（RTW 线程回调拷静态数组）+ pthread 后台连接 + 成功自动写 `/data/etc/wifi/wapi.conf`

**🔴 用户实测问题：点击 WiFi 项目进不去/看不到 SSID 列表**（可能原因见第六章假设 H1~H5）。

### 1.2 蓝牙（用户要求：补上相关代码一并分析）

用户期望的蓝牙管理（手机式，参照 WiFi）：

```
Settings → 点击 Bluetooth 项目 → 进入蓝牙子页
  → 子页里有附近设备清单（discovery 扫描结果）
  → 点击设备 → 配对（create_bond / pair reply）
  → 配对成功 → 已配对设备列表管理（connect/disconnect/remove）
  → 状态栏蓝牙图标随适配器状态变色
```

**当前已实现**：**只有开关**（`settings_bt_cb` → `dm_net_bt_set` → `bt_adapter_enable/disable`），**没有设备清单、没有配对、没有已配对管理**。状态栏蓝牙图标随缓存状态变色（每 10s 低频查询 `bt_adapter_get_state`）。

---

## 二、背景

### 2.1 硬件

| 项 | 值 |
|----|-----|
| 主控 | Allwinner **R528**（双核 ARM Cortex-A7） |
| WiFi | **RTL8733BS**（SDIO 接口，Realtek 驱动 `realtek_ieee80211`，板级 bringup 已 `realtek_wl_initialize(0)`） |
| 蓝牙 | 同颗 RTL8733BS（**H4 UART**，ZBlue 协议栈，`bluetoothd` 服务进程由 rcS.nsh 启动） |
| 屏 | BOE 1200x1920 MIPI DSI（横屏 1920x1200），LVGL 9.1 |
| 系统 | openvela（NuttX 定制），UI = `apps/luncher_dm/deskmate_ui.c`（唯一源） |

### 2.2 软件链路

```
WiFi: UI(deskmate_ui.c) → dm_net.c(封装) → wifi_conf.h(wifi_on/scan/connect) → RTL8733BS 驱动
蓝牙: UI(deskmate_ui.c) → dm_net.c(封装) → bt_adapter.h(socket IPC) → bluetoothd(rcS 启动) → ZBlue 栈 → H4 UART → RTL8733BS
```

- `bluetoothd` 由 `boards/r528/r528s3-gemini-s1/src/etc/init.d/rcS.nsh` 启动（`bluetoothd &`）
- WiFi 开机自动连接：rcS 里若存在 `/data/etc/wifi/wapi.conf` 则 `sh /etc/wifi/start_wifi.sh &`（读取 JSON：ssid/psk/bssid，用 `wapi` 命令配置）
- UI 是 LVGL 单线程；**驱动回调（RTW 扫描线程 / bluetoothd socket）不能直接碰 LVGL 对象**——必须拷到静态数据 + UI 轮询/异步刷新

### 2.3 编译接线（已生效）

`apps/luncher_dm/Makefile`：
```make
# Use common HAL includes
$(eval $(AW_ADD_HAL_INCLUDES))

# WiFi (RTL8733BS) — status query + on/off from UI (wifi_conf.h)
$(eval $(AW_ADD_WIFI_INCLUDES))

# Bluetooth framework (ZBlue) — bt_adapter API, socket IPC to bluetoothd
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/frameworks/connectivity/bluetooth/framework/include
```
- `AW_ADD_WIFI_INCLUDES` 展开为 `$(AW_BOARD_DIR)/drivers/realtek_ieee80211` 的 api/wifi、include、platform 等 12 个 include 路径（定义在 `vendor/allwinnertech/Common.mk:40`）
- 蓝牙框架库 `libbluetooth.a` 在 `apps/staging/`，由 `Application.mk` 自动链接（`LDLIBS += $(wildcard $(APPDIR)/staging/*.a)`）
- `CSRCS = ... dm_net.c`（新增独立编译单元）

### 2.4 🔴 关键编译坑（已踩过，勿重犯）

`wifi_conf.h`（→ `wifi_structures.h` → `platform/include/dlist.h`）定义了 `struct list_head`/`__list_add`，与 HAL 头 `aw_list.h`（经 `sunxi_hal_pwm.h` 引入）**重定义冲突**。因此**所有 WiFi/蓝牙驱动头必须放在独立编译单元 `dm_net.c`**，`deskmate_ui.c` 只 include `dm_net.h` 调封装 API，禁止直接 include wifi/bt 头。

---

## 三、驱动层 API 清单

### 3.1 WiFi（`boards/r528/drivers/realtek_ieee80211/api/wifi/wifi_conf.h`）

| API | 签名 | 语义 / 注意 |
|-----|------|-------------|
| `wifi_on` | `int wifi_on(rtw_mode_t mode)` | 开射频。`RTW_MODE_STA=0` / `RTW_MODE_AP=1`。**耗时**（射频上电+驱动初始化，可能几百 ms） |
| `wifi_off` | `int wifi_off(void)` | 关射频 |
| `wifi_is_up` | `int wifi_is_up(rtw_interface_t interface)` | 接口是否 up；`RTW_STA_INTERFACE=0` |
| `wifi_is_connected_to_ap` | `int wifi_is_connected_to_ap(void)` | 是否已关联 AP |
| `wifi_scan_networks` | `int wifi_scan_networks(rtw_scan_result_handler_t h, void *ud)` | **异步扫描**。回调在 RTW 线程，**禁止阻塞/碰 UI**；结果回调 `rtw_scan_handler_result_t*`（`scan_complete=1` 表示结束）；`user_data` 在扫描期间必须保持有效 |
| `wifi_connect` | `int wifi_connect(char *ssid, rtw_security_t sec, char *psk, int ssid_len, int psk_len, int key_id, void *semaphore)` | **阻塞验证**（内部等 join semaphore 最长 **60s**，传 NULL 自建）。WPA2 PSK 密码要求 8~64 字节，错误返回 `RTW_INVALID_KEY` |
| `wifi_disconnect` | `int wifi_disconnect(void)` | 断开 |
| `wifi_get_setting` | `int wifi_get_setting(const char *ifname, rtw_wifi_setting_t *p)` | 查当前连接配置（`WLAN0_NAME="wlan0"`）；`rtw_wifi_setting_t.ssid[33]` 为当前 SSID |
| `wifi_get_rssi` | `int wifi_get_rssi(int *pRSSI)` | 当前连接信号 |

**关键结构**（`include/wifi_structures.h`）：
```c
typedef struct rtw_ssid {
    unsigned char len;     /* SSID 长度 */
    unsigned char val[33]; /* SSID 名称（无 \0！按 len 截取） */
} rtw_ssid_t;

typedef struct rtw_scan_result {
    rtw_ssid_t       SSID;             /* AP 名称 */
    rtw_mac_t        BSSID;            /* MAC */
    signed short     signal_strength;  /* dBm：<-90 很差，>-30 极好 */
    rtw_bss_type_t   bss_type;
    rtw_security_t   security;         /* 0=OPEN；WPA2_AES_PSK=(WPA2|AES) */
    unsigned int     channel;
} rtw_scan_result_t;

typedef struct rtw_scan_handler_result {
    rtw_scan_result_t ap_details;
    rtw_bool_t        scan_complete;   /* 1 = 扫描结束（最后一个回调） */
    void*             user_data;
} rtw_scan_handler_result_t;

typedef struct rtw_wifi_setting {
    rtw_mode_t        mode;
    unsigned char     ssid[33];
    unsigned char     channel;
    rtw_security_t    security_type;
    unsigned char     password[65];
    unsigned char     key_idx;
} rtw_wifi_setting_t;
```

**安全枚举**（`include/wifi_constants.h`）：`RTW_SECURITY_OPEN=0`、`RTW_SECURITY_WEP_PSK`、`RTW_SECURITY_WPA_TKIP_PSK`、`RTW_SECURITY_WPA2_AES_PSK` 等（位域组合）。

### 3.2 蓝牙（`/data/dm/frameworks/connectivity/bluetooth/framework/include/`）

**实例创建**（`bluetooth.h`）：`bluetooth_create_instance(void) -> bt_instance_t*`（socket IPC 连 bluetoothd，失败返回 NULL）。

**适配器控制**（`bt_adapter.h`）：
| API | 语义 |
|-----|------|
| `bt_adapter_enable(ins)` / `bt_adapter_disable(ins)` | 开/关适配器 |
| `bt_adapter_get_state(ins) -> bt_adapter_state_t` | 状态：`OFF=0 / BLE_TURNING_ON / BLE_ON / TURNING_ON / ON / TURNING_OFF / BLE_TURNING_OFF` |
| `bt_adapter_register_callback(ins, const adapter_callbacks_t*)` | 注册回调（`on_adapter_state_changed` / `on_discovery_state_changed` / `on_discovery_result` / `on_pair_request` / `on_connection_state_changed` 等） |
| `bt_adapter_start_discovery(ins, uint32_t timeout)` | 开始设备发现（timeout 秒） |
| `bt_adapter_stop_discovery(ins)` | 停止发现 |
| `bt_adapter_get_name(ins, char*, size)` 等 | 查询属性 |

**设备管理**（`bt_device.h`）：
| API | 语义 |
|-----|------|
| `bt_device_create_bond(ins, bt_address_t*, bt_transport_t)` | 发起配对（`BT_TRANSPORT_BR_EDR` / `BT_TRANSPORT_BLE` / `BT_TRANSPORT_LE`） |
| `bt_device_remove_bond(ins, bt_address_t*, uint8_t transport)` | 解除配对 |
| `bt_device_pair_request_reply(ins, bt_address_t*, bool accept)` | 响应配对请求（回调 `on_pair_request` 时调用） |
| `bt_device_connect(ins, bt_address_t*)` | 连接 |
| `bt_device_disconnect(ins, bt_address_t*)` | 断开 |
| `bt_device_get_connection_state(ins, bt_address_t*, bt_transport_t)` | 查询连接状态 |

**回调结构**（`adapter_callbacks_t`，`bt_adapter.h:529-546`）：`on_adapter_state_changed`、`on_discovery_state_changed`、`on_discovery_result`（`bt_discovery_result_t* remote`，含 name/addr）、`on_pair_request`、`on_pair_display`、`on_connection_state_changed`、`on_bond_state_changed` 等。

**参考实现**：`apps/bt_instance/bt_start.c`（`g_bt_inst = bluetooth_create_instance();` → `bt_adapter_enable(g_bt_inst)` / `bt_adapter_get_state` / `bt_device_*`；含 profile_init 与回调示例）。

---

## 四、现有代码全文（2026-08-08 固化 ok-20260808-21 版本）

### 4.1 `apps/luncher_dm/dm_net.h`（封装接口，UI 唯一入口）

```c
/*
 * dm_net.h — 网络驱动接线封装（2026-08-08）
 *
 * 状态栏图标 + Settings WiFi/蓝牙开关直连驱动：
 *   - WiFi：RTL8733BS SDIO（wifi_conf.h，板级 bringup 已初始化）
 *   - 蓝牙：bluetoothd socket IPC（rcS.nsh 启动）+ bt_adapter API
 *
 * ⚠️ 必须独立编译单元：wifi_conf.h 的 dlist.h 与 HAL aw_list.h 都定义
 * struct list_head，与 sunxi_hal_*.h 同编译单元会重定义报错。
 * UI 代码只允许经这些封装 API 访问驱动，禁止直接 include wifi/bt 头。
 */
#ifndef __DM_NET_H
#define __DM_NET_H

/* @return 1 = STA 接口 up（radio on / 已连接），0 = down */
int  dm_net_wifi_is_up(void);

/* @param on 1 = wifi_on(RTW_MODE_STA)，0 = wifi_off() */
void dm_net_wifi_set(int on);

/* @return 1 = 蓝牙适配器已开启（ON/BLE_ON），0 = 关闭 */
int  dm_net_bt_is_on(void);

/* @param on 1 = bt_adapter_enable，0 = bt_adapter_disable（实例懒创建） */
void dm_net_bt_set(int on);

/* ================================================================
 * WiFi 管理（2026-08-08 手机式接入：SSID 清单 + 密码验证 + 记住配置）
 * 扫描/连接均为驱动异步接口，UI 侧轮询结果，禁止在回调里碰 LVGL。
 * ================================================================ */

#define DM_NET_AP_MAX 24

typedef struct {
    char  ssid[33];      /* AP 名称（含 \0） */
    int   security;      /* rtw_security_t（0=OPEN） */
    short rssi;          /* 信号强度 dBm */
    int   connected;     /* 是否为当前连接网络 */
} dm_net_ap_t;

/* 发起异步扫描；返回 0=已启动（结果就绪后 scan_done()==1） */
int  dm_net_wifi_scan_start(void);
int  dm_net_wifi_scan_done(void);
int  dm_net_wifi_scan_count(void);
int  dm_net_wifi_scan_get(int idx, dm_net_ap_t *ap);

/* 异步连接：起线程调 wifi_connect（阻塞最长 60s，不能占 UI 线程）。
 * psk 可为 NULL（OPEN 网络）。连接状态见 dm_net_wifi_connect_state。
 * 连接成功时自动写入 wapi.conf（开机自动重连）。 */
int  dm_net_wifi_connect_start(const char *ssid, const char *psk);
int  dm_net_wifi_connect_state(void);   /* 0=idle 1=connecting 2=ok 3=fail */

/* 当前已连接网络的 SSID（经 wifi_get_setting 查询；未连接时置空） */
int  dm_net_wifi_cur_ssid(char *buf, int buflen);

/* 连接成功后把 SSID/PSK 写入 /data/etc/wifi/wapi.conf（start_wifi.sh
 * 开机读取自动重连，JSON: {"ssid":"..","psk":"..","bssid":""}） */
int  dm_net_wifi_save_conf(const char *ssid, const char *psk);

#endif /* __DM_NET_H */
```

### 4.2 `apps/luncher_dm/dm_net.c`（驱动封装实现，独立编译单元）

```c
/*
 * dm_net.c — 网络驱动接线实现（2026-08-08）
 *
 * 独立编译单元，仅 include WiFi/蓝牙驱动头（不含任何 HAL 头），
 * 规避 dlist.h(struct list_head) 与 aw_list.h 的重定义冲突。
 */
#include "dm_net.h"
#include "wifi_conf.h"
#include "bluetooth.h"
#include "bt_adapter.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ================================================================
 * WiFi 管理（2026-08-08）：异步扫描 + 后台线程连接 + wapi.conf 记忆
 * 扫描回调运行在 RTW 线程——只拷贝数据到静态数组，不碰 UI。
 * ================================================================ */

static dm_net_ap_t      g_aps[DM_NET_AP_MAX];
static int              g_ap_count;
static volatile int     g_scan_done;
static volatile int     g_conn_state;   /* 0=idle 1=connecting 2=ok 3=fail */
static pthread_t        g_conn_thread;
static char             g_psk_buf[65];  /* 连接线程密码副本（单连接串行） */

/* 蓝牙实例懒创建 + 适配器状态缓存。socket IPC 有往返开销，
 * 真实状态每 10 次查询（≈10s）刷新一次缓存，其余读缓存。 */
static bt_instance_t        *g_bt_ins;
static bt_adapter_state_t    g_bt_state = BT_ADAPTER_STATE_OFF;
static int                   g_bt_tick;

/* 扫描回调（RTW 线程上下文，禁止阻塞/操作 LVGL） */
static rtw_result_t dm_net_scan_handler(rtw_scan_handler_result_t *r)
{
    if (r == NULL)
        return RTW_SUCCESS;

    if (r->scan_complete) {
        g_scan_done = 1;
        return RTW_SUCCESS;
    }

    if (g_ap_count >= DM_NET_AP_MAX)
        return RTW_SUCCESS;

    dm_net_ap_t *ap = &g_aps[g_ap_count];
    memset(ap, 0, sizeof(*ap));
    memcpy(ap->ssid, r->ap_details.SSID.val, r->ap_details.SSID.len);
    ap->ssid[r->ap_details.SSID.len] = '\0';
    ap->security = (int)r->ap_details.security;
    ap->rssi     = r->ap_details.signal_strength;
    ap->connected = 0;
    g_ap_count++;

    return RTW_SUCCESS;
}

int dm_net_wifi_scan_start(void)
{
    g_ap_count  = 0;
    g_scan_done = 0;
    return wifi_scan_networks(dm_net_scan_handler, NULL);
}

int dm_net_wifi_scan_done(void)  { return g_scan_done; }
int dm_net_wifi_scan_count(void) { return g_ap_count; }

int dm_net_wifi_scan_get(int idx, dm_net_ap_t *ap)
{
    if (idx < 0 || idx >= g_ap_count || ap == NULL)
        return -1;
    *ap = g_aps[idx];
    return 0;
}

/* 后台连接线程（wifi_connect 阻塞最长 60s，不能占 UI 线程） */
static void *dm_net_conn_worker(void *arg)
{
    const char *ssid = (const char *)arg;
    int ret;

    /* 密码可空（OPEN 网络）；wifi_connect 要求 WPA PSK 8~64 字节 */
    if (g_psk_buf[0])
        ret = wifi_connect((char *)ssid, RTW_SECURITY_WPA2_AES_PSK,
                           g_psk_buf, strlen(ssid), strlen(g_psk_buf),
                           0, NULL);
    else
        ret = wifi_connect((char *)ssid, RTW_SECURITY_OPEN,
                           NULL, strlen(ssid), 0, 0, NULL);

    g_conn_state = (ret == RTW_SUCCESS) ? 2 : 3;

    /* 连接成功 → 自动写入 wapi.conf（start_wifi.sh 开机自动重连）。
     * g_psk_buf 在 state==1 期间不会被 UI 修改（connect_start 拒绝并发）。 */
    if (ret == RTW_SUCCESS)
        dm_net_wifi_save_conf(ssid, g_psk_buf[0] ? g_psk_buf : "");

    free((void *)ssid);
    return NULL;
}

int dm_net_wifi_connect_start(const char *ssid, const char *psk)
{
    if (ssid == NULL || g_conn_state == 1)
        return -1;

    g_psk_buf[0] = '\0';
    if (psk)
        strncpy(g_psk_buf, psk, sizeof(g_psk_buf) - 1);

    char *ssid_copy = strdup(ssid);
    if (ssid_copy == NULL)
        return -1;

    g_conn_state = 1;
    if (pthread_create(&g_conn_thread, NULL, dm_net_conn_worker,
                       ssid_copy) != 0) {
        free(ssid_copy);
        g_conn_state = 3;
        return -1;
    }
    pthread_detach(g_conn_thread);
    return 0;
}

int dm_net_wifi_connect_state(void) { return g_conn_state; }

/* 当前已连接网络的 SSID（wifi_get_setting 查询；未连接时置空） */
int dm_net_wifi_cur_ssid(char *buf, int buflen)
{
    rtw_wifi_setting_t st;

    if (buf == NULL || buflen <= 0)
        return -1;
    buf[0] = '\0';

    if (wifi_get_setting(WLAN0_NAME, &st) != RTW_SUCCESS)
        return -1;

    strncpy(buf, (char *)st.ssid, buflen - 1);
    buf[buflen - 1] = '\0';
    return 0;
}

/* 写入 wapi.conf（start_wifi.sh 开机读取，JSON 格式） */
int dm_net_wifi_save_conf(const char *ssid, const char *psk)
{
    FILE *f;
    char dir[64];

    snprintf(dir, sizeof(dir), "/data/etc/wifi");
    mkdir(dir, 0755);   /* 目录可能不存在（首次写入） */

    f = fopen("/data/etc/wifi/wapi.conf", "w");
    if (f == NULL)
        return -1;

    fprintf(f, "{\"ssid\":\"%s\",\"psk\":\"%s\",\"bssid\":\"\"}\n",
            ssid ? ssid : "", psk ? psk : "");
    fclose(f);
    return 0;
}

int dm_net_wifi_is_up(void)
{
    return wifi_is_up(RTW_STA_INTERFACE);
}

void dm_net_wifi_set(int on)
{
    if (on)
        wifi_on(RTW_MODE_STA);
    else
        wifi_off();
}

int dm_net_bt_is_on(void)
{
    bt_adapter_state_t st = g_bt_state;

    if (g_bt_ins && (++g_bt_tick % 10 == 0))
        st = bt_adapter_get_state(g_bt_ins);

    return (st == BT_ADAPTER_STATE_ON || st == BT_ADAPTER_STATE_BLE_ON);
}

void dm_net_bt_set(int on)
{
    if (g_bt_ins == NULL)
        g_bt_ins = bluetooth_create_instance();
    if (g_bt_ins == NULL)
        return;

    if (on) {
        bt_adapter_enable(g_bt_ins);
        g_bt_state = BT_ADAPTER_STATE_ON;
    } else {
        bt_adapter_disable(g_bt_ins);
        g_bt_state = BT_ADAPTER_STATE_OFF;
    }
}
```

### 4.3 `apps/luncher_dm/deskmate_ui.c` 关键段（WiFi 子页 / 蓝牙开关 / NETWORK 行）

**4.3.1 网络相关全局变量与前向声明**：
```c
/* 前向声明（settings 音量回调定义在 g_music_volume 之后，此处先声明） */
static void settings_volume_cb(lv_event_t *e);
/* 网络接线（2026-08-08）：Settings WiFi/蓝牙开关回调 + 状态栏图标刷新 */
static void settings_wifi_cb(lv_event_t *e);
static void settings_bt_cb(lv_event_t *e);
static void settings_wifi_row_open_cb(lv_event_t *e);
static void dm_net_status_refresh(void);
```
```c
static void create_settings_subpage(lv_obj_t *parent);
static void create_wifi_subpage(lv_obj_t *parent);   /* 2026-08-08 WiFi 管理 */
```
```c
/* 网络状态（2026-08-08 接线驱动）：状态栏图标 + Settings 开关直连
 * 驱动 API 封装在 dm_net.c（独立编译单元，规避 dlist.h/aw_list.h 冲突） */
static lv_obj_t      *status_wifi_lbl;
static lv_obj_t      *status_bt_lbl;

/* WiFi 管理子页（2026-08-08 手机式接入）状态 */
static lv_obj_t      *wifi_status_lbl;    /* 子页顶部当前连接状态 */
static lv_obj_t      *wifi_list_cont;     /* SSID 清单容器（重建用） */
static lv_timer_t    *wifi_timer;         /* 扫描/连接轮询（500ms） */
static lv_obj_t      *wifi_pwd_overlay;   /* 密码输入层 */
static lv_obj_t      *wifi_pwd_ta;        /* 密码 textarea */
static char           wifi_pwd_ssid[33];  /* 待连接 SSID */
static int            wifi_last_conn_state;
```

**4.3.2 show_subpage 分发（新增 "WiFi" 分支）**：
```c
    else if (strcmp(title, "AI") == 0)       create_ai_subpage(content);
    else if (strcmp(title, "Settings") == 0) create_settings_subpage(content);
    else if (strcmp(title, "WiFi") == 0)     create_wifi_subpage(content);
```

**4.3.3 状态栏图标刷新（挂 clock timer，1s 一次）**：
```c
    /* 网络状态图标（2026-08-08 接线驱动，每秒刷新一次） */
    dm_net_status_refresh();
```
```c
/* 状态栏 wifi/bt 图标 ← 驱动实时状态（每秒由 clock timer 调用） */
static void dm_net_status_refresh(void)
{
    if (status_wifi_lbl) {
        int up = dm_net_wifi_is_up();
        lv_obj_set_style_text_color(status_wifi_lbl,
            up ? lv_color_hex(COL_BLUE) : lv_color_hex(COL_SEC), 0);
    }
    if (status_bt_lbl) {
        int on = dm_net_bt_is_on();   /* socket IPC 低频刷新在 dm_net.c 内部 */
        lv_obj_set_style_text_color(status_bt_lbl,
            on ? lv_color_hex(COL_BLUE) : lv_color_hex(COL_SEC), 0);
    }
}

/* Settings → WiFi 开关（2026-08-08 接线 RTL8733BS） */
static void settings_wifi_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    dm_net_wifi_set(on ? 1 : 0);
    dm_net_status_refresh();
}

/* Settings → 蓝牙开关（2026-08-08 接线 bluetoothd，实例懒创建） */
static void settings_bt_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    dm_net_bt_set(on ? 1 : 0);
    dm_net_status_refresh();
}
```

**4.3.4 Settings NETWORK 段（WiFi 行保留开关 + 整行点击进子页）**：
```c
    /* ═══════ NETWORK ═══════ */
    settings_section_label(parent, "NETWORK");
    lv_obj_t *net = settings_group_card(parent);
    /* 2026-08-08 接线：初始状态读真实驱动状态（wifi_is_up / bt state） */
    int wifi_up = dm_net_wifi_is_up();
    lv_obj_t *wifi_row = settings_row_switch_cb(net, LV_SYMBOL_WIFI, COL_BLUE,
                                                "WiFi", wifi_up > 0,
                                                settings_wifi_cb);
    /* 点击 WiFi 行（非开关处）→ 进入 WiFi 管理子页（手机式 SSID 清单） */
    lv_obj_add_flag(wifi_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(wifi_row, settings_wifi_row_open_cb,
                        LV_EVENT_CLICKED, NULL);
    settings_add_separator(net);
    int bt_on = dm_net_bt_is_on();
    settings_row_switch_cb(net, LV_SYMBOL_BLUETOOTH, COL_BLUE, "Bluetooth",
                           bt_on, settings_bt_cb);
```

**4.3.5 settings_row_switch_cb（带回调变体，LV_EVENT_VALUE_CHANGED）**：
```c
static lv_obj_t *settings_row_switch_cb(lv_obj_t *card, const char *icon,
                                        uint32_t icon_color, const char *label,
                                        bool initial_state, lv_event_cb_t cb)
{
    lv_obj_t *row = make_clean_cont(card);
    lv_obj_set_size(row, lv_pct(100), SETTING_ROW_H);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);

    /* Icon badge */
    lv_obj_t *badge = lv_obj_create(row);
    lv_obj_set_size(badge, DM(36), DM(36));
    lv_obj_set_style_radius(badge, RAD_CARD, 0);
    lv_obj_set_style_bg_color(badge, lv_color_hex(icon_color), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(badge, 8, 0);
    lv_obj_set_style_shadow_color(badge, lv_color_hex(icon_color), 0);
    lv_obj_set_style_shadow_opa(badge, LV_OPA_30, 0);
    lv_obj_set_style_border_width(badge, 0, 0);
    lv_obj_set_style_pad_all(badge, 0, 0);
    lv_obj_set_scrollbar_mode(badge, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ic = lv_label_create(badge);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_font(ic, FONT_ICON, 0);
    lv_obj_set_style_text_color(ic, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(ic);

    /* Label */
    lv_obj_t *lb = lv_label_create(row);
    lv_label_set_text(lb, label);
    lv_obj_set_style_text_font(lb, FONT_BODY, 0);
    lv_obj_set_style_text_color(lb, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_flex_grow(lb, 1);

    /* Switch */
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, DM(44), DM(24));
    if (initial_state)
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    if (cb)
        lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);

    return row;
}

/* No-callback variant (all other settings rows) */
static lv_obj_t *settings_row_switch(lv_obj_t *card, const char *icon,
                                     uint32_t icon_color, const char *label,
                                     bool initial_state)
{
    return settings_row_switch_cb(card, icon, icon_color, label,
                                  initial_state, NULL);
}
```

**4.3.6 WiFi 管理子页实现（完整）**：
```c
/* ================================================================
 * WIFI 管理子页（2026-08-08 手机式接入）
 * SSID 清单（wifi_scan_networks 异步扫描）→ 点击 AP →
 *    OPEN 直连 / 加密弹软键盘输密码 → wifi_connect 后台验证 →
 *    成功写 wapi.conf（开机自动重连）+ 列表打勾 + 状态栏变蓝
 * ================================================================ */

/* Settings WiFi 行点击（非开关处）→ 打开 WiFi 管理子页 */
static void settings_wifi_row_open_cb(lv_event_t *e)
{
    /* LV_EVENT_CLICKED 会冒泡：事件 target 是子对象（switch）时忽略，
     * 只有点击行本体才进子页（开关的 VALUE_CHANGED 由 settings_wifi_cb 处理） */
    if (lv_event_get_target(e) != lv_event_get_current_target(e))
        return;
    show_subpage("WiFi");
}

static void wifi_scan_btn_cb(lv_event_t *e)
{
    (void)e;
    dm_net_wifi_scan_start();
    if (wifi_status_lbl)
        lv_label_set_text(wifi_status_lbl, "扫描中…");
}

/* 密码输入层：确认 → 起后台连接；取消 → 关闭输入层 */
static void wifi_pwd_ok_cb(lv_event_t *e)
{
    (void)e;
    const char *psk = wifi_pwd_ta ? lv_textarea_get_text(wifi_pwd_ta) : "";
    if (wifi_pwd_ssid[0]) {
        dm_net_wifi_connect_start(wifi_pwd_ssid, psk[0] ? psk : NULL);
        if (wifi_status_lbl)
            lv_label_set_text_fmt(wifi_status_lbl, "正在连接 %s…", wifi_pwd_ssid);
    }
    if (wifi_pwd_overlay) {
        lv_obj_del(wifi_pwd_overlay);
        wifi_pwd_overlay = NULL;
        wifi_pwd_ta     = NULL;
    }
}

static void wifi_pwd_cancel_cb(lv_event_t *e)
{
    (void)e;
    if (wifi_pwd_overlay) {
        lv_obj_del(wifi_pwd_overlay);
        wifi_pwd_overlay = NULL;
        wifi_pwd_ta     = NULL;
    }
}

/* 点击清单中的某个 AP 行 */
static void wifi_ap_click_cb(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(row);
    dm_net_ap_t ap;

    if (dm_net_wifi_scan_get(idx, &ap) != 0)
        return;

    if (ap.security == 0) {          /* OPEN：直连无需密码 */
        dm_net_wifi_connect_start(ap.ssid, NULL);
        if (wifi_status_lbl)
            lv_label_set_text_fmt(wifi_status_lbl, "正在连接 %s…", ap.ssid);
        return;
    }

    /* 加密网络：弹出密码输入层（全屏，覆盖在子页之上） */
    if (wifi_pwd_overlay)
        return;
    strncpy(wifi_pwd_ssid, ap.ssid, sizeof(wifi_pwd_ssid) - 1);

    lv_obj_t *scr = lv_scr_act();
    wifi_pwd_overlay = lv_obj_create(scr);
    lv_obj_set_size(wifi_pwd_overlay,
                    lv_disp_get_hor_res(lv_disp_get_default()),
                    lv_disp_get_ver_res(lv_disp_get_default()));
    lv_obj_set_style_bg_color(wifi_pwd_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(wifi_pwd_overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(wifi_pwd_overlay, 0, 0);
    lv_obj_set_style_pad_all(wifi_pwd_overlay, 0, 0);
    lv_obj_clear_flag(wifi_pwd_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(wifi_pwd_overlay);

    lv_obj_t *box = lv_obj_create(wifi_pwd_overlay);
    lv_obj_set_size(box, lv_pct(80), LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(box, DM(20), 0);
    lv_obj_set_style_radius(box, RAD_CARD, 0);

    lv_obj_t *ttl = lv_label_create(box);
    lv_label_set_text_fmt(ttl, "连接 %s", wifi_pwd_ssid);
    lv_obj_set_style_text_font(ttl, FONT_TITLE, 0);
    lv_obj_set_style_text_color(ttl, lv_color_hex(COL_TEXT), 0);

    wifi_pwd_ta = lv_textarea_create(box);
    lv_obj_set_size(wifi_pwd_ta, lv_pct(100), DM(60));
    lv_textarea_set_password_mode(wifi_pwd_ta, true);
    lv_textarea_set_one_line(wifi_pwd_ta, true);
    lv_textarea_set_placeholder_text(wifi_pwd_ta, "输入密码");
    lv_obj_set_style_text_font(wifi_pwd_ta, FONT_BODY, 0);

    lv_obj_t *kb = lv_keyboard_create(wifi_pwd_overlay);
    lv_keyboard_set_textarea(kb, wifi_pwd_ta);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_obj_t *btnrow = lv_obj_create(box);
    lv_obj_set_size(btnrow, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btnrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_bg_opa(btnrow, LV_OPA_0, 0);
    lv_obj_set_style_border_width(btnrow, 0, 0);
    lv_obj_set_style_pad_all(btnrow, 0, 0);
    lv_obj_set_style_pad_row(btnrow, 0, 0);

    lv_obj_t *ok = lv_btn_create(btnrow);
    lv_obj_set_flex_grow(ok, 1);
    lv_obj_add_event_cb(ok, wifi_pwd_ok_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ok_l = lv_label_create(ok);
    lv_label_set_text(ok_l, "连接");
    lv_obj_set_style_text_font(ok_l, FONT_BODY, 0);

    lv_obj_t *cancel = lv_btn_create(btnrow);
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_add_event_cb(cancel, wifi_pwd_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_l = lv_label_create(cancel);
    lv_label_set_text(cancel_l, "取消");
    lv_obj_set_style_text_font(cancel_l, FONT_BODY, 0);
}

/* 重建 SSID 清单（扫描完成后调用；行 user_data = AP 索引） */
static void wifi_rebuild_list(void)
{
    if (wifi_list_cont == NULL)
        return;

    lv_obj_clean(wifi_list_cont);
    char cur[33] = {0};
    dm_net_wifi_cur_ssid(cur, sizeof(cur));

    int n = dm_net_wifi_scan_count();
    for (int i = 0; i < n; i++) {
        dm_net_ap_t ap;
        if (dm_net_wifi_scan_get(i, &ap) != 0)
            break;

        lv_obj_t *row = lv_obj_create(wifi_list_cont);
        lv_obj_set_size(row, lv_pct(100), DM(52));
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_color(row, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_70, 0);
        lv_obj_set_style_radius(row, RAD_CARD, 0);
        lv_obj_set_style_pad_left(row, DM(12), 0);
        lv_obj_set_style_pad_right(row, DM(12), 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, wifi_ap_click_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_set_user_data(row, (void *)(intptr_t)i);

        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text(nm, ap.ssid);
        lv_obj_set_style_text_font(nm, FONT_BODY, 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(COL_TEXT), 0);
        lv_obj_set_flex_grow(nm, 1);

        /* 右侧：已连接 / 加密锁 + 信号 */
        char meta[48];
        if (cur[0] && strcmp(ap.ssid, cur) == 0)
            snprintf(meta, sizeof(meta), "%s 已连接", LV_SYMBOL_WIFI);
        else
            snprintf(meta, sizeof(meta), "%s%s %d dBm",
                     ap.security ? LV_SYMBOL_BELL : LV_SYMBOL_WIFI,
                     ap.security ? "" : "", (int)ap.rssi);
        lv_obj_t *mt = lv_label_create(row);
        lv_label_set_text(mt, meta);
        lv_obj_set_style_text_font(mt, FONT_BODY, 0);
        lv_obj_set_style_text_color(mt,
            (cur[0] && strcmp(ap.ssid, cur) == 0)
                ? lv_color_hex(COL_GREEN) : lv_color_hex(COL_SEC), 0);
    }
}

/* 轮询：扫描完成 → 重建清单；连接状态变化 → 更新状态/保存配置 */
static void wifi_poll_cb(lv_timer_t *timer)
{
    (void)timer;
    if (dm_net_wifi_scan_done() && wifi_list_cont) {
        wifi_rebuild_list();
    }

    int st = dm_net_wifi_connect_state();
    if (st != wifi_last_conn_state && wifi_status_lbl) {
        if (st == 2) {   /* 连接成功：记住配置 + 刷新状态栏/清单 */
            char cur[33] = {0};
            dm_net_wifi_cur_ssid(cur, sizeof(cur));
            dm_net_wifi_save_conf(cur, NULL);   /* psk 已由 dm_net 线程保存 */
            lv_label_set_text_fmt(wifi_status_lbl, "已连接 %s", cur);
            wifi_rebuild_list();
            dm_net_status_refresh();
        } else if (st == 3) {
            lv_label_set_text(wifi_status_lbl,
                              "连接失败：密码错误或无法连接");
        }
        wifi_last_conn_state = st;
    }
}

static void create_wifi_subpage(lv_obj_t *parent)
{
    subpage_big_title(parent, "WiFi");

    /* 顶部：当前连接状态 + 扫描按钮 */
    lv_obj_t *head = make_clean_cont(parent);
    lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_bottom(head, 8, 0);

    wifi_status_lbl = lv_label_create(head);
    lv_obj_set_style_text_font(wifi_status_lbl, FONT_BODY, 0);
    lv_obj_set_style_text_color(wifi_status_lbl, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_flex_grow(wifi_status_lbl, 1);

    lv_obj_t *scan_btn = lv_btn_create(head);
    lv_obj_set_size(scan_btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_add_event_cb(scan_btn, wifi_scan_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *scan_lbl = lv_label_create(scan_btn);
    lv_label_set_text(scan_lbl, "重新扫描");
    lv_obj_set_style_text_font(scan_lbl, FONT_BODY, 0);

    /* SSID 清单容器（扫描结果填充） */
    wifi_list_cont = lv_obj_create(parent);
    lv_obj_set_size(wifi_list_cont, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wifi_list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(wifi_list_cont, LV_OPA_0, 0);
    lv_obj_set_style_border_width(wifi_list_cont, 0, 0);
    lv_obj_set_style_pad_all(wifi_list_cont, 0, 0);
    lv_obj_set_style_pad_row(wifi_list_cont, 4, 0);
    lv_obj_set_style_radius(wifi_list_cont, 0, 0);
    lv_obj_clear_flag(wifi_list_cont, LV_OBJ_FLAG_SCROLLABLE);

    /* 轮询 timer（500ms）：扫描完成重建清单、连接状态刷新 */
    wifi_last_conn_state = dm_net_wifi_connect_state();
    wifi_timer = lv_timer_create(wifi_poll_cb, 500, NULL);
    lv_timer_set_repeat_count(wifi_timer, -1);

    /* 进入子页即自动扫描 */
    dm_net_wifi_scan_start();
    if (wifi_status_lbl)
        lv_label_set_text(wifi_status_lbl, "扫描中…");
}
```

---

## 五、🔴 已知问题（2026-08-09 更新：根因已由日志铁证定位，见 §0）

> **历史背景**：用户最早反馈（8-8，ok-20260808-21）"WIFI只有开和关，点击WIFI本项目进去不能看到SSID列表"。经 8-8 修复链（-22 防重入回归 → -24 嵌套导航修复 → -25/-26 扫描失败处理）与 8-9 上板日志核对，**点击入口问题已闭环**（§0 结论 1），**当前唯一拦路虎 = WiFi 固件下载失败（download_fw FAIL status=0x27）**（§0 结论 2）。以下 H1~H5 为历史排查记录（§0 结论已更新，**不再作为主攻方向**）。

### 🔴 当前主攻：WiFi 固件下载失败（§0 结论 2，以 §0 为准）

- 现象：`RTL871X: download_fw: download firmware FAIL! status=0x27` → `rtl8723f_hal_init Download Firmware from file failed` → `drv_open fail, bup=0`
- **已排除路径拍平**（8-9 genromfs 实验）：res.fex 打包层级正常，`/resource/etc/wifi/FW_NIC_ACUT.bin` 大概率在。
- 上板验证（必须，本地无法替代）：`ls -l /resource/etc/wifi/` + `cat /resource/etc/wifi/FW_NIC_ACUT.bin | wc -c`（应=120424）。
- 驱动层嫌疑排序：①`rtw_retrieve_from_file`（osdep_service.c:1408）open/read 失败原因 ②SDIO 时序/芯片复位（与蓝牙 110 同源嫌疑：`/dev/gpio2` 复位电平、供电）③固件版本与芯片 cut 不匹配（ACUT vs BCUT，cut_version==0 用 ACUT）。

### 其他已确认事实

- 状态栏 wifi/bt 图标已接线（`dm_net_status_refresh` 每秒刷新，连上=蓝/断开=灰）。
- `wifi_on(RTW_MODE_STA)` 与 `wifi_scan_networks` 在 `apps/wifi_test/test_sdio_wifi.c` 有现成对照用法（`test_wifi init / sta start / sta scan`）。

---

### 📦 历史排查归档（H1~H5，8-9 前结论，仅供参考）

| # | 假设 | 结论 |
|---|------|------|
| H1 | WiFi 行点击事件未触发（进不去子页） | **已证伪**（8-9 日志 `Open subpage: WiFi` 出现，点击链路正常）；历史根因=-22 防重入拦掉嵌套，-24 已用 subpage_parent 修复 |
| H2 | 子页进去了但扫描无结果（空清单/"扫描中…"卡住） | **UI 层已修**（-26：scan_start 不查返回值→g_scan_fail+失败提示）；驱动层根因实为 §0 固件下载 0x27 |
| H3 | `dm_net_wifi_scan_get` 索引错位 / user_data 类型 | 低概率，非当前主因 |
| H4 | 子页 UI 构建问题（白屏/异常） | **已排除**（8-9 日志子页正常打开） |
| H5 | 蓝牙未实现设备清单 | **预期内**：UI 已补（-22/-23），但蓝牙 H4 open 110 是驱动层问题（§0 结论 3），待驱动修好才能验证 |

---

## 六、给 ChatGPT 的开发指引（分析任务清单）

> 请基于本文档给出**完整开发方案**，按以下顺序输出；涉及代码修改时给出可直接落地的 diff 级建议（注意 AGENTS.md 的 dlist.h/aw_list.h 编译坑——WiFi/蓝牙头只能进 dm_net.c）。

1. **🔴 最高优先：定位并修复 WiFi 固件下载失败（download_fw FAIL status=0x27）**（§0 结论 2）：
   - 核实 res.fex 打包路径：`tools/scripts/pack_img.sh` 的 `make_res_image()`（约 560 行）数据源 = `board/${PACK_PROJECT_PATH}/data/res`（gemini-s1 无独立 data/res → 用 `board/common/data/res`），genromfs 参数 `-d $1 -V X4BResource -a 2048`。
   - **关键疑点**：romfs 二进制里 `etc/wifi/` 路径前缀计数=0 而 `FW_NIC_ACUT.bin` 字符串存在（数据 120424B 与源一致）→ 固件疑似被拍平到 romfs 根目录。给出：①上板 `ls -l /resource/` 验证命令；②若确为路径问题，修复打包数据源目录结构（或把 `Make.defs` 的 `CCVWIFIFW_PATH`/`WIFIFW_PATH` 宏改指实际路径）；③若路径正确仍 0x27，查 SDIO 固件下载时序/芯片复位（与蓝牙 110 同源嫌疑）。
   - 同时核对 cut_version：`rtw_get_fw_file_path_from_os` 里 cut_version==0 用 ACUT，否则用 BCUT（两个文件都要进镜像）。
2. **WiFi 完整接入收尾**（固件修好后）：SSID 清单去重/排序（按 RSSI）、信号图标分级、加密锁图标、已连接打勾、连接状态机（connecting→ok/fail、60s 超时）、断开/忘记网络交互、wapi.conf 与 start_wifi.sh 兼容性核对。
3. **蓝牙完整接入**（驱动 110 修好后）：附近设备清单、点击配对（含配对请求弹窗回复）、已配对设备管理（connect/disconnect/remove）、状态栏图标联动；注意 bluetoothd socket IPC 回调线程与 LVGL 单线程的同步（参考 wifi_poll_cb 轮询法）。
4. **线程/生命周期审计**：扫描回调（RTW 线程）与连接线程（pthread）静态数据竞争；子页关闭时 `wifi_timer`/`bt_timer`/`wifi_pwd_overlay`/`bt_pair_overlay` 的销毁（-22/-24 已补，复核）。
5. **验收清单**：正常连播/扫描 2h、疯狂切换 100 次、异常场景（拔卡/坏文件/驱动未就绪）。

---

## 七、关键文件索引

| 文件 | 作用 |
|------|------|
| `apps/luncher_dm/deskmate_ui.c` | UI 唯一源（WiFi/蓝牙子页、状态栏图标、Settings 行） |
| `apps/luncher_dm/dm_net.c/h` | 驱动封装独立编译单元（WiFi/蓝牙 API 唯一入口） |
| `apps/luncher_dm/Makefile` | include 接线（AW_ADD_WIFI_INCLUDES + BT framework） |
| `vendor/allwinnertech/boards/r528/drivers/realtek_ieee80211/Make.defs` | 🔴 固件路径宏 `CCVWIFIFW_PATH`/`WIFIFW_PATH`（`/resource/etc/wifi/FW_NIC_{A,B}CUT.bin`） |
| `vendor/allwinnertech/lichee/board/common/data/res/etc/wifi/` | 🔴 WiFi 固件源（FW_NIC_ACUT.bin / FW_NIC_BCUT.bin / wapi.conf） |
| `vendor/allwinnertech/lichee/tools/scripts/pack_img.sh` | 🔴 res.fex 打包（`make_res_image` 约 560 行，genromfs -d/-V/-a） |
| `vendor/allwinnertech/boards/r528/drivers/realtek_ieee80211/os/os_dep/osdep_service.c` | `rtw_get_fw_file_path_from_os`/`rtw_retrieve_from_file` 固件加载实现 |
| `vendor/allwinnertech/boards/r528/drivers/realtek_ieee80211/api/wifi/wifi_conf.h/.c` | WiFi 驱动 API |
| `vendor/allwinnertech/boards/r528/drivers/realtek_ieee80211/include/wifi_structures.h` | rtw_ssid_t / rtw_scan_result_t / rtw_wifi_setting_t |
| `vendor/allwinnertech/boards/r528/drivers/realtek_ieee80211/include/wifi_constants.h` | RTW_SECURITY_* / WLAN0_NAME |
| `frameworks/connectivity/bluetooth/framework/include/bluetooth.h` | bt_instance_t / bluetooth_create_instance |
| `frameworks/connectivity/bluetooth/framework/include/bt_adapter.h` | 适配器控制 + 回调 |
| `frameworks/connectivity/bluetooth/framework/include/bt_device.h` | 配对/连接/断开 |
| `apps/bt_instance/bt_start.c` | 蓝牙框架参考用法 |
| `apps/wifi_test/test_sdio_wifi.c` | WiFi 驱动参考用法（test_wifi init/sta/scan） |
| `boards/r528/r528s3-gemini-s1/src/etc/init.d/rcS.nsh` | bluetoothd / start_wifi.sh 启动 |
| `boards/r528/r528s3-gemini-s1/src/etc/wifi/start_wifi.sh` | 开机自动连接（读 wapi.conf） |

---

*wifidev.md by AtomCode (deepseek-v4-flash) · 2026-08-08（§0 更新 2026-08-09）· 供 ChatGPT 分析开发指引*


