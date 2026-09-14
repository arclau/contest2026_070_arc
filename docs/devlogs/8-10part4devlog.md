# DevLog 2026-08-10 — 天气链路修复全景（WiFi 重连 → 天气/时间/温湿度 → 天气卡交互与 API debug）

> part4 承接 part3（-22~-26 状态栏根治 + 文件管理器图标）。本 part 记录
> ok-20260810-35 ~ ok-20260810-42：**WiFi 开机自动重连 → 天气预设惠州 +
> NTP 对时 + 状态栏温湿度分源 → 天气卡点击刷新 + API debug → 天气链路
> 三个 bug 逐个闭环（请求截断 / JSON 解析 / debug 重叠）**。
> 本次全程采用「本地可复现先本地验证」：HTTP/JSON 逻辑全部在 PC 上模拟
> 跑通后才上板，避免无谓刷机循环。

## 背景

- 用户报告：WiFi 能连上（192.168.*.* + ping 百度 0% 丢包）但**天气不更新、
  时间不对时、状态栏/天气卡温湿度数据源混乱**；随后要求天气卡可点击刷新、
  点击有按压反馈、点击后显示 API 对接 debug 信息。

## 节点 1：WiFi 开机自动重连修复（固化 ok-20260810-35）

- **现象**：上板日志满屏 `nsh: WAPI_CONF=...: command not found`、
  `nsh: SSID=: command not found`、`nsh: for: command not found`，
  WiFi 开机不自动连、DHCP 失败，只剩 netinit 静态 10.*.*.* 兜底。
- **根因**：`start_wifi.sh` 是 POSIX 语法（`VAR=value`、`$(...)`、`for`、
  `grep -oP`），但 rcS.nsh 里 `sh` 是 **NSH 内置脚本解析器**（系统无 mksh/
  busybox），逐行解析时 POSIX 结构全部报 "command not found" → SSID/PSK
  从未设置 → `wapi reconnect` 无目标失败 → DHCP 失败。
- **方案**：重写 `start_wifi.sh` 为纯 NSH 兼容语法（只用 `wapi/sleep/renew/
  echo` 普通命令，无变量/无 `$()`/无 for）：`disconnect` → `scan`（阻塞
  填充驱动扫描队列，解决 candidate==NULL）→ `reconnect`（wapi 自读
  /data/etc/wifi/wapi.conf，嵌套 {"wlan0":{...}} 格式）→ `renew`×4（DHCP
  重试）。
- **验证**：上板 WiFi 自动连上 wifi-home 并拿到 DHCP IP（192.168.*.*），
  ping 百度通。固化 ok-20260810-35。

## 节点 2：天气预设惠州 + NTP 对时 + 温湿度分源（固化 ok-20260810-36）

- **天气坐标**：上海(31.23,121.47) → **惠州(23.11,114.42)**；`http_get`
  加 5s SO_RCVTIMEO/SO_SNDTIMEO（防阻塞 UI 线程）；fetch 失败返回 -1，
  `weather_update_cb` 失败 30s 快速重试、成功 10min 周期。
- **NTP 对时根因**：`ntpc_daemon` 用 `netlib_check_ipconnectivity()`（ping
  各 DNS 服务器）判断连通性 → DNS 禁 ICMP 时恒 0 → **永不采样、联网也不
  对时**（R528 实机现象）。方案：去掉 ping 前置直接采样（网络不可用自然走
  retry，仿安卓"网络可用即对时"）。
- **温湿度分源**：天气卡（w_cur_temp_lbl/w_cur_desc_lbl）= 天气 API；
  状态栏（status_weather_label/subpage/standby）= 传感器
  （luncher_dm.c update_sensor_cb 每秒驱动，subject 存 10 倍值）。
- **验证**：固化 ok-20260810-36，待上板。

## 节点 3：start_wifi 注释英文 + 联网强制对时 + 天气后台线程（固化 ok-20260810-37）

- **现象**：上板出现 `nsh: 10: command not found`——上轮在 start_wifi.sh
  加的中文注释 `# 2026-08-10 联网后…` 被 NSH 拆出 "10" 当命令（NSH 对
  UTF-8 中文注释处理有缺陷，旧版也出过 `▒` 乱码）。
