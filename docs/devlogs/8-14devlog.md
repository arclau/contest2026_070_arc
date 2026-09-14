# DevLog 2026-08-14 — 语音对话闭环：双向流式 TTS 打通 + WiFi 预配置 + Bug Review（P72）

## 背景

2026-08-14 会话核心目标：把「语音对话」最后一段——TTS 语音播报——真正打通。前序 P65~P71
已打通录音/ASR/LLM 链路（上板 ASR 出文本待验），但 TTS 一直 403（`volc.service_type.10029
not granted`）。用户 8-14 提供了新版「双向流式语音合成 WebSocket」协议文档 + 协议 zip
（`TTS Websocket Bidirection protocols.zip`，含官方 Python 实现 protocols_.py 559 行）+
API Key（`<REDACTED-KEY>`），并确认自己已开通对应资源。

会话同时完成：①WiFi（wifi-home/1*********5）预配置进固件，刷机自动连；②`--test asr` -27 文件过大
修复（PCM 缓冲 128KB→256KB）；③**关键方法论转变**——用户要求"本地测试 OK 再改源码，别反复
刷机"：TTS 协议层改为 PC 本地直连火山服务器实测（官方 protocols_.py + 用户 API Key），
100% 确认根因后才改板端代码，本地端到端 FULL PASS（8 帧 124936B PCM + SessionFinished）
才编译固化。会话末按用户要求做整条语音链路 Bug Review，零代码改动收尾。

**总览**：

| 节点 | 主题 | 固化 |
|------|------|------|
| P72 | 双向流式 TTS 打通（seed-tts-2.0） + WiFi 预配置 + asr -27 修复 + Bug Review | ok-20260814-1~5 |

**P72 固化链**：

| tag | 内容 |
|-----|------|
| ok-20260814-1 | WiFi 预配置 wifi-home/1*********5 进 usrdata.fex |
| ok-20260814-2 | AGENT_VOICE_PCM_BUF_SIZE 128KB→256KB（--test asr -27 EFBIG） |
| ok-20260814-3 | volc_tts_ws.c 重写为双向流式 V3（X-Api-Key + seed-tts-2.0） |
| ok-20260814-4 | --test tts/full 改接 speak_stream + 音色换 2.0（55000000 修复） + ASR peak 日志 |
| ok-20260814-5 | 本地实测三根因修复：req_params.text 包裹 / Error 帧解析 / 事件等待+立即 FinishSession |

---

## 一、WiFi 预配置进固件（刷机自动连 wifi-home）

**需求**：用户不想每次刷机后手动输 WiFi 名 wifi-home + 密码 1*********5，要求预配置到固件。

**机制调研**（读代码确认）：
- 开机链路：`rcS.nsh` 检查 `/data/etc/wifi/wapi.conf` 存在 → `. /etc/wifi/start_wifi.sh`
  （前台执行，P70 已改）→ `wapi reconnect wlan0` 自动读该文件 → `renew wlan0` DHCP。
- 打包来源：`/data` = usrdata 分区，由 `pack_img.sh make_usrdata_image` 从
  `board/${PACK_PROJECT_PATH}/data/UDISK`（不存在）回退 `board/common/data/UDISK` 生成。
- 板端 UI 保存格式（dm_net.c `dm_net_wifi_save_conf`）：`{"wlan0":{"mode":2,"auth":4,
  "cmode":8,"alg":3,"ssid":"...","bssid":"","psk":"..."}}`，bssid 留空按 SSID 匹配。

**改动**：`vendor/allwinnertech/lichee/board/common/data/UDISK/etc/wifi/wapi.conf`
（原模板 Rivotek-Visitor）→ `ssid:"wifi-home" / psk:"1*********5" / bssid:""`（WPA2/AES）。

**验证**：编译打包后 `strings usrdata.fex | grep wifi-home` 命中（预配置进固件 ✅）；产物
nsh.fex=vela.bin 一致。用户上板实测开机自动连 wifi-home、TLS/HTTP 全通（TTS/ASR/LLM 均通）。

---

## 二、--test asr -27 文件过大修复（PCM 缓冲 128KB→256KB）

**现象**（上板 8-14 首轮日志）：`ai_agent --test asr /data/mictest.wav` 报
`File too large or empty: 160044` → `-27 (-EFBIG)`。

