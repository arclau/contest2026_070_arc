# DevLog 2026-08-12 — AI 语音子 APP：UI 重构 + 语音链路全打通（P64~P69）

## 背景

用户对 AI 子页提出系列需求：①Hero 按钮/图标过大占空间，不符合 deskmate-ui 设计逻辑；
②去掉录音 DEMO 与文字输入窗口；③语音聊天"不流畅/点按无效果/没提示"；④语音链路（点按录音 →
ASR 转文字 → ai_agent → TTS 回复）要真正可用。全程按「现象→多假设→最小验证→结论」推进，
历经 P64~P69 六节点，语音链路从"全灭"打到"差最后一段 TTS 上板验证"。

**总览**：

| 节点 | 主题 | 固化 |
|------|------|------|
| P64 | AI 子页 UI 重构（Hero→圆圈+aibg、移除录音DEMO/输入/回复区、卡片缩小竖版） | ok-20260812-1/2 |
| P65 | 语音链路修复三连（DNS IP 兜底 + 未联网提示 + 6 卡点击效果） | ok-20260812-3~5 |
| P66 | 联网判断修复（wifi_is_connected_to_ap 恒 0 → 改 wlan0 IP） | ok-20260812-6 |
| P67 | 语音全流程审查 + 3 修复（B1 退页 stop / B4 忙等防丢 / B7 栈 32K） | ok-20260812-7 |
| P68 | 点按延录上板生效 + WS 400 修复（Resource-Id + 坑3变体） | ok-20260812-9 |
| P69 | ASR 流式链路突破 + start_silence_time I32 修复 + 手册交付 | ok-20260812-10 |

---

## 一、P64 AI 子页 UI 重构（deskmate-ui skill）

**现象**：AI 子页大标题 + Hero 卡（72px 环 + 72px 按钮、312px 高）图标过大占空间；
7 张快捷卡 + 回复区 + 输入行超出屏幕；用户要求 Hero 改圆圈、aibg 横幅置顶、卡片缩小竖版。

**方案**（按 deskmate-ui skill 设计逻辑）：
- 删除大标题 + Hero 卡 → **aibg.png 品牌横幅**（560×154 原生嵌入 337KB，`convert_aibg.py`
  生成 aibg.c/h，顶部居中）+ **语音对讲圆圈**（DM(46)=110px 触摸目标，按住 Listening/
  松开 Speaking）+ 状态小字，水平居中
- 移除 Record（MIC DEMO）卡 / textarea+发送钮+虚拟键盘（ai_send_cb/ai_ta_*/ai_kb_* 全删）/
  回复区（语音优先，回复走 TTS 播报）
- 6 快捷卡缩小竖版：DM(100)→DM(64)、badge DM(36)→DM(32)、标签 FONT_BODY→FONT_LABEL

**二次调优（用户反馈）**：
- **水平居中 bug**：`lv_obj_set_flex_align(parent, CENTER, START, CENTER)` 主轴垂直居中 +
  交叉轴靠左 → 改 `(START, CENTER, CENTER)`
- **圆圈玻璃蓝紫风格**：白玻璃→紫渐变（bg_opa 60%）+ 半透明白边 3px + 蓝紫外圈阴影
  `0x6C5CE7`（width 16/spread 4）+ **蓝色麦克风符号**（COL_BLUE）
- **按下动态**：`transform_scale 1.12x` + 阴影加深（16→26）+ 玻璃提亮（60→80），
  替换原按红 0xFF3B30（LV_STATE_PRESSED 自动回退）

**打包坑（新增）**：aibg 840×231(1.5x) 嵌入 776KB 使 nsh.fex **超分区**
（16650>16384 扇区，pack 报 "dl file nsh.fex size too large"）→ 原生 560×154 嵌入
（337KB，余量 ~295KB）。

**改动文件**：`vendor/allwinnertech/apps/luncher_dm/`：deskmate_ui.c（AI 子页重构/
回调删除）、aibg.c/aibg.h（新，convert_aibg.py 生成）、deskmate_ui.h（无）、Makefile（+aibg.c）
**验证**：编译 ✅ · 打包 ✅ · md5 一致 · 固化 ok-20260812-1/2。

---

## 二、P65 语音链路修复三连 + P67 全流程审查（DNS/提示/点击/三 BUG）

**现象**：用户点语音按钮无 LOG/无效果；语音聊天全灭。

