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

#include <stdint.h>   /* uint8_t（蓝牙地址） */

/* @return 1 = STA 接口 up（radio on / 已连接），0 = down */
int  dm_net_wifi_is_up(void);

/* @return 1 = 已连接 AP（有实际网络），0 = 未连（语音/联网功能不可用） */
int  dm_net_wifi_connected(void);

/* @param on 1 = wifi_on(RTW_MODE_STA)，0 = wifi_off() */
void dm_net_wifi_set(int on);

/* @return 1 = 蓝牙适配器已开启（ON/BLE_ON），0 = 关闭 */
int  dm_net_bt_is_on(void);

/* @param on 1 = bt_adapter_enable，0 = bt_adapter_disable（实例懒创建） */
void dm_net_bt_set(int on);

/* ================================================================
 * 蓝牙管理（wifijianyi.md：设备发现去重 + 配对请求 + IPC 断线重连）
 * 回调运行在 bluetoothd socket 线程——只更新静态缓存，UI 轮询拉取。
 * ================================================================ */

#define DM_NET_BT_MAX 24

typedef struct {
    char   name[65];     /* 设备名（含 \0，强制截断） */
    uint8_t addr[6];     /* MAC（BT_ADDR_LENGTH=6） */
    int    paired;       /* 是否已配对（预留） */
} dm_net_bt_dev_t;

/* 启动发现（15s 超时）；仅在适配器已 ON 时执行（状态抢跑防御） */
int  dm_net_bt_start_scan(void);
int  dm_net_bt_stop_scan(void);
int  dm_net_bt_scan_done(void);       /* 1 = discovery 结束 */
int  dm_net_bt_dev_count(void);
int  dm_net_bt_dev_get(int idx, dm_net_bt_dev_t *dev);

/* 配对请求（on_pair_request 回调缓存，UI 弹窗）：
 * pending=1 有待响应请求；取地址；回复 accept=1/0 */
int  dm_net_bt_pair_pending(void);
int  dm_net_bt_pair_get_addr(uint8_t addr[6]);
void dm_net_bt_pair_reply(int accept);

/* 可发现模式（Discoverable）：on=1 开放可见（bt_adapter_set_scan_mode
 * connectable+discoverable），on=0 恢复仅可连接 */
void dm_net_bt_set_discoverable(int on);

/* 已配对设备列表（bt_adapter_get_bonded_devices 查询，UI 轮询刷新）：
 * refresh 后 count/get 读取；设备名查不到时置空由 UI 显示"未知设备" */
int  dm_net_bt_paired_refresh(void);
int  dm_net_bt_paired_count(void);
int  dm_net_bt_paired_get(int idx, dm_net_bt_dev_t *dev);

/* 连接/断开指定设备（addr 为 6 字节 MAC） */
int  dm_net_bt_connect(const uint8_t addr[6]);
int  dm_net_bt_disconnect(const uint8_t addr[6]);

/* 2026-09-10 P206：同步查询对端是否已连（bt_device_is_connected 封装），
 * 供 UI 轮询回读连接结果；@return 1=已连接，0=未连/参数错 */
int  dm_net_bt_is_connected(const uint8_t addr[6]);

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

/* 发起异步扫描；返回 0=已启动（结果就绪后 scan_done()==1），
 * -1 = 驱动忙/超时（scan_failed()==1，UI 应提示重试） */
int  dm_net_wifi_scan_start(void);
int  dm_net_wifi_scan_done(void);
int  dm_net_wifi_scan_failed(void);
int  dm_net_wifi_scan_count(void);
int  dm_net_wifi_scan_get(int idx, dm_net_ap_t *ap);

/* 异步连接：起线程调 wifi_connect（阻塞最长 60s，不能占 UI 线程）。
 * psk 可为 NULL（OPEN 网络）。连接状态见 dm_net_wifi_connect_state。
 * 连接成功时自动写入 wapi.conf（开机自动重连）。 */
int  dm_net_wifi_connect_start(const char *ssid, const char *psk);
int  dm_net_wifi_connect_state(void);   /* 0=idle 1=connecting 2=ok 3=fail */

/* UI 取消/退出子页时调用：递增连接代数让在途线程丢弃结果并复位 idle */
void dm_net_wifi_connect_abort(void);

/* 当前已连接网络的 SSID（经 wifi_get_setting 查询；未连接时置空） */
int  dm_net_wifi_cur_ssid(char *buf, int buflen);

/* 2026-09-10 P206：断开当前 AP（wifi_disconnect 封装；无 forget 接口，
 * 详见 dm_net.c）；@return 0=成功，-1=失败 */
int  dm_net_wifi_disconnect(void);

/* 连接成功后把 SSID/PSK 写入 /data/etc/wifi/wapi.conf（start_wifi.sh
 * 开机读取自动重连，JSON: {"ssid":"..","psk":"..","bssid":""}） */
int  dm_net_wifi_save_conf(const char *ssid, const char *psk);

/* 读取已保存配置中的 SSID（解析 /data/etc/wifi/wapi.conf JSON；
 * 无保存文件/解析失败时 buf 置空并返回 -1）——WiFi 子页"已保存网络" */
int  dm_net_wifi_saved_ssid(char *buf, int buflen);

#endif /* __DM_NET_H */