**根因**：`voice_channel.c read_pcm_file()` 入口用 `AGENT_VOICE_PCM_BUF_SIZE`(128KB) 检查
**整个 WAV 文件**大小；`arecord -c 1 -d 5` 产物 = 44B WAV 头 + 160000B PCM = 160044B > 131072B
被拒（-27）。且 5s PCM 数据本身 160000B 也超 128KB。

**方案**：`agent_config.h` `AGENT_VOICE_PCM_BUF_SIZE (128*1024)` → `(256*1024)`（≈8s）。
已确认 `voice_asr_recognize` batch 路径按 3200B chunk 循环发送、对 160000B 无隐藏上限。
**坑3变体**：改头文件增量不重编 → 删 `src/voice/*.o` 强制重编。

**验证**：上板 `--test asr` 成功 `Sent 160000 bytes in 3200-byte chunks`（-27 修复 ✅）；
但服务器返回 `No text recognized (0ms)` → -61（录音无语音，见遗留）。

---

## 三、TTS 双向流式改造：403 → seed-tts-2.0（核心）

### 3.1 根因定位：旧接口 ≠ 用户开通的接口

**现象**（8-14 上板）：`--test tts` 仍 403 `volc.service_type.10029 requested resource
not granted`。用户提供新版协议文档 + zip + API Key，确认自己"开通了"。

**根因**：用户开通的是**新版「双向流式语音合成 WebSocket」接口**（seed-tts-2.0 大模型），
而原代码 volc_tts_ws.c 是**旧版 V1 接口**（`/api/v1/tts/ws_binary` + Bearer token +
8B 帧 + 单次 submit JSON）。两者不是一套：旧资源 ID 10029 从未开通 → 403。

| | 旧版（原代码） | 新版（用户开通） |
|---|---|---|
| 接口 | `/api/v1/tts/ws_binary` | `wss://openspeech.bytedance.com/api/v3/tts/bidirection` |
| 鉴权 | `Authorization: Bearer;token` | `X-Api-Key` + `X-Api-Resource-Id: seed-tts-2.0` |
| 会话 | 单次 submit JSON | StartConnection→StartSession→TaskRequest→收音频→FinishSession |

### 3.2 重写 volc_tts_ws.c（822→1016 行，V3 双向流式）

协议核对依据：用户提供的 `protocols_.py`（559 行，官方 Python 实现，含 Message 帧格式/
MsgType/EventType/请求构造）+ 官方文档（火山 6561/1329505 等）。

- **鉴权**：ws_upgrade 改 `X-Api-Key` + `X-Api-Resource-Id` 头（用户 API Key
  `<REDACTED-KEY>` 已在 `agent_secrets.h` `AGENT_SECRET_VOLC_API_KEY` 注入）。
- **帧构造 send_volc_frame**：4B 头（byte0=0x11 version|header_size、byte1=msg_type|flags、
  byte2=serialization|compression）+ event(i32 BE) [+ session_id(u32 len+utf8)，连接级事件
  不带] [+ sequence] + payload(u32 len+data)，与 protocols_.py marshal 一致。
- **事件流**：StartConnection(1)→StartSession(100, req_params: speaker/audio_params
  format=pcm sample_rate=24000)→TaskRequest(200, text)→FinishSession(102)→FinishConnection(2)。
- **响应解析 recv_bidir_resp**：unmarshal 顺序（sequence 先于 event）、FullServer 事件
  （SessionStarted/Finished/Failed）、AudioOnlyServer PCM 帧、ping→pong、超时容忍。
- **音色**：`AGENT_VOICE_DEFAULT_SPEAKER` 先保留旧值，后因 55000000 换 2.0（见 §3.3）。
- 保留 DNS IP 兜底（openspeech 解析失败回退 CDN IP）+ 10s 首块超时 + 500ms 后续。

### 3.3 上板验证暴露两个问题（ok-20260814-4 修复）

上板日志：`[volc_tts] TTS V3 request` + `55000000: resource ID is mismatched with
speaker related resource`。两个发现：

1. **--test tts 走错路径**：`voice_channel_test_tts` 调 `voice_tts_speak`（**旧 HTTP V3
   unidirectional**），根本没走到新 WS！→ 改接 `voice_tts_speak_stream`（流式回调
   `tts_collect_cb` 累积 PCM，行为与原一次性返回一致）；`--test full` 的 TTS 段
   （voice_channel.c:1551）同样问题一并改。**确认**：UI 实际链路 `voice_channel_speak`
   本就调 `voice_tts_speak_stream`，无需改。
