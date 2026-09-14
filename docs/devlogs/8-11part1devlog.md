# DevLog 2026-08-11 — 时间对时链路闭环 → 状态栏三件套 → 回退

> part1 承接 8-10（天气链路闭环但时间对时未解决）。本 part 记录
> ok-20260811-1 ~ ok-20260811-8：**NTP 换国内源 → HTTP Date 头兜底对时 →
> hero 时钟点击对时 → -8h 偏差修复 → 对时链路闭环（P40~P44）→ WiFi 0x27
> 上板可用/蓝牙暂缓（P45）→ 状态栏三件套上板出问题回退（P46~P47）**。
> 全流程「本地先验证再上板」：NTP 源连通性用 PC 脚本实测，Date 头解析
> 逻辑在 PC 上验证，减少刷机循环。

## 背景

- 8-10 遗留：联网后时间不对时（天气已 OK，28C/79% 惠州 5 天）。
- 8-11 上午用户确认：**天气和时间已自动更新**（对时链路闭环），WiFi 已可用。
- 下午状态栏改进（中文年月日/实时前缀/WiFi 图标）上板后用户反馈**时间消失
  + WiFi 0x27** → 用户指示回退到时间和天气都 OK 的版本。

## 节点 1：NTP 换国内源（固化 ok-20260811-1，P40）

- **现象**：自动对时代码早已实现（SYSTEM_NTPC=y + rcS ntpcstart + DHCP 后
  ntpcstop/ntpcstart 强制重启 + 去 ping DNS 前置）但时间仍不对时。
- **本地实测（PC 脚本 `/tmp/ntp_test.py`，零刷机）**：6 个 NTP 源
  （0/1/2.pool.ntp.org + ntp.aliyun.com + ntp.tencent.com + cn.pool.ntp.org）
  UDP 123 **全部可达**、DNS 解析正常 → **"国内 UDP123 被墙"假设不成立**，
  换源非根因（但保留：延迟更低更稳）。
- **方案**：defconfig 显式加 `CONFIG_NETUTILS_NTPCLIENT_SERVER=
  "ntp.aliyun.com;ntp.tencent.com;cn.pool.ntp.org"`（覆盖 Kconfig 默认
  0/1/2.pool.ntp.org）。
- **验证**：编译/打包/产物一致 7531968B + strings 验证已进固件。

## 节点 2：HTTP Date 头兜底对时（固化 ok-20260811-2，P41）

- **现象**：NTP 链路虽然公网可达，但板上 UDP 123 / 路由器放行未知；而
  **天气 HTTP 链路已验证可用**（TCP 80/443 通）。
- **本地验证**：open-meteo 响应头 `Date: Tue, 11 Aug 2026 01:05:02 GMT`
  与本机 UTC 时间一致 → Date 头可作为对时来源。
- **方案**（dm_weather.c）：
  1. 新增 `parse_http_date()`：解析 RFC 7231 IMF-fixdate `Date:` 头
     → time_t（月份表 + 年月日时分秒边界校验）；
  2. `http_get()` 加 `time_t *server_time` 输出参数，**跳头前**解析
     （memmove 会毁掉头，必须在跳头前）；
  3. `dm_weather_fetch()` 成功后 `settimeofday(UTC)`，串口打
     `[time] sync via HTTP Date OK`。
- **验证**：strings 确认进固件。与 NTP 国内源双保险。

## 节点 3：hero 时钟点击对时（固化 ok-20260811-3，P42）

- **需求**：用户要求 hero 时钟可点击 + 按压效果 + 点击手动对时。
- **方案**（ui/ui_home.c）：
  - `clock_click_cb()`：CLICKABLE + LV_EVENT_CLICKED → 复用
    `weather_kick_fetch()`（busy 防重入）→ fetch 成功即 HTTP Date 对时；
  - 按压效果：transform width/height +3（抄天气卡观感；容器透明背景
    shadow 无效故只做放大）；
  - 天气卡 debug 行显示 "Syncing..."；前向声明 weather_kick_fetch。
- **验证**：编译/打包/产物一致 + strings 验证。

## 节点 4：-8h 偏差修复（固化 ok-20260811-4，P43）

- **现象**：上板后时间**会跳但慢 8 小时**（用户反馈）。
- **根因**：板上 `rc.sysinit` 早已 `set TZ Asia/Shanghai`
  （CONFIG_LIBC_LOCALTIME=y），`parse_http_date()` 用 `mktime()` 把
  **GMT 字段按东八区本地**解释 → time_t 少 8 小时。
- **方案**：`mktime()` → `timegm()`（按 UTC 解释；`nuttx/include/time.h`
  已声明，可用）。
- **验证**：编译/打包/产物一致 7531968B + strings 验证。

## 节点 5：对时链路闭环 + WiFi 可用 + 蓝牙暂缓（P44~P45，用户确认）

- 用户 8-11 确认：**天气和时间已自动更新**（P44 对时闭环，勿再查）；
- **WiFi 0x27 上板可用**（P45）；**蓝牙 H4 110 仍不可用，用户决定先不搞**
  （软件侧已穷尽，剩硬件排查：示波器 PG6/PG7 / 芯片 BT 域使能 / 供电，
  全景见 `8-10part2devlog.md`，随时可续）。

