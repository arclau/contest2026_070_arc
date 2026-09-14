# DevLog 2026-08-16 — 弹窗排队/提示音语音优先 + shtc3 并发锁 + WiFi 连上即拉天气

> **会话交接**：本文件是 8-16 第二篇（part2）。承接 part1（P101~P110，代码 ok-20260816-22）。本 part 记录：用户上板日志暴露的两个健康提醒 bug（弹窗互覆盖倒计时冻结、音乐占声卡提示音静默跳过）→ 弹窗排队 + 提示音语音优先；顺带全链路审查（无新增严重 bug）+ shtc3 并发 measure -1 根治；开机初始化提速（WiFi 连上即拉天气/对时 + 天气 4h 周期）。结束代码 **ok-20260816-26**（3 个固化点）。

## 一、喝水/久坐弹窗互覆盖 → 倒计时冻结（P111，ok-20260816-24）

### 现象（用户上板日志）
```
17:57:49 health_tick_cb: [health] water popup L1
17:57:49 health_popup_show: [health] popup kind=0 level=1: 该喝水啦
17:57:49 health_tick_cb: [health] sit popup L1
17:57:49 health_popup_show: [health] popup kind=1 level=1: 起来活动一下吧
[tone] pcm busy -16 (music playing? skip)
```
用户反馈：①不会语音播报叫我起来/喝水；②喝水的倒数卡住了。

### 根因（两条独立问题）
1. **弹窗互覆盖**：喝水/久坐 L1 同为 1800s（天热喝水缩至 1800s 与久坐同秒归零），同一 tick 两个 POP 事件 12ms 内连发。`health_popup_show` 复用**单一弹窗容器** + `health_popup_kind` 全局，第二个弹窗把第一个**覆盖改写**（kind=0→1）→ 喝水弹窗按钮永远点不到 → `g_water_remain_s=0` 永远等按钮 → 喝水倒计时冻结（设计上 remain=0 停住等按钮，被覆盖则永不解锁）。
2. **提示音静默跳过**：`dm_tone_play` worker 打开 `hw:audiocodec` 返回 -16（EBUSY，音乐 XPlayer 占用）时**按 P103 设计直接跳过**（"声卡被音乐占用时不打断音乐"）→ 弹窗出了但完全无声；且 P107 实证 XPlayer **暂停也不释放句柄**（pause 只调 snd_vela_pcm_pause 不关 handle）。

### 方案
- **ui_home.c 弹窗排队**（`health_popup_show` / `health_popup_flush_pending` / `g_popup_pending_kind/level`）：
  - 已有弹窗显示时，后到提醒**挂起不覆盖**；当前弹窗关闭（DISMISS 事件）后补显
  - 补显守卫 `level>0 && remain==0`（"正等按钮"状态）——sit_stood 同时重置喝水/离开清零后挂起自动作废，防陈旧弹窗误显
  - 左右键不再显式 hide（改由 DM_HEALTH_EVT_DISMISS 统一 hide+flush）；离开（PRESENCE 0）清挂起
  - 文案抽 `health_popup_text()` 供弹窗与补显共用（cfg 可配），消除两处维护
- **deskmate_ui.c 提示音语音优先**（`dm_tone_play`）：
  - XPlayer 持有声卡（`g_xplayer` 非空，播放/暂停均不释放）→ 先 `music_audio_force_stop()` **停音乐释放声卡再播提示音**（对齐 PTT 语音优先，P107 同款行为）
  - tone worker 加 EBUSY 重试（≤5×200ms，兜底 PTT/TTS 释放中的窗口）
  - 线程安全核对：dm_tone_play 全部调用路径（弹窗/欢迎语/对时补播 `health_greet_maybe_catchup`←`weather_update_cb` lv_timer）均在 LVGL 主线程，动音乐状态安全

### 验证
编译→打包→资源检查→固化 **ok-20260816-24**。全链路审查（ok-20260816-22..HEAD）结论：无新增严重 bug（线程安全/排队守卫/cfg 字符串生命周期/EBUSY 重试 handle 管理逐项核对通过）。

## 二、shtc3 并发 measure -1 根治（P112，ok-20260816-25）

### 现象
日志每 2~4 分钟一条 `Failed to send measure command: -1` + `Attempting soft reset to recover...`（soft reset 自愈，功能不受影响但刷屏）。初判误为 prox（LTR553），实为 **shtc3（温湿度）**——正是 AGENTS.md 遗留项 "shtc3 HPWORK"。

### 根因（代码实锤）
- SHTC3 与 LTR553 **同挂 i2c_bus2**（r528_boot.c:808/813）
- shtc3 的 **temp_worker 和 humi_worker 两个 HPWORK worker 各自独立调度、都调 `shtc3_measure(priv)`**（各自 work_queue 重新排队），luncher_dm 传感器轮询（`shtc3_fetch`，LVGL 线程）是**第三个并发调用方**
- `shtc3_measure`（wakeup→发命令→等15ms→读6字节→CRC）**全程无互斥锁**，`sleeping` 标志非原子 → 两 worker 交错时同一 I2C 总线 I2C_TRANSFER 返回 -1 → soft reset 自愈
- 历史佐证：8-7part3 已记 "shtc3 HPWORK 254 阻塞"；P99 pimmux 修复（关 UART1）后仍偶发 → **并发无锁才是主因**，pimmux 只是部分根因