2. **音色不匹配**：默认 `zh_male_beijingxiaoye_emo_v2_mars_bigtts` 是 **1.0 音色**
   （`*_mars_bigtts`），与 seed-tts-2.0 资源不匹配 → 换官方 **2.0 音色**
   `zh_male_sophie_uranus_bigtts`（魅力苏菲男声，`*_uranus_bigtts` 系列）。

附带：`--test asr` 加 PCM peak/非零比日志（区分静音 vs 增益，16bit 满幅 32768、
静音 wav 通常 peak<200）；修 -Wshadow/-Wformat-truncation/unused var 三个编译告警。

---

## 四、本地协议实测（关键方法论：PC 直连火山，不刷机）

**背景**：ok-20260814-4 上板后 `--test tts` 换新路径但仍失败：`[volc_tts_ws]`
WS upgrade OK → StartSession 发出 → `payload overrun: off=8 plen=1920361842 flen=93` ×2
→ `0 audio chunks delivered (err=-104)`。用户明确要求：**先本地测试 OK 再改源码，别反复刷机**。

**本地环境**：Python 3.10 + websockets 16.1（注意 13+ 参数 `extra_headers`→`additional_headers`）
+ 用户提供的官方 protocols_.py + API Key，PC 直连 `wss://openspeech.bytedance.com/
api/v3/tts/bidirection`（443 可达）。

**本地验证结论**（写 6 个测试脚本逐层逼近）：

| 脚本 | 验证点 | 结果 |
|------|--------|------|
| tts_proto_test.py | 鉴权 + 事件流 + 裸 text 格式 | ConnectionStarted/SessionStarted ✅；TaskRequest 后 TIMEOUT |
| tts_proto_test2.py | 英文/带 model/文本帧 | 仍 TIMEOUT；带 model 报 session limit |
| tts_proto_test3.py | 多参数变体 + 原始字节 dump | 全部 TIMEOUT；**发现 Error 帧布局** |
| tts_proto_test4.py | 每用例新连接 + 长等待 | 仍 TIMEOUT（连接级正常） |
| tts_proto_test5.py | FinishSession 探测 | **TaskRequest 后静默，FinishSession 才回 TTSSentenceStart(text="")** |
| tts_proto_test6.py | **req_params.text 包裹** | **7 帧 117070B 音频 ✅（FULL PASS 前半）** |
| tts_proto_test7.py | 完整官方流程 | **8 帧 124936B PCM + SessionFinished ✅ FULL PASS** |

### 4.1 根因三连（本地 100% 实锤）

1. **TaskRequest 格式错**：text 必须包在 `req_params` 里——
   `{"req_params":{"text":"..."}}`（官方 iOS/Android SDK 示例同款结构）。裸
   `{"text":"..."}` 服务器合成空文本（TTSSentenceStart text=""，无音频）——
   这正是 TaskRequest 静默 + 板端 0 chunks 的根因。**已修** send_task_request。
2. **Error 帧解析 bug（-104 直接根因）**：Error 帧(0xF)布局 =
   `[hdr][error_code 4B][payload_len 4B][payload]`，**无 event 字段**。原代码把
   error_code（实测 55000000）当 payload_len 读 → `payload overrun` → 服务器断连
   -104。**已修**：msg_type==0xF 时先读 error_code 再取 payload，并打印
   `server error code=xxx payload=...`。
3. **事件流未等待 + 未立即 FinishSession**：官方流程要求 StartConnection→等
   ConnectionStarted→StartSession→等 SessionStarted→TaskRequest；且**单次文本发完必须
   立即 FinishSession**，服务器才回音频 + SessionFinished（原实现发完 TaskRequest 干等
   超时）。**已修**：新增 `recv_wait_event()`，主流程逐步确认。

### 4.2 附带修复

- 动态 header 长度：`(buf[0]&0x0F)*4`（protocols_.py HeaderSizeBits 语义，与旧版
  recv_tts_audio 一致），替换写死 VOLC_HDR_SIZE=4。
- payload overrun 时 dump 帧头 12B hex，便于上板对照。
- 恢复超时容忍：音频中 3 次连续超时（500ms）才判 EOF，防网络抖动截断音频
  （重写时丢失过，review 发现补回）。
- ConnectionFailed/SessionFailed 显式报错（原 done=1 静默，调用方误判成功）。
- 删除无用 `session_started` 变量 + 修 3 个编译告警。

---

## 五、上板验证 + Bug Review（ok-20260814-5）