- **方案**：
  1. start_wifi.sh 注释全部改英文；
  2. DHCP 成功后 `ntpcstop` + `ntpcstart` **强制重启** NTP daemon（单纯
     `ntpcstart` 幂等返回旧 pid 不会重新采样；先停再启才能"联网即对时"）；
  3. **天气 fetch 移后台线程**（ui_home.c）：仿 dm_net_scan_worker，
     `weather_fetch_worker` pthread + `g_weather_busy/done/ok` 标志，
     UI 定时器只查标志不阻塞；timer 初始周期 600s→30s。
- **验证**：固化 ok-20260810-37。

## 节点 4：天气卡点击刷新 + API debug 信息（固化 ok-20260810-38）

- **需求**：点击整个天气卡强制刷新；显示 API 对接信息。
- **方案**：
  1. 天气卡加 `LV_OBJ_FLAG_CLICKABLE` + `LV_EVENT_CLICKED` →
     `weather_card_click_cb`（显示 "Refreshing..." + `weather_kick_fetch()`）；
  2. `dm_weather.c` 新增 `g_dbg[128]` + `dbg_set()`：http_get 各失败点记录
     `DNS fail: <host>` / `connect fail (errno)` / `send fail` / `recv empty
     (timeout?)`；`dm_weather_fetch` 加 gettimeofday 耗时，成功
     `OK <ms>: <lat>, <days> days, <t>C/<h>%`，失败 `FAIL <ms>: <reason>`；
  3. 天气卡底部新增灰色小字 `w_debug_lbl`（deskmate_ui.h/c 声明定义），
     fetch 完成后自动显示；
  4. **API 调研结论**：open-meteo 即最优（GitHub 官方仓库 open-meteo/
     open-meteo，5970★、AGPLv3、官方托管 api.open-meteo.com；免费无 key、
     非商业 1 万次/天、HTTP GET 即用），无需更换。
- **验证**：固化 ok-20260810-38。

## 节点 5：天气卡按压效果 + FAIL 串口日志（固化 ok-20260810-39）

- **现象**：用户反馈点击天气卡无视觉反馈；天气卡显示 FAIL。
- **方案**：
  1. 按压反馈（抄 ui_bt/ui_files 列表行 + music_round_btn 观感）：
     `LV_STATE_PRESSED` 下 transform +3 放大 + bg OPA_COVER + 蓝色光晕
     shadow 14/40%，松开自动恢复白玻璃；
  2. `weather_fetch_worker` 完成后 `LV_LOG_USER("weather fetch: %s",
     dm_weather_debug())`——把 FAIL 具体环节打到串口。
- **验证**：固化 ok-20260810-39。

## 节点 6：debug 双重嵌套 bug（固化 ok-20260810-40）

- **现象**：上板串口 `weather fetch: FAIL 5257ms: FAIL 5257ms: `——
  原因被吞、只剩空壳。
- **根因**：`dbg_set("FAIL %ldms: %s", ..., g_dbg)` 的 `g_dbg` **既是
  vsnprintf 输出目标又是 %s 输入源**，源/目标重叠 → 未定义行为，
  真实失败原因被覆盖。
- **方案**：先拷到局部 `reason[128]` 再格式化。
- **验证**：固化 ok-20260810-40；上板后串口打出真实原因
  `FAIL 0ms: DNS fail: api.open-meteo.com`（WiFi 未连时正常）、
  `FAIL 5276ms: recv empty (timeout?)`（WiFi 已连后）。

## 节点 7：JSON 解析取错字段（固化 ok-20260810-41）—— 本地模拟揪出

- **现象**：WiFi 已连仍收不到数据，怀疑解析层。
- **本地验证（PC 上完成，零刷机）**：
  1. curl 板端同款 URL：`HTTP 200, 761B, 1.67s` → **连通性/大小均正常**；
  2. 把 `json_int/json_int_array/json_str_array` 原样编译成本地程序喂真实
     响应 → **current 全提取为 0、daily 数组错乱**。
- **根因**：`strstr(buf, "\"temperature_2m\"")` 在全串找**第一个**匹配，
  而 open-meteo 响应里 `current_units`/`daily_units` 段（单位串
  `"temperature_2m":"°C"`）**先于** `current`/`daily` 数值段 → `atoi("°C")=0`，
  daily 的 `[` 还落到 `time` 数组上。
- **方案**：先 `strstr("\"current\":")` / `strstr("\"daily\":")` 定位段，
  **段内**解析。修复后本地验证：`cur 28/79/1` + 5 天数据与真实值全一致。