## 节点 6：状态栏三件套（固化 ok-20260811-6，P46）—— ⚠️ 上板后回退

- **需求**：①左上时间右侧加中文年月日小字 ②中间温湿度加「实时」前缀
  ③WiFi 图标开启时不显示问题。
- **方案**：
  1. 中文年月日：`create_status_bar_ex()` 加 `date_out` 参数，左区改
     row 容器（时间 FONT_TITLE + 日期 FONT_BODY 小字 COL_SEC），Home/
     子页/锁屏三处调用点同步；`clock_update_cb` 每秒刷新
     status_date_lbl/subpage_date_lbl/standby_bar_date_lbl；
  2. 「实时」前缀：`luncher_dm.c update_sensor_cb`（每秒刷新处，改初始
     文本会被覆盖）+ create_status_bar_ex 初始文本同步；
  3. **WiFi 图标根因**：`wifi_is_up()` = `rltk_wlan_running && _wifi_is_on`，
     而 `_wifi_is_on` **仅 `wifi_on()` C API 置位**；开机自动连接走
     start_wifi.sh → wapi（wext ioctl 直连驱动）不经过 `wifi_on()`
     → 恒 0 → 图标永不显示 → `dm_net_wifi_is_up()` 改
     `wifi_is_up() || wifi_is_connected_to_ap()`。
- **上板结果（P47 回退）**：用户反馈**状态栏左上角时间消失 + WiFi 出问题
  （日志 download_fw FAIL status=0x27）** → 用户指示回退。

## 节点 7：回退状态栏三件套（固化 ok-20260811-7/8，P47）

- **动作**：5 文件 `git checkout ok-20260811-4 --` 恢复：
  `deskmate_ui.c` / `deskmate_ui.h` / `dm_net.c` / `luncher_dm.c` /
  `ui/ui_home.c`（+54/-12 全部还原）。
- **验证**：编译/打包/产物一致 **7531968B（与 ok-20260811-4 相同）**。
- **回退后状态**：时间/天气/对时均正常（P44 闭环版）。
- 🔴 **状态栏改进需求保留**：下次重做前先查两个根因——
  ①时间消失（left 容器/flex 布局或字体问题，上板 FT_Load_Glyph error 0x14）
  ②WiFi 0x27（download_fw FAIL 固件下载失败，AGENTS.md 早记录的硬件问题，
  与 UI 改动无因果关系，但当时状态栏每秒查询增加副作用嫌疑）。

## 改动文件表（本 part 全集，按节点）

| 文件 | 改动 |
|------|------|
| `vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh/defconfig` | NTP 服务器换国内源（P40） |
| `vendor/allwinnertech/apps/luncher_dm/dm_weather.c` | parse_http_date + http_get server_time + settimeofday + timegm（P41/P43） |
| `vendor/allwinnertech/apps/luncher_dm/ui/ui_home.c` | hero 时钟点击对时（P42）；状态栏三件套（P46，已回退） |
| `vendor/allwinnertech/apps/luncher_dm/deskmate_ui.c/.h` | date_out 参数 + 3 个日期全局指针（P46，已回退） |
| `vendor/allwinnertech/apps/luncher_dm/luncher_dm.c` | 「实时」前缀（P46，已回退） |
| `vendor/allwinnertech/apps/luncher_dm/dm_net.c` | wifi_is_up || connected（P46，已回退） |
| `devlog.md` / `AGENTS.md` | P40~P47 节点 + 当前焦点同步 |

## 验证命令与产物

```bash
cd /data/dm && ./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh -j$(nproc)
cd /data/dm/vendor/allwinnertech/lichee && source envsetup.sh && lunch_nuttx 2 && pack
ls -la .../image/nsh.fex /data/dm/nuttx/vela.bin   # 产物一致（-4/-7 均为 7531968B）
strings /data/dm/nuttx/vela.bin | grep -E "ntp.aliyun|sync via HTTP Date|Clock clicked"
python3 /tmp/ntp_test.py                          # 6 NTP 源 UDP123 连通性实测（P40）
bash /data/vela/git_snapshot.sh                   # ok-20260811-1~8
# 回退：git -C vendor/allwinnertech checkout ok-20260811-4 -- apps/luncher_dm/<5 文件>
```

## 遗留事项

- 🔴 **状态栏改进需求保留**（中文年月日/实时前缀/WiFi 图标）——重做前先查：
  ①时间消失根因（left 容器/flex/字体，上板 FT_Load_Glyph error 0x14）
  ②WiFi 0x27（download_fw FAIL 固件下载，硬件问题，勿擅改驱动）。
- 🔴 蓝牙 H4 110 硬件排查挂起（用户暂缓，`8-10part2devlog.md` 全景）。
- 待回归：蓝牙各子页功能（-13 重设计）、Files/Books/Games UI（-21~-25 未动）。

*DevLog by AtomCode (deepseek-v4-flash)*