**固化**：ok-20260814-5（三仓 tag + ai_agent 仓独立 commit 9104422，含全部修复）。
产物 nsh.fex=vela.bin md5 `a66b2612c1a28cf1757df1b58b2e7a47` 一致；特征串
`req_params`/`server error code`/`event %d received`/`FinishSession` strings 验证进固件；
usrdata wifi-home 仍在。

**用户要求 Bug Review 整条语音流程**（会话末），审查范围与结论：

| 环节 | 审查结果 |
|------|----------|
| send_volc_frame 帧构造 | ✅ 与 protocols_.py marshal 一致（4B 头 + event + session_id 跳过规则 + payload 长度前缀） |
| recv_bidir_resp 响应解析 | ✅ unmarshal 顺序一致；Error 帧已单独处理；动态 header；超时容忍 |
| recv_wait_event 事件等待 | ✅ 逐步确认 + Error 即失败 |
| 主流程事件顺序 | ✅ StartConnection→SessionStarted→TaskRequest→立即 FinishSession |
| voice_channel 调用链 | ✅ speak/test_tts/test_full 三处均走 speak_stream；回调 is_last 处理正确；无旧 voice_tts_speak 实际调用 |
| 采样率链路 | ✅ 请求 24kHz → 服务器 24kHz PCM → 播放 hw_params_set_rate(24000)，三处统一，codec 支持 8k~48k |
| 旧 HTTP V3 路径 | ✅ 死代码（无调用者），注册无副作用，保留无害 |
| 内存/栈 | ✅ 帧缓冲堆分配(32KB)，无大栈数组（P60 教训）；PCM 收集 256KB 有上限保护 |

**Bug Review 结论：零代码改动**（审查发现的问题已在 §4 全部修复），固件即审查后代码。

---

## 六、改动文件表

| 文件 | 改动 |
|------|------|
| `vendor/allwinnertech/lichee/board/common/data/UDISK/etc/wifi/wapi.conf` | WiFi 预配置 wifi-home/1*********5（bssid 留空按 SSID） |
| `packages/ai_agent/include/agent_config.h` | PCM 缓冲 128KB→256KB；TTS Resource→seed-tts-2.0；默认音色→zh_male_sophie_uranus_bigtts |
| `packages/ai_agent/src/voice/volc_tts_ws.c` | 重写为双向流式 V3（822→1216 行）：X-Api-Key 鉴权、事件帧构造、recv_wait_event、Error 帧解析、动态 header、立即 FinishSession、超时容忍、帧头 dump |
| `packages/ai_agent/src/voice/voice_channel.c` | --test tts/full 改接 speak_stream（tts_collect_cb）；ASR peak 日志；修告警 |
| `packages/ai_agent/src/voice/volc_tts.c` | 未改（旧 HTTP 路径死代码，保留） |

**ai_agent 仓提交链**：0efbe35（V3 重写）→ 4e8b833（改接+音色+诊断）→ 9104422（本地实测
三根因修复）。

---

## 七、验证命令与产物

```bash
# 编译打包固化（AGENTS.md §二）
cd /data/dm && ./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh -j$(nproc)
cd /data/dm/vendor/allwinnertech/lichee && source envsetup.sh && lunch_nuttx 2 && pack
md5sum lichee/out/r528s3/gemini-s1_nand/image/nsh.fex nuttx/vela.bin   # 需一致
strings nuttx/vela.bin | grep -E "seed-tts-2.0|req_params|server error code"
bash /data/vela/git_snapshot.sh                                          # 三仓 tag ok-20260814-N
git -C packages/ai_agent add -A && git -C packages/ai_agent commit -m "..." && git -C packages/ai_agent tag ok-20260814-N

# 上板复测（顺序）
nsh> ai_agent --test tts hello
#   预期串口：event 50 received → event 150 received → TaskRequest text=5 bytes
#            → "N audio chunks delivered" → 喇叭播 "hello"

# 本地协议实测脚本（留存于 /tmp/tts_proto/）
python3 tts_proto_test6.py   # req_params.text 格式 → 音频
python3 tts_proto_test7.py   # 完整官方流程 → 8 帧 124936B + SessionFinished
```

**镜像**：`lichee/out/r528s3/gemini-s1_nand/rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img`

---

## 八、遗留事项 / 可能的问题与处理

