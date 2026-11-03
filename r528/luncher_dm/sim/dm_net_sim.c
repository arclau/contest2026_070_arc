/* dm_net_sim.c — goldfish 模拟器用 WiFi/BT 假状态实现
 *
 * 实现 dm_net.h 全部 API：WiFi 开关/扫描/连接均为假状态机，
 * BT 扫描返回 2 个假设备。UI 流程可在无 RTL8733BS/bluetoothd 时走通。
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "dm_net.h"

static int s_wifi_on;
static int s_wifi_connected;
static int s_bt_on;
static int s_bt_disc;
static int s_scan_done;
static int s_paired_cnt;

/* ---------------- WiFi ---------------- */

int dm_net_wifi_is_up(void) { return s_wifi_on; }
int dm_net_wifi_connected(void) { return s_wifi_connected; }

void dm_net_wifi_set(int on)
{
    s_wifi_on = on ? 1 : 0;
    if (!on) s_wifi_connected = 0;
    printf("[sim-net] wifi_set(%d)\n", on);
}

int dm_net_wifi_scan_start(void)
{
    s_scan_done = 0;
    return 0;
}

int dm_net_wifi_scan_done(void) { return s_scan_done; }
int dm_net_wifi_scan_failed(void) { return 0; }

int dm_net_wifi_scan_count(void) { return 2; }

int dm_net_wifi_scan_get(int idx, dm_net_ap_t *ap)
{
    if (!ap || idx < 0 || idx > 1) return -1;
    memset(ap, 0, sizeof(*ap));
    if (idx == 0) {
        snprintf(ap->ssid, sizeof(ap->ssid), "OpenVela-Demo");
        ap->rssi = -42;
        ap->security = 0;
    } else {
        snprintf(ap->ssid, sizeof(ap->ssid), "MiOffice-5G");
        ap->rssi = -67;
        ap->security = 0;
    }
    return 0;
}

int dm_net_wifi_connect_start(const char *ssid, const char *psk)
{
    (void)psk;
    printf("[sim-net] wifi connect -> %s\n", ssid ? ssid : "?");
    s_wifi_connected = 1;
    return 0;
}

int dm_net_wifi_connect_state(void) { return s_wifi_connected ? 2 : 0; }
void dm_net_wifi_connect_abort(void) { s_wifi_connected = 0; }

int dm_net_wifi_cur_ssid(char *buf, int buflen)
{
    if (!buf || buflen <= 0) return -1;
    if (s_wifi_connected) {
        snprintf(buf, buflen, "OpenVela-Demo");
    } else {
        buf[0] = '\0';
    }
    return 0;
}

int dm_net_wifi_disconnect(void)
{
    s_wifi_connected = 0;
    return 0;
}

int dm_net_wifi_save_conf(const char *ssid, const char *psk)
{
    (void)ssid; (void)psk;
    return 0;
}

int dm_net_wifi_saved_ssid(char *buf, int buflen)
{
    if (!buf || buflen <= 0) return -1;
    snprintf(buf, buflen, "OpenVela-Demo");
    return 0;
}

/* ---------------- Bluetooth ---------------- */

int dm_net_bt_is_on(void) { return s_bt_on; }

void dm_net_bt_set(int on)
{
    s_bt_on = on ? 1 : 0;
    printf("[sim-net] bt_set(%d)\n", on);
}

int dm_net_bt_start_scan(void)
{
    s_scan_done = 0;
    return 0;
}

int dm_net_bt_stop_scan(void)
{
    s_scan_done = 1;
    return 0;
}

int dm_net_bt_scan_done(void) { return s_scan_done; }

int dm_net_bt_dev_count(void) { return s_bt_on ? 2 : 0; }

int dm_net_bt_dev_get(int idx, dm_net_bt_dev_t *dev)
{
    if (!dev || idx < 0 || idx > 1) return -1;
    memset(dev, 0, sizeof(*dev));
    if (idx == 0) {
        snprintf(dev->name, sizeof(dev->name), "Mi Buds 4");
        dev->addr[0] = 0x11; dev->addr[1] = 0x22; dev->addr[2] = 0x33;
        dev->addr[3] = 0x44; dev->addr[4] = 0x55; dev->addr[5] = 0x66;
    } else {
        snprintf(dev->name, sizeof(dev->name), "JBL Flip 6");
        dev->addr[0] = 0xaa; dev->addr[1] = 0xbb; dev->addr[2] = 0xcc;
        dev->addr[3] = 0xdd; dev->addr[4] = 0xee; dev->addr[5] = 0xff;
    }
    return 0;
}

int dm_net_bt_pair_pending(void) { return 0; }

int dm_net_bt_pair_get_addr(uint8_t addr[6])
{
    (void)addr;
    return -1;
}

void dm_net_bt_pair_reply(int accept) { (void)accept; }

void dm_net_bt_set_discoverable(int on)
{
    s_bt_disc = on ? 1 : 0;
    printf("[sim-net] bt_set_discoverable(%d)\n", on);
}

int dm_net_bt_is_discoverable(void) { return s_bt_disc; }

int dm_net_bt_avrcp_take(void) { return 0; }
int dm_net_bt_a2dp_streaming(void) { return 0; }

int dm_net_bt_paired_refresh(void)
{
    s_paired_cnt = 1;
    return 0;
}

int dm_net_bt_paired_count(void) { return s_paired_cnt; }

int dm_net_bt_paired_get(int idx, dm_net_bt_dev_t *dev)
{
    if (!dev || idx != 0) return -1;
    memset(dev, 0, sizeof(*dev));
    snprintf(dev->name, sizeof(dev->name), "Mi Buds 4");
    dev->addr[0] = 0x11; dev->addr[1] = 0x22; dev->addr[2] = 0x33;
    dev->addr[3] = 0x44; dev->addr[4] = 0x55; dev->addr[5] = 0x66;
    dev->paired = 1;
    return 0;
}

int dm_net_bt_connect(const uint8_t addr[6]) { (void)addr; return 0; }
int dm_net_bt_disconnect(const uint8_t addr[6]) { (void)addr; return 0; }
int dm_net_bt_is_connected(const uint8_t addr[6]) { (void)addr; return 0; }