**① DNS IP 兜底（根因-0x52）**：
- 现象：`net_connect openspeech.bytedance.com:443: -0x0052`，每次 ASR 连接失败；
  `recording thread exit: 0 chunks` + `no PCM data for batch ASR`（-ENODATA）
- 根因：`-0x52 = MBEDTLS_ERR_NET_UNKNOWN_HOST`（net_sockets.h:66 确认）= getaddrinfo
  失败——openspeech.bytedance.com 带 **3 级 CNAME 链**（bytedns1→cdngslb→queniuiq），
  板端 DNS 解析不过（对比 LLM 的 ark.cn-beijing.volces.com 可直解）
- 方案：volc_asr.c/volc_tts_ws.c `asr_net_connect_fallback`——域名解析失败回退直连
  已知 CDN IP（183.240.127.14 等 6 个，PC `getent hosts` 取得），SNI 保留
- 附带：recording_thread 启动竞态（readi 先于 capture prepare）→ 0 字节期间短重试 ≤1s；
  "streaming ASR unavailable" 无条件打印误导 → 仅失败时打印

**② 未联网文字提示（用户反馈"没联网要有提示"）**：
- `ai_voice_press_cb` 检查 `dm_net_wifi_connected()`，未联网 → 状态区 "No WiFi -
  connect first" 并拦截录音；release 加 `ai_voice_active` 标志防误发 stop；
  进 AI 子页初始状态文字按联网态显示

**③ 6 卡点击效果（用户反馈"点击不了"）**：6 张快捷卡加 `LV_OBJ_FLAG_CLICKABLE` +
  按下变灰 `0xE5E5EA` + RAD_CARD（抄 Settings 行 L660-664 模式）+ `ai_action_cb`
  点击状态区显示动作引导（"Say: weather today" 等）

**P67 全流程审查 + 3 修复**（按 §八 审计六查）：
- **B1 卡死**：PTT 录音中退出 AI 子页 close_subpage 不补发 stop → agent 无超时一直录
  占 capture → 后续 EBUSY（P63 坑）→ close_subpage 补发 `dm_ai_voice("stop")`+清标志
- **B4 丢帧**：dm_ai_voice busy 时直接返回 → 快速点按 release 的 stop 丢失 →
  busy 忙等 ≤500ms 再发
- **B7 栈溢出**：AGENT_VOICE_STACK 16K→32K（recording_thread 内 mbedtls handshake，
  P60 教训 12KB 崩）
- 审查确认安全：speak/start 锁序无死锁（不同时持两锁）、TTS 收流有超时（10s+1.5s）、
  playback_stop=drop、dm_ai 共享串无锁竞态风险低

**改动文件**：ai_agent（volc_asr.c/volc_tts_ws.c/voice_channel.c/agent_config.h）+
  luncher_dm（deskmate_ui.c/dm_ai.c/dm_net.c/h）
**验证**：编译 ✅ · 打包 ✅ · 固化 ok-20260812-3~5/7（ai_agent 仓独立 commit 125069b/04301bf）。

---

## 三、P66 联网判断修复（wifi_is_connected_to_ap 恒 0）

**现象**：WiFi 已连（agent 日志 `connected, ssid=wifi-home` / `Network connected: 10.*.*.*`），
但点语音按钮**无任何 LOG**（被拦截）。

**根因**：UI 拦截判断 `dm_net_wifi_connected()` = `wifi_is_connected_to_ap()`，在开机
自动连接（start_wifi.sh → wapi/wext ioctl 直连驱动）路径下**恒 0**——驱动内部关联
标志未置位（与 P46/P47 `_wifi_is_on` 同款坑，P47 回退后复现）。

**方案**：`dm_net_wifi_connected()` 改判 **wlan0 是否已获 IPv4**
（`netlib_get_ipv4addr(WLAN0_NAME)` ≠ INADDR_ANY，与 ai_agent netmgr 同源判断）。
流程=开机未联网提示 No WiFi → 联网后自动放行。

**改动文件**：dm_net.c/dm_net.h（新增 dm_net_wifi_connected）
**验证**：编译 ✅ · 固化 ok-20260812-6。

---

## 四、P68 点按延录上板生效 + WS 400 修复（Resource-Id + 坑3变体）

**现象**：用户上板——点按语音圆圈后自动延录 4s 生效（`chunk#1~50, peak=870→4860`
真实语音、`recorded 128000B`、batch ASR 拿到 4s 音频），**0 字节问题根治**；
但 `WS upgrade failed: HTTP 400` 依旧。