1. **TTS 上板最终确认（唯一待办）**：烧 ok-20260814-5 后 `--test tts hello` 应出声。
   协议层已在 PC 本地 FULL PASS（8 帧 124936B PCM），上板若仍有问题，串口会打印
   `server error code=xxx payload=...` 明确错误，据此定位；若 `N audio chunks delivered`
   但无声 → 查 playback（audio_playback.c，P8 音频链路已验证）。
2. **ASR -61 No text recognized**：`--test asr` 发送成功但服务器判无语音。新增 peak 日志
   区分：`peak>2000`=说话清晰、`peak<200`=静音（测试时须大声说话）；peak 高仍空 →
   增益/服务器端问题再查。R4 增益（HAL 31 + 软件 ×12）已拉满。
3. **TTS 音色**：当前默认 `zh_male_sophie_uranus_bigtts`（2.0 男声）。若用户账号音色库
   无此音色 → StartSession 报错，需从控制台音色库取一个 `*_uranus_bigtts`/`saturn_*`
   音色 ID，改 `agent_config.h` `AGENT_VOICE_DEFAULT_SPEAKER` 一行重编。
4. **协议脚本留存**：`/tmp/tts_proto/`（test1~7 + 官方 protocols_.py + zip 解压），
   可复用于后续协议回归；建议迁移到 `project_docs/` 或 `/data/vela/` 备档。
5. **旧 HTTP V3 路径**（volc_tts.c）：死代码保留；若未来要清理，删 volc_tts_register
   注册即可（voice_tts_speak_stream 不依赖它）。
6. **AI 语音 UI 链路实测**：TTS 通后仍需上板走 UI 语音圆圈（按住大声说话 →
   ASR: xxx → TTS 播报）验证端到端；TTS resource_id 已由用户开通（seed-tts-2.0 生效）。
7. **蓝牙 H4 110 硬件排查**：继续挂起（用户暂缓，勿擅改驱动）。

---

## 九、交接用测试流程（2026-08-15 给接手人「东」）

> 目标：验证「语音对话」全链路（WiFi→LLM→ASR→TTS→UI 播报）。照着 T0~T5 顺序执行，
> 每步有命令 + 预期串口 + 判定。任何一步不过 → 看「FAQ」对照处理，或记 log 找排查。

### 9.1 交接快照（先确认环境）

| 项 | 值 |
|----|-----|
| 固化 tag | **ok-20260814-5**（三仓 + ai_agent 仓独立 tag 同名） |
| 代码版本 | ai_agent HEAD=9104422；源码 `/data/dm/packages/ai_agent/` |
| 镜像 | `/data/dm/vendor/allwinnertech/lichee/out/r528s3/gemini-s1_nand/rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img`（36MB） |
| 产物校验 | `md5sum lichee/out/r528s3/gemini-s1_nand/image/nsh.fex nuttx/vela.bin` 应相同（当前 a66b2612...） |
| TTS 凭证 | API Key `<REDACTED-KEY>`（已编译注入 agent_secrets.h）；Resource `seed-tts-2.0`；音色 `zh_male_sophie_uranus_bigtts` |
| LLM 凭证 | Ark Key `ark-REDACTED` + `deepseek-v4-flash-260425`（已注入） |
| WiFi 预配置 | wifi-home / 1*********5（wapi.conf，bssid 留空按 SSID） |

### 9.2 前置条件

- 硬件：R528 板 + BOE 屏 + GT9271 触摸 + 串口（115200）+ 喇叭（接 HP/LINE OUT）+ USB 烧录线
- 环境：PC 可访问 `openspeech.bytedance.com:443`（TTS/ASR）与 `ark.cn-beijing.volces.com:443`（LLM）
- 烧录：PhoenixSuit 或 sunxi-fel 烧镜像；烧完自动进 nsh（`nsh> ` 提示符）

### 9.3 分链测试步骤（T0→T5）

**T0 开机 + WiFi 自动连**（预期 ~30s 内）
```bash
# 串口应见（开机自动执行 start_wifi.sh 前台）
[wifi] start_wifi
[wifi] wifi startup done
nsh> wapi show wlan0        # ESSID 应显示 wifi-home
nsh> ifconfig wlan0         # 应有 inet 地址（DHCP 成功）
```
判定：wifi-home 已连 + 有 IP。若 WiFi 没自动连 → 串口 `wapi connect wlan0 wifi-home 1*********5` 手动连，
并检查 rcS.nsh 是否 `. /etc/wifi/start_wifi.sh`（P70 改前台后不能带 `&`）。