### 方案（shtc3.c，30 行）
- 结构体加 `mutex_t dev_lock`（对齐 ltr553 惯例）
- `shtc3_measure` 全程 `nxmutex_lock/unlock`（goto out 统一解锁，5 个返回点收敛）
- `shtc3_register` `nxmutex_init` + 4 处错误路径 `nxmutex_destroy`

### 验证
编译→打包→资源检查→固化 **ok-20260816-25**。上板期望：串口不再出现 `Failed to send measure command: -1`。

## 三、开机初始化提速：WiFi 连上即拉天气/对时 + 天气 4h 周期（P113，ok-20260816-26）

### 背景（用户拍板方向）
时间和天气**必须等 WiFi 启动好后才能同步**——所以同步动作应**跟在 WiFi 后面**（连上即拉），而不是等定时器轮询；天气 10min 同步一次过频，4h 一次即可。WiFi 本身加快（P113b，见遗留）可让整体更快。

### 方案（ui_home.c，18 行）
1. **WiFi 连上边沿 → 立即踢天气 fetch**：`clock_update_cb`（每秒轮询点）加 `dm_net_wifi_connected()`（wlan0 拿到 IPv4，P66 判据）**0→1 边沿检测**（`g_wifi_prev_conn`），连上瞬间 `weather_kick_fetch()`——fetch 内部自带 HTTP Date 对时（P41），**时间+天气一把梭**，不再等 30s 轮询 timer（原最坏晚 30s）
   - `g_wifi_prev_conn` 初值 -1（开机首次观察不踢，初始 kick 在 ui_home_create）；重连（1→0→1）也会再踢
   - `weather_kick_fetch` 有 `g_weather_busy` 防重入，安全
2. **天气成功周期 10min→4h**：`lv_timer_set_period(timer, 600000→14400000)`，成功分支+预取注释同步；开机失败重试仍 30s 不受影响

### 验证
编译→打包→资源检查→固化 **ok-20260816-26**。上板期望：WiFi 连上立即出现 `weather fetch: OK`（原可能晚 30s）+ 欢迎语按 P106 尽快补播。

## 改动文件汇总

| 仓 | 文件 | 改动 |
|----|------|------|
| vendor | `apps/luncher_dm/ui/ui_home.c` | P111 弹窗排队/flush/文案统一；P113 WiFi 连上边沿踢天气 + 4h 周期 |
| vendor | `apps/luncher_dm/deskmate_ui.c` | P111 提示音语音优先（force_stop 停音乐）+ EBUSY 重试 |
| vendor | `chips/r528/.../sensor/temperature/shtc3.c` | P112 并发锁（dev_lock + measure 加锁 + register init/destroy） |

## 验证命令与产物

- 编译：`./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh -j$(nproc)`
- 打包：`lichee && source envsetup.sh && lunch_nuttx 2 && pack`
- 资源检查：P92 脚本（res.fex 含 MiSans/FW_NIC_ACUT/12 tones + 整包镜像含完整 res.fex 块），P111/P112/P113 三次均通过
- 固化：`bash /data/vela/git_snapshot.sh` → ok-20260816-24/25/26

## 遗留事项

- **P113b 候选：start_wifi.sh 13s 固定 sleep 优化**（用户已表态"WiFi 能加快整体就能加快"）——`wapi scan` 内部已自带轮询（200ms×25≤5s）、`dhcpc_request` 内部已自带重试（CONFIG_NETUTILS_DHCPC_RETRIES），脚本内 7 个固定 sleep（1+2+2+2+2+2+2）是盲等：scan 后 sleep2 冗余、renew×4+8s 是盲试 → 改事件驱动轮询（reconnect 后轮询 `wapi show wlan0` 关联态 + 单次 renew + `ifconfig` 验 IP）13s→3~6s。约束：start_wifi.sh 保持前台、ai_agent 顺序不变（P70 0x27）；prebuilt 改后需 distclean 全量重编 + strings 验证
- 音乐被提示音 force_stop 后不自动恢复（需 ai_agent→luncher_dm 跨进程 WS 通知，P84 voice_evt 可扩展）
- AI 回复延迟（LLM 非流式，需用户拍板流式化）
- 上板复测（烧 ok-20260816-26 整包镜像，勾 res 分区）：P111 喝水/久坐同 tick 弹窗依次出现、音乐中提醒停音乐播报；P112 无 measure -1；P113 WiFi 连上即出天气/对时
- 光感 ALS 已采集未订阅（自动亮度/午休检测）；蓝牙 H4 110 挂起（勿擅改驱动）；Files/Books/Games 回归

*DevLog by AtomCode (deepseek-v4-flash)*