**根因链**：
1. **点按延录**（P68 UI 侧）：按住 <1.5s 视为点按 → 不立即 stop，`ai_auto_stop_cb`
   一次性 timer 延录 4s 后自动 stop（capture period 64ms 起步，几十 ms 点按读不到数据）；
   再按一次取消延录保持录音；close_subpage 同步清理 timer + 补发 stop（B1 兼容）。
   注：该 UI 改动在上一固件即生效。
2. **WS 400**：`X-Api-Resource-Id` 值错——`volc.seedasr.sauc.duration` 被拒，
   官方文档示例（web 对照 doubao-asr2-openai-proxy 实战文档）为
   **`volc.bigasr.sauc.duration`** → agent_config.h 修改。
3. **坑3变体实锤**：改 `agent_config.h` 后**增量编译不重编 volc_asr.o**——
   `strings vela.bin | grep volc` 仍是旧值 `volc.seedasr`！→ 删 `src/voice/*.o`
   强制重编 → strings 验证 `volc.bigasr.sauc.duration` 进固件。

**改动文件**：deskmate_ui.c（点按延录：g_ai_auto_stop_timer/ai_voice_press_ts/
  ai_auto_stop_cb/release 延录分支/close_subpage 清理）+ ai_agent agent_config.h（Resource-Id）
**验证**：编译 ✅ · 打包 ✅ · md5 一致（72c7882d）· 固化 ok-20260812-8/9
（ai_agent 仓 f320768/ok-20260812-8，重编后固件 ok-20260812-9）。

---

## 五、P69 ASR 流式链路突破 + start_silence_time I32 修复 + 手册交付

**现象**：上板重测——**WS 400 修复生效**：`WebSocket upgrade OK` +
`full_client_request: reqid=...` + `stream: session opened` + `ASR pre-connected` +
`chunk#1~52, 166400 read/sent` 音频全部送出；新错误：
```
[volc_asr] Server error 55000000: {"error":"kiteX processing (seq=1) error: ...
field[start_silence_time] error need I32 type, but got: STRING"}
[volc_asr] stream: recv error: -5 → [voice] stream ASR failed: -5
```

**根因**：`send_full_client_request` 里 `start_silence_time`/`vad_silence_time`
用 `cJSON_AddStringToObject("3000")` 发成**字符串**，服务器要求 **I32 整数**。

**方案**：改 `cJSON_AddNumberToObject(req, "start_silence_time", 3000)` +
`vad_silence_time 800`；其余字段对照豆包 bigmodel 协议逐一核对无误
（audio.format="pcm" 字符串、rate/bits/channel 数字、model_name="bigmodel"、
enable_*/vad_signal bool、sequence 数字）。

**📘 手册交付**：`/data/dm/DOUBAO_VOICE_INTEGRATION.md`（189 行 → 整合版 267 行 12 章）：
鉴权三套体系 / 四段链路错误码 / 二进制帧 msg_type 表 / full_client_request 字段
类型表（I32 红线）/ 踩坑实录 11 条 / 串口调试锚点表 / strings 验证坑3变体法；
后续语音问题先查它。

**改动文件**：ai_agent volc_asr.c（cJSON 类型）+ 文档 DOUBAO_VOICE_INTEGRATION.md
**验证**：编译 ✅（删 voice .o 强制重编）· 打包 ✅ · md5 一致（1562c13b）·
固化 ok-20260812-10（ai_agent 仓 7a0580b）。

---

## 六、文档整合（AIYOUHUA.MD → 手册，用户要求）

**需求**：把 AIYOUHUA.MD（2026-08-11 链路审计）的有效内容并入
DOUBAO_VOICE_INTEGRATION.md，旧内容/被否定内容剔除。

**并入**：详细架构图（§1）、关键设计决策表（§2）、四链路审查结论（§6）、
断点状态表 8 项更新版（§7）、P54 三大坑（§8）、上板验证清单（§11）。
**剔除**：media_recorder 方案（P56 已改 AW alsa 原生）、v2/asr 接口（已改 bigmodel）、
textarea/键盘/AI_RESP_MAX（P64 已移除文本链路）、语音三大缺口"缺凭证/缺触发/mic 未知"
（P54/P53/P63 全闭环）、set_volc_* 运行时配置（编译期已注入）、断点表旧状态。
AIYOUHUA.MD 原文件保留（手册 §12 注明为其整合更新版）。

---

## 遗留事项