**T1 LLM 文本链路**（验证公网 + LLM 凭证）
```bash
nsh> ai_agent --test llm hello
# 预期：TLS Handshake OK → LLM response: 34 bytes → "Hello! How can I assist you today?"
```
判定：有英文回复 = LLM ✅（P54 已闭环，此步应秒过）。

**T2 ASR 录音识别**（验证 MIC + ASR 链路）
```bash
nsh> arecord -c 1 -d 5 /data/mictest.wav     # 录 5s，**对着 MIC 大声说话**
nsh> ai_agent --test asr /data/mictest.wav
# 预期：ASR: pcm=160000B peak=xxxx nonzeroratio=xx% → ASR result: <你说的文字>
```
判定标准：
- `peak>2000` = 说话清晰 ✅；`peak<200` = 录音静音（说话不够大声/离 MIC 远），重录
- 出文字 = ASR ✅；peak 高但 `No text recognized` → 见 FAQ
- 不再出现 `-27 File too large`（P72 已修 256KB 缓冲）

**T3 TTS 播报（本会话核心，唯一待验证项）**
```bash
nsh> ai_agent --test tts hello
# 预期串口：
[volc_tts_ws] event 50 received          # ConnectionStarted
[volc_tts_ws] event 150 received         # SessionStarted
[volc_tts_ws] TaskRequest text=5 bytes
[volc_tts_ws] N audio chunks delivered   # N>0 = 收到音频
TTS: playback done                        # 喇叭播 "hello"
```
判定：**喇叭出声 = TTS 闭环 ✅（语音对话最后一段）**。若不出声见 FAQ。

**T4 端到端（capture→ASR→LLM→TTS→speaker）**
```bash
nsh> ai_agent --test full 4               # 开始后大声说话 4s
# 预期：FULL: ASR = <文字> → FULL: LLM = <回复> → FULL: TTS N bytes, playing... → 出声
```
判定：全流程串起来出声 = 语音对话完整闭环 ✅。

**T5 UI 语音圆圈（最终体验）**
1. 主屏 → AI 子页（aibg 横幅 + 蓝色语音圆圈）
2. 按住圆圈**大声说话**（<1.5s 松手=自动延录 4s）
3. 预期：串口 `[ws] voice start via WS` → `PTT recording started` → `ASR: xxx` → TTS 播报回复
判定：听到 AI 语音回复 = UI 链路闭环 ✅。

### 9.4 FAQ（错误码对照）

| 现象 | 含义 | 处理 |
|------|------|------|
| `TTS HTTP 403 / requested resource not granted` | 旧资源未开通（P72 前） | 已换 seed-tts-2.0，不应出现；若出现查 `AGENT_DOUBAO_TTS_RESOURCE` |
| `55000000 resource mismatched with speaker` | 音色与资源不匹配（1.0 vs 2.0） | 默认音色已是 2.0；若账号无此音色 → 控制台换 `*_uranus_bigtts` ID 改 agent_config.h:407 重编 |
| `payload overrun ... err=-104` | Error 帧解析 bug（P72 已修） | 已修（先读 error_code）；若再出现抓 hdr hex 发我 |
| `server error code=xxx payload=...` | 服务器明确报错（新日志） | 按 payload 内容处理（如音色/参数问题） |
| `No text recognized (0ms)` → -61 | 服务器判无语音 | 测试时大声说话；peak 日志确认录音能量 |
| `N audio chunks delivered` 但无声 | 播放链路问题 | 查 audio_playback.c（P8 已验证）；`aplay /data/tts_out.pcm` 单独测播放 |
| `Cannot open /data/...` | 文件路径问题 | arecord 落盘路径确认 `/data/mictest.wav` |

### 9.5 本地协议回归脚本（可选，PC 上用）

```bash
# /tmp/tts_proto/ 留存（建议迁移 /data/vela/tts_proto/ 备档）
python3 tts_proto_test7.py   # 完整官方流程：应 8 帧音频 + SessionFinished（FULL PASS）
python3 tts_proto_test6.py   # req_params.text 格式验证 → 音频
```
改动 TTS 协议前先跑这两个确认不回归；注意 API Key 在脚本内明文，勿外传。

### 9.6 改动代码后必做（纪律）

编译 → 打包 → md5 一致 → strings 验证 → git_snapshot 固化（AGENTS.md §二），
ai_agent 仓需**单独** commit+tag（独立于三仓）。

---

*DevLog by AtomCode (deepseek-v4-flash)*


