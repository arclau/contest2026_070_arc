# DevLog 2026-08-10 — 状态栏恢复 + 子页修复 + WiFi/蓝牙 iPad 风格重设计（V0.0.2）

## 背景

8-9 遗留 ok-20260809-25（P27）后，上一任 AI 在 8-10 凌晨做了 ok-20260810-1~10：iPad 风格 WiFi/蓝牙子页（-8/-9）+ 所有子页右上角「迷你状态栏」与锁屏迷你状态栏（-10）。用户上板后认为「状态栏被搞坏」（主屏常驻 132px 状态栏设计被 -10 的小图标条替代），拍板：**不做新状态栏，回退上一版本**。

本日三段工作：
1. **-11 回退**到 ok-20260809-25，恢复原有状态栏
2. **-12 补 FLOATING**：回退后「点行进不去子页」复现 → 只补回 -10 的 overlay FLOATING 修复（不碰状态栏）
3. **-13 iPad 风格重设计**：用户要求用 deskmate-ui skill 重新设计 WiFi/蓝牙子页（像 iPad WiFi 页、风格对齐 Settings、修元素重叠、修扫描）

## 节点 1：-11 回退（状态栏恢复）

- **现象**：主屏顶部常驻状态栏（132px，时钟+天气+图标）在子页里被 -10 的「右上角迷你状态栏」取代，观感坏
- **根因**：ok-20260810-10 在 show_subpage 给所有子页加了 sbar（wifi+bt+时钟 TOP_RIGHT 小条），偏离「常驻状态栏」设计
- **方案**：用户拍板回退 → `git_snapshot.sh -r ok-20260809-25`（三仓 reset）
- **验证**：HEAD=91fa6fb0；grep 无 `subpage_clock_lbl/sbar` 残留；`create_status_bar`+`TOP_BAR_H=132` 原样在 ui_home.c

## 节点 2：-12 补 FLOATING（子页进不去修复）

- **现象**：回退后上板「进不去子页」（点 WiFi/蓝牙行无反应）
- **根因**：-10 的 `LV_OBJ_FLAG_FLOATING` 修复随回退丢失。scr 是 flex column 布局，subpage_overlay 留在 flex 流中被重排到屏幕外（日志：`Open subpage done 时 overlay y=0 而非 sh`），嵌套子页排到父 overlay 之后
- **方案**：只补两处 FLOATING（共 10 行，与 -10 逐字一致），**不碰状态栏/sbar**
  - `deskmate_ui.c` show_subpage：subpage_overlay 加 FLOATING
  - `ui_home.c` show_standby：standby_overlay 加 FLOATING（锁屏盖不全残留同源）
- **验证**：md5 a3898a8a；固化 **ok-20260810-12**

## 节点 3：-13 WiFi/蓝牙子页 iPad 风格重设计

- **现象**：WiFi 子页「WIFI 和扫描重叠在一起、扫描不出 SSID」；蓝牙同样元素重叠
- **根因**：
  - 重叠 = 子页 `parent` 缺 `LV_FLEX_FLOW_COLUMN`（标题/状态/列表全叠左上角）——-8 已修但随回退丢失
  - 扫描不出 = 扫描在 UI 线程同步调 `wifi_scan_networks`，驱动忙时内部等 2s 才返回 RTW_TIMEOUT（卡顿 + 无失败提示）；缺 `g_scan_busy` 防重入
- **方案**：移植上一任 AI 已验证的 iPad 风格成果（**不含 -10 状态栏**）：
  - `ui/ui_wifi.c` ← ok-20260810-9（621 行：状态卡 + Wi-Fi 主开关 + 已连接网络卡 + SSID 分组卡 + 其他网络手动加入 + 已保存网络卡 + 底部提示）
  - `ui/ui_bt.c` ← ok-20260810-8（394 行：状态卡 + 主开关/Discoverable + MY DEVICES + OTHER DEVICES）
  - `dm_net.c/h` ← ok-20260810-9（后台扫描线程 `dm_net_scan_worker` + `g_scan_busy` 防重入 + `dm_net_wifi_saved_ssid` 读 wapi.conf）
- **验证**：编译通过、pack 成功、nsh.fex=vela.bin（md5 b500f5ef）；固化 **ok-20260810-13**

### 改动文件表（-13）

| 文件 | 来源 | 变更 |
|------|------|------|
| `apps/luncher_dm/ui/ui_wifi.c` | ok-20260810-9 | iPad 风格 WiFi 页（+340） |
| `apps/luncher_dm/ui/ui_bt.c` | ok-20260810-8 | iPad 风格蓝牙页（+96） |
| `apps/luncher_dm/dm_net.c` | ok-20260810-9 | 扫描移后台线程 + busy 防重入 + saved_ssid（+80） |
| `apps/luncher_dm/dm_net.h` | ok-20260810-9 | +dm_net_wifi_saved_ssid |

## 验证命令与产物

```bash
# -11
bash /data/vela/git_snapshot.sh -r ok-20260809-25 -y
# -12 / -13 编译打包
cd /data/dm && ./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh -j$(nproc)
cd /data/dm/vendor/allwinnertech/lichee && source envsetup.sh && lunch_nuttx 2 && pack
md5sum .../image/nsh.fex /data/dm/nuttx/vela.bin   # -12: a3898a8a / -13: b500f5ef
# 固化
bash /data/vela/git_snapshot.sh                     # -11 / -12 / -13
```

产物：ok-20260810-13（镜像 md5 a035fed0，`lichee/out/r528s3/gemini-s1_nand/rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img`）

## 遗留事项

- **WiFi 扫描空 / 0x27**：若扫描仍失败提示「扫描失败」，为驱动层 `download_fw FAIL status=0x27`（闭源 `librtl8733bs.a`），需上板硬件排查 GPIO2 复位波形、SDIO 供电时序；UI 层已做到正确提示与重试
- **蓝牙 H4 open 110**：仍驱动层（bt_slip_open sync 握手超时），待上板排查
- 上板回归：子页能进、无重叠、扫描三态、连接链路、蓝牙配对

*DevLog by AtomCode (deepseek-v4-flash)*