1. **TTS 上板未实测**（凭证已配，volc_tts_ws 已加 IP 兜底）——`voice_test_tts <text>` 直测
2. 语音全链路最后一段：点按 → `ASR: xxx` → `speak/TTS network done` 播报（P69 修复后待上板）
3. batch ASR 大音频边界（>128KB pcm_buf 截断）；半双工（PTT/TTS 同 codec）压力测试
4. agent 侧 PTT 无超时（UI 已 B1 兜底；极端可加 120s 自动停）
5. dm_ai WS client 自动重连/心跳；API Key 明文在固件风险（生产改 config.json）
6. 蓝牙 H4 110 硬件排查挂起（用户暂缓，勿擅改驱动）

---

## 七、语音 TASK-01~07 严格顺序执行（用户方法论：修 bug AI → 执行工程任务 AI）

**背景**：用户指示"冻结区"（DNS/TLS/WS/ASR 协议/LLM 全部已验证，不再研究），按
TASK-01~07 严格执行，每完成一个输出 PASS/FAIL+日志+修改文件+修改原因+下一步，
**不允许跨任务**；128MB RAM 正确路线=R528 只做 Audio I/O+网络+UI+状态机，云端跑 AI。

| TASK | 内容 | 代码层结果 | 固化 |
|------|------|-----------|------|
| 01 voice_test_tts | TTS 合成+喇叭播放 | **FAIL→修复**：`voice_test_tts` 是进程内 CLI，P59 `</dev/null` 自启使其不可达（`command not found`）→ 新增 `ai_agent --test tts` 子命令模式 + `voice_channel_test_tts` 补播放段（原只落盘） | ok-60812-11/12 |
| 02 voice_test_asr | 文件 ASR 识别 | 就绪：`--test asr` 分支 + `read_pcm_file` **WAV 头剥离**（arecord 落盘带 44B 头污染首包） | ok-60812-13 |
| 03 voice_test_llm | LLM 问答 | 冻结区（P54 实测 1.8s）→ 只补 `--test llm` 同步入口（llm_chat） | ok-60812-14 |
| 04 voice_test_full | 端到端（capture→ASR→LLM→TTS→speaker） | `voice_channel_test_full()` + `--test full [secs]`，全复用已验证组件 | ok-60812-15 |
| 05 接 voice_channel | 状态机就绪审查 | ✅ 锁序无死锁/生命周期四问/边界防护全过（无改动） | — |
| 06 接 UI | UI 纯 START/STOP 控制器 | ✅ P68 点按延录已实现（press→start，release/延录→stop，零核心逻辑参与） | — |
| 07 稳定性测试 | 清单输出 | ✅ 连播 20 次/切界面/断网/快速连点/锁屏/音乐并发/内存/2h 长稳 | — |

**TASK 改动文件**（全部在 packages/ai_agent，未碰 UI/协议/驱动）：agent_main.c
（--test 子命令 tts/asr/llm/full 分支）、voice_channel.c（test_full 函数/WAV 头剥离/
test_tts 播放段）、voice_channel.h（test_full 声明）。

## 八、WiFi 0x27 卡死修复（AI 启动延后，用户判断）

**现象**：开机自连成功（`Network connected: 10.*.*.*`），但 Settings 手动关→开 WiFi →
`download_fw: download firmware FAIL! status=0x27` → WiFi 起不来（P45 可用/P47 失败历史）。

**根因（用户判断+佐证）**：`rcS.nsh` 里 `start_wifi.sh &` **后台启动**，`ai_agent </dev/null &`
在 `sleep 3` 后**提前启动**——AI 与 WiFi 驱动初始化并发，污染驱动状态 → 后续手动开关时 0x27。

**方案**：`start_wifi.sh` 去 `&` **前台执行**——WiFi 初始化（固件下载/连接/DHCP）先完整
完成，之后才启动 luncher_dm/ai_agent/bluetoothd，彻底消除并发窗口。

**验证**：distclean 全量重编（坑3：prebuilt 必须全量）→ `strings vela.bin` 实锤
`sh /etc/wifi/start_wifi.sh`（无 &）进固件 → 打包 md5 `db63ec23` → 固化
`ok-20260812-16`（commit 94f13513，含 rcS.nsh 改动）。

## 九、待上板实测清单（换会话后的第一步，按序执行）

```bash
# 前置：确认联网（状态栏 WiFi 蓝图标）
ai_agent --test tts 你好              # TASK-01：TTS OK + 喇叭出声
arecord -c 1 -d 4 /data/mic.wav       # 录 4s 测试音（说句话）
ai_agent --test asr /data/mic.wav     # TASK-02：应打印 ASR result = xxx
ai_agent --test llm 现在几点          # TASK-03：应打印 LLM result
ai_agent --test full 4                # TASK-04：FULL: capture/ASR/LLM/playback done
```