- **验证**：固化 ok-20260810-41。

## 节点 8：HTTP 请求截断（固化 ok-20260810-42）—— 天气链路闭环

- **现象**：上板 `FAIL 5276ms: recv empty (timeout?)`（connect 成功但
  服务器不回数据）。
- **本地验证（PC 上完成）**：精确计算板端请求 = **261 字节**，而
  `char req[256]` 只能装 255 → **截断 6 字节**，尾部变成
  `...Connection: clos`（缺 `e\r\n\r\n`）→ 服务器等不到完整请求头
  永不响应 → recv 5s 超时空收。本地模拟修复后：`reqlen=261` 完整发送，
  收到 922B 含 temperature_2m。
- **方案**：`req[256]` → `req[512]`；`snprintf` 返回值 `>= sizeof(req)`
  报 `req truncated` 防御；`send` 改用 `reqlen` 精确发送。
- **验证**：**上板天气卡成功显示惠州真实天气**：
  `weather fetch: OK 1142ms: 23.11, 5 days, 28C/79%` ✅ 天气链路闭环。

## 改动文件表（本 part）

| 文件 | 改动 |
|------|------|
| `vendor/allwinnertech/boards/r528/r528s3-gemini-s1/src/etc/wifi/start_wifi.sh` | NSH 兼容重写；注释英文；DHCP 后 ntpcstop+ntpcstart |
| `apps/netutils/ntpclient/ntpclient.c` | 去掉 ping DNS 前置检查，直接采样（对时根因） |
| `vendor/allwinnertech/apps/luncher_dm/dm_weather.c` | 坐标惠州；5s 超时；debug（dbg_set 各失败点 + 耗时）；段内解析（current/daily）；req[512] 防截断 |
| `vendor/allwinnertech/apps/luncher_dm/dm_weather.h` | 新增 `dm_weather_debug()` |
| `vendor/allwinnertech/apps/luncher_dm/ui/ui_home.c` | 天气后台线程（busy/done/ok）；30s/10min 周期；点击回调 + 按压效果；w_debug_lbl 显示；worker 串口日志 |
| `vendor/allwinnertech/apps/luncher_dm/deskmate_ui.h/.c` | 新增 `w_debug_lbl` 全局 |
| `vendor/allwinnertech/apps/luncher_dm/luncher_dm.c` | 状态栏温湿度改由传感器驱动（update_sensor_cb） |

## 验证命令与产物

```bash
cd /data/dm && ./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh distclean  # 改 prebuilt 后必做
cd /data/dm && ./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh -j$(nproc)
cd /data/dm/vendor/allwinnertech/lichee && source envsetup.sh && lunch_nuttx 2 && pack
ls -la .../image/nsh.fex /data/dm/nuttx/vela.bin   # 产物一致 7531968B
strings /data/dm/nuttx/vela.bin | grep -E "ntpcstop|DNS fail|latitude=23.11|Connection: close"
bash /data/vela/git_snapshot.sh                    # ok-20260810-35~42
```

本地模拟（本次新增工作流，防刷机循环）：
```bash
curl -sS 'http://api.open-meteo.com/v1/forecast?latitude=23.11&longitude=114.42&current=temperature_2m,relative_humidity_2m,weather_code&daily=weather_code,temperature_2m_max,temperature_2m_min&timezone=Asia%2FShanghai&forecast_days=5'
# 解析逻辑/请求长度均可编译本地程序 + 真实响应验证（详见各节点）
```

## 遗留事项

- 🔴 **时间对时仍未解决**（用户 8-10 晚间确认：天气 OK 但时间不对时，
  "明天搞"）。已做：去 ping 前置 + 联网 ntpcstop/ntpcstart 强制重启。
  下一步排查方向：①串口看 `Started the NTP daemon` 与采样日志是否真的
  执行；②DNS 解析 pool.ntp.org 是否成功；③NTP UDP 123 端口到公网是否
  可达（与 WiFi 0x27 同属网络环境/硬件排查方向）。
- 🔴 蓝牙 H4 110 / WiFi 0x27 待上板硬件排查（8-10part2devlog.md）。
- 天气已闭环；若日后 open-meteo 国内不可达，备选和风天气免费版（需 key）。

*DevLog by AtomCode (deepseek-v4-flash)*