1. **TASK-01~04 上板实测**（命令如上，各步 PASS/FAIL 判定，日志贴回）
2. **WiFi 回归**：开机日志 WiFi 先于 ai_agent；Settings WiFi 开关 ×3 无 0x27
3. **AI 语音 UI 链路**：联网后点按语音圆圈（延录 4s）→ `WebSocket upgrade OK` →
   `ASR: xxx` → TTS 播报（**TTS 上板至今未实测，是全程最后一段**）
4. **TASK-07 稳定性**：连播 20 次/疯狂切界面/断网/快速连点/内存对比/2h 长稳
5. **遗留项**：半双工（PTT/TTS 同 codec）压力测试、agent PTT 无超时（B1 已 UI 兜底）、
   dm_ai WS 自动重连/心跳、API Key 明文风险（生产改 config.json）、蓝牙 H4 110（挂起）

---
*DevLog by AtomCode (deepseek-v4-flash)*

---

# 二、下午场：语音链路上板实测与四轮修复（P70 续，ok-20260812-17~21）

## 背景

P70 代码层完成后上板实测 TASK-01~04，暴露一串真实 bug：ASR 响应解析连续三层错误、
rcS 启动脚本用 NSH 不存在的 `sh` 命令、TTS resource 未开通、MIC 拾音电平过低。
按「现象→多假设→最小验证→结论」逐段排除，四轮修复全部固化。

**总览**：

| 轮次 | 主题 | 固化 |
|------|------|------|
| R1 | ASR 响应控制帧误解析（ping/pong/空 payload → Bad JSON -71） | ok-20260812-17（ai_agent 349fd88） |
| R2 | ASR 响应帧带 4B seq → payload 偏移错位；rcS `sh`→`.`；TTS 403 定位 | ok-20260812-18（ai_agent cbbdaf2 + vendor ca064ac6） |
| R3 | ASR 响应 header/payload 包装兼容（成功无 code 字段、result 是对象） | ok-20260812-19（ai_agent fcb7b20） |
| R4 | MIC 增益提升（MIC1 gain 19→31 + 软件 ×6→×12）治 VAD 无语音 | ok-20260812-20/21（ai_agent 6bea2bf/8d37680） |

## 一、R1：ASR 响应控制帧误解析（Bad JSON -71）

**现象**：`--test asr`/`--test full 4`/UI 语音圆圈全部 `Bad JSON in response` →
`recv error: -71`（EPROTO）。

**根因**：`recv_volc_response` 把服务器发的 WS 控制帧（ping/pong）或空 payload 帧
当 volc 二进制帧解析，负载非协议头 → `cJSON_Parse` 失败 → 直接 -71 中断会话，
不再收后续真实结果帧。

**方案**：①ping→回 pong、pong/text 帧跳过；②空 payload（payload_len==0）帧跳过；
③Bad JSON 时转储帧头 8B 便于上板定位。volc_tts_ws.c 同步同样控制帧处理。

**验证**：编译 ✅ · strings 确认新诊断串进固件 ✅ · 固化 ok-20260812-17。

## 二、R2：响应帧 seq 偏移 + rcS `sh` 命令 + TTS 403

### 2.1 ASR 响应帧带 4B seq（上板 Bad JSON 复现）

**现象**：新诊断输出 `Bad JSON in response: flen=122 volc_hdr=4 plen=1
head=1191100000000001`。

**根因（head 一锤定音）**：`00 00 00 01` 是**服务器响应帧的 4B sequence 字段**
（首个响应 seq=1），不是 payload_len！服务器响应布局 `[4B volc头][4B seq]
[4B payload_size][payload]`（与 TTS 侧 `audio_off=volc_hdr_len+8`、0xF 错误帧
`msg_off=+8` 同协议族）。原实现把 seq 当长度读到 1 → data_off 少算 4B → JSON
截断 → Bad JSON。

**方案**：payload_len 从 `volc_hdr_len+4` 读、data_off=`volc_hdr_len+8`；最终结果
判定优先用帧头有符号 resp_seq（负值=最终，同 TTS 约定）；诊断转储 12B 帧头。

### 2.2 rcS.nsh `sh` 命令不存在（开机 WiFi 自动连接失效）

**现象**：开机 `sh /etc/wifi/start_wifi.sh` → `nsh: 10: command not found`，
start_wifi.sh 首行 echo 从未回显 = 脚本根本没执行；08:01:20 的 `connected ssid=wifi-home`
是用户手动连的。

**根因**：NSH 命令表 g_cmdmap **没有 `sh` 命令**（只有 `.` 和 `source` 映射
cmd_source，全表 grep 核实无 "sh"）→ `sh` 不是合法命令。

**方案**：rcS.nsh + etctmp rcS 共 4 处 `sh /etc/...` → `. /etc/...`（`. ` 是
cmd_source）。**坑3**：prebuilt 改后必须 distclean 全量重编 + strings 验证。

### 2.3 TTS 403（配置问题，非代码 bug）

**现象**：`--test tts hello` → `TTS HTTP 403: {"code":45000030,"message":
"[resource_id=volc.service_type.10029] requested resource not granted"}`。

**结论**：`AGENT_DOUBAO_TTS_RESOURCE="volc.service_type.10029"`（agent_config.h:352）
是写死默认值，全工程仅此一处、从未被用户/文档确认 → 账号未开通该资源（同 P68
ASR Resource-Id 类型）。**待用户从火山控制台确认正确 TTS resource_id** 后改宏重编。

**验证**：R2 三项编译 ✅ · strings 确认 `. /etc/wifi/start_wifi.sh` 进固件、旧
`sh /etc/` 残留 0 ✅ · 固化 ok-20260812-18。

## 三、R3：成功响应无 code 字段 + result 是对象

**现象**：R2 后 Bad JSON 消失，新错误 `ASR error -1: unknown` + `recv error: -5`。

**根因（json dump 实锤）**：服务器成功响应是
`{"audio_info":{"duration":0},"result":{"additions":{...},"text":""}}` ——
**无 code/header 字段**（code 取不到 → -1 误报并丢弃结果）、**result 是对象**
（原实现只走 result[0].text 永远取不到）。

**方案**：①无 code 字段 = 成功，仅显式 code≠1000 才报错；②result 兼容
对象 result.text / 数组 result[0].text / payload.text 三种形态；③batch 路径
（volc_asr_recognize）首个成功响应即 break（不死等 seq<0，避免 30s 超时）；
④报错时转储 JSON 前 256B。

**验证**：编译 ✅ · 固化 ok-20260812-19（fcb7b20）/ok-20260812-20（6bea2bf）。

## 四、R4：MIC 拾音电平过低（VAD 判无语音）

**现象**：R3 后 ASR 协议全通，新错误 `No text recognized (1ms)` → -61（服务器
返回 `"text":""` + `"duration":0`）；UI 流式 chunk peak 仅 216/150/402/114
（正常语音 1200~3594）；batch 路径 `ssl_write: -0x004e`（服务器重置连接）。

**根因**：audio_capture.c 只设 `MIC1 gain volume=19`（该寄存器 mask **0x1F=0~31**，
codec 默认 `.mic1gain=0x1f` 即 31 最大）→ 轻声说话未达服务器 VAD 语音判定阈值。

**方案**：①`snd_ctl_set("MIC1 gain volume")` 19→**31**（最大 PGA 增益）；
②软件增益 `AGENT_AUDIO_CAPTURE_GAIN` 6→**12**；③VAD 参数（start_silence_time/
vad_silence_time）保持文档 §5 值不动（防切短命令设计，非问题所在）。

**验证**：编译 ✅ · 打包产物一致（8098392B）✅ · 固化 ok-20260812-21（8d37680）。

## 五、遗留事项（明天继续）

1. **上板实测**：烧 ok-20260812-21 后 `ai_agent --test full 4` 或 UI 语音圆圈
   **大声贴近 MIC 说话**，串口预期 `chunk peak ≥1000` → `ASR: xxx` → TTS 播报
2. **TTS 上板未实测**（全程最后一段）：UI 语音问答走 WS binary TTS（Bearer 凭证已注入，
   应能出声）；`--test tts` 仍 403，**等用户提供火山 TTS resource_id**
3. **蓝牙 H4 110 硬件排查**（挂起，用户暂缓，勿擅改驱动）
4. 半双工（PTT/TTS 同 codec）压力测试、agent PTT 无超时（B1 已 UI 兜底）、
   dm_ai WS 自动重连/心跳、API Key 明文风险（生产改 config.json）

---
*DevLog by AtomCode (deepseek-v4-flash) · 2026-08-12 · 语音链路上板实测四轮修复（ok-20260812-17~21）*

