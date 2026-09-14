# 豆包语音 ASR/TTS 对接调试手册（R528 openvela）

> **适用对象**：后续接手语音链路（AI 子页 → ai_agent → 豆包语音）的工程师/AI 会话。
> **版本**：2026-08-12 整合版——合并《AIYOUHUA.MD》（2026-08-11 链路审计）与
> 《DOUBAO_VOICE_INTEGRATION.md》（上板实测手册）；固件基线 ok-20260812-10。
> **总纲**：语音链路是**四段串联**（DNS → TLS → WS 升级 → 二进制帧协议），每段有
> 独立错误码；**逐段排除、段段验证**，不要跨段猜。

---

## 1. 系统架构与端到端链路

```
┌─ 板子（R528, 1920x1200 横屏）─────────────────────────────────────────┐
│  luncher_dm（UI 进程）                 packages/ai_agent（官方框架）    │
│  ┌────────────────────┐  WS 28789      ┌──────────────────────────┐   │
│  │ AI 子页（语音圆圈）  │ 短连接         │ agent_main（独立进程）      │   │
│  │  create_ai_subpage  │ {"type":       │ ├ ws_server (28789)      │   │
│  │  ├ aibg 横幅/状态    │  "voice",      │ ├ message_bus → agent_loop│  │
│  │  ├ 语音圆圈(点按/按住)│  "action":     │ ├ llm_proxy ─HTTPS──▶    │   │
│  │  └ 6 快捷卡(引导文案)│  "start|stop"} │ │   火山方舟(DeepSeek)     │   │
│  └────────────────────┘                │ └ voice_channel          │   │
│   dm_ai.c（RFC6455 最小 client）         │   ├ audio_capture (AW alsa)│  │
│   dm_ai_voice() 短连接 fire-and-forget  │   ├ volc_asr(流式/batch)  │   │
│   点按<1.5s 自动延录 4s 再 stop          │   ├ volc_tts(流式)        │   │
│                                         │   └ audio_playback(喇叭)  │   │
└─────────────────────────────────────────┴──────────────────────────┘
        TLS 443（openspeech.bytedance.com）
        ├ /api/v3/sauc/bigmodel   ← ASR 流式（X-Api-* 头鉴权）
        └ /api/v3/tts/unidirectional ← TTS（x-api-key 鉴权）
```

**链路① 流式 ASR（PTT 主路径）**：WS 升级 → full_client_request → 逐包 audio →
finish（负包）→ 文本 → message_bus → LLM → TTS → 喇叭。
**链路② batch ASR（流式不可用兜底）**：PTT 整段 PCM 累积 → 一次性发送识别。
**链路③ TTS**：`x-api-key` HTTP 鉴权，流式 chunk 回调写 playback。
**链路④ 文本问答**：已从 UI 移除（P64：回复区/输入窗口删除，语音优先），
`dm_ai_ask` 仅后端保留。

---

## 2. 关键设计决策（已定，勿轻易推翻）

| 决策 | 原因 | 状态 |
|------|------|------|
| 用官方 `packages/ai_agent`，不抄选手代码 | 用户明确要求 | ✅ |
| 独立 APP + WS(28789) 通道 | 官方推荐形态，UI 完全自控 | ✅ |
| LLM 用火山方舟 `deepseek-v4-flash-260425` | 国内可达 + 免费额度（P54 上板 1.8s 实测通；v3-241226 已下架 404） | ✅ |
| 语音 ASR/TTS 用豆包（大模型版 bigmodel） | ai_agent 内置，凭证 P54 已注入 | ✅ 上板中 |
| 开机自启 `ai_agent </dev/null &` | 用户需求（P59 防抢 NSH stdin） | ✅ |
| 语音交互=点按/按住圆圈 | P68 点按延录 4s（<1.5s 点按自动延录） | ✅ 上板生效 |

---

## 3. 鉴权体系（三套，别混）

| 服务 | 位置 | Header | 值来源 |
|------|------|--------|--------|
| ASR 流式 WS | HTTP Upgrade 头 | `X-Api-App-Key` / `X-Api-Access-Key` / `X-Api-Resource-Id` / `X-Api-Connect-Id` | agent_secrets.h `AGENT_SECRET_VOLC_*` |
| TTS unidirectional | HTTP 头 | `x-api-key` + `X-Api-Resource-Id` | `AGENT_SECRET_VOLC_API_KEY` |
| LLM（方舟） | HTTP 头 | `Authorization: Bearer <ark-key>` | `AGENT_SECRET_API_KEY`（与语音无关） |

**凭证存储**：`packages/ai_agent/include/agent_secrets.h`（编译期固化，开机即用，
无需 set_volc_*）；config.json 存在时优先，`set_*` 可覆盖。
**当前凭证**（用户 2026-08-11 提供）：App-Key `<REDACTED-APPID>` / Access-Key `<REDACTED-TOKEN>` /
Resource-Id `volc.bigasr.sauc.duration` / TTS x-api-key `<REDACTED-KEY>...`。
**⚠️ 改凭证/宏后必须删 `src/voice/*.o` 重编 + strings 验证**（坑3变体，§9.2）。

---

## 4. 四段连接链路（逐段错误码）

### 4.1 DNS 解析
- **错误**：`net_connect openspeech.bytedance.com:443: -0x0052` = `MBEDTLS_ERR_NET_UNKNOWN_HOST`。
- **根因（实锤）**：域名 3 级 CNAME 链（bytedns1→cdngslb→queniuiq），板端解析不过；
  对比 LLM 的 ark.cn-beijing.volces.com 可直解。
- **修复（P65）**：`asr_net_connect_fallback`/`tts_net_connect_fallback`——域名失败回退
  直连已知 CDN IP（183.240.127.14 等 6 个，PC `getent hosts` 取得），SNI 仍用真实域名。
- **锚点**：`IP fallback connected: 183.240.127.14`。

### 4.2 TLS 握手
- **错误**：`handshake: -0xXXXX`。要点：ALPN http/1.1、TLS1.2~1.3、VERIFY_OPTIONAL。
- **锚点**：`TLS connected to openspeech.bytedance.com:443`。

### 4.3 WS 升级（HTTP 101）
- **错误**：`WS upgrade failed: HTTP 400`。
- **根因（P68 实锤）**：`X-Api-Resource-Id` 值错——`volc.seedasr.sauc.duration` 被拒，
  官方示例 **`volc.bigasr.sauc.duration`**。
- **要点**：Upgrade/Connection/Sec-WebSocket-Key(base64 16B)/Version:13 + X-Api-* 四头；
  **无** Sec-WebSocket-Protocol 子协议头（官方示例不带）。
- **锚点**：`WebSocket upgrade OK`。

### 4.4 二进制帧协议
- **8 字节头**：`[ver=1][msg_type][serialization][0x00][payload_len BE32]`。
- **msg_type**：0x5 FULL_CLIENT_REQUEST(JSON) / 0x6 AUDIO / 0x7 AUDIO_LAST /
  0x9 SERVER_RESPONSE / 0xF SERVER_ERROR（error_code BE32 + msg）。serialization：JSON=1, RAW=0。

---

## 5. full_client_request JSON 字段表（⚠️ 类型敏感！）

```json
{
  "user":   { "uid": "agent" },
  "audio":  { "format": "pcm", "rate": 16000, "bits": 16, "channel": 1,
              "language": "zh-CN" },
  "request":{ "model_name": "bigmodel", "enable_itn": true, "enable_punc": true,
              "sequence": 1, "show_utterances": false, "vad_signal": true,
              "start_silence_time": 3000, "vad_silence_time": 800 }
}
```

| 字段 | 类型 | 备注 |
|------|------|------|
| `audio.format` | **string** | 大模型版必须 `"pcm"`（v2 接口才是 `"raw"`） |
| `audio.rate/bits/channel` | number | 16000/16/1 |
| `request.model_name` | string | `"bigmodel"` |
| `request.enable_*`/`vad_signal`/`show_utterances` | bool | JSON true/false |
| `request.sequence` | number | 1 |
| `request.start_silence_time`/`vad_silence_time` | **number（I32）** | 🔴 P69 踩坑：曾用 `cJSON_AddStringToObject("3000")`，服务器报 `Server error 55000000: need I32 type, got STRING`。必须 `cJSON_AddNumberToObject` |

**cJSON 口诀**：字符串值 `AddString`，数字值 `AddNumber`，开关 `AddBool`——少个引号就是一次 55000000。
**Response**：`{"result":[{"text":"...","seq":N,"definite":bool}],...}`，`seq<0` 为最终结果。

---

## 6. 链路逐段审查结论（AIYOUHUA 四链路，更新至 2026-08-12 状态）

### 链路① UI → dm_ai WS → ws_server —— ✅ 通（语音控制）
- `dm_ai.c`：RFC6455 最小 WS client（base64 key 握手 / masked 帧 / 30s 收包超时）；
  语音控制 `dm_ai_voice("start|stop")` 短连接 fire-and-forget，busy 忙等 ≤500ms 防丢帧（B4）。
- `ws_server.c`：`{"type":"voice","action":"start|stop"}` → `voice_channel_start/stop()`
  （P53 自扩，用户授权；`{"type":"message"}` 文本链路 UI 已移除）。
- 已知留白：dm_ai 无自动重连/心跳 ping（agent 重启后 UI 语音需重新操作）。

### 链路② message_bus → LLM proxy → 火山方舟 —— ✅ 通（上板实测）
- `llm_proxy.c`：`Authorization: Bearer` + mbedTLS 3.4.0 + OpenAI 兼容
  （`ark.cn-beijing.volces.com/api/v3/chat/completions`，120s 超时，cJSON 解析）。
- Key/模型编译注入（agent_secrets.h），开机即用；`ask` 1.8s 返回（P54 实测）。
- ⚠️ 风险：Key 明文在固件（可提取），生产改运行时 config.json。

### 链路③ 语音 voice_channel → capture → ASR → LLM → TTS —— ✅ 打通中
- `voice_channel_start`：rec_thread 循环 `audio_capture_read`（AW alsa `snd_vela_pcm_*`，
  P56 从 media_recorder 改板级原生路径）→ `voice_asr_stream_send` 流式 → 文本 →
  message_bus → agent_loop → `voice_channel_speak`（TTS）→ `audio_playback_write`。
- **触发链路已闭环**：UI 语音圆圈（点按/按住）→ WS voice start/stop（P53）→
  点按延录 4s（P68 上板生效，录到 128000B）。
- **协议已打通**：DNS 兜底（P65）→ WS 400 修复（P68）→ full req I32 修复（P69）。
- 半双工：PTT 前停 TTS（AEC workaround），同 codec 切换时序待压力验证。

### 链路④ 音频设备路径 —— ✅ 无冲突
| 项 | ai_agent | 板上实际 | 结论 |
|----|----------|----------|------|
| 录音 | AW alsa capture（hw:audiocodec, MIC1 使能 P57） | MICIN1→ADC1→DMA 实测有声（P63） | ✅ |
| 播放 | AW alsa playback（dm_sound 同源） | 喇叭实测有声（音乐链路） | ✅ |
| 依赖 | 不依赖 NuttX audio 框架 | `CONFIG_MEDIA=y`，走 AW 栈 | ✅ |
| 并发 | 与 libcedarx XPlayer 同 codec | 半双工注意（AEC workaround 已有） | ⚠️ |

---

## 7. 断点状态表（AIYOUHUA 断点更新版——全部闭环）

| # | 链路段 | 2026-08-11 状态 | 2026-08-12 状态 |
|---|--------|-----------------|-----------------|
| 1 | UI → WS client → ws_server | ✅ 通 | ✅ 通（语音控制 P53） |
| 2 | message_bus → LLM → 火山 | ✅ 软件层 | ✅ 上板 1.8s（P54） |
| 3 | 公网 HTTPS 443 实测 | ⚠️ 未验证 | ✅ 通（P54） |
| 4 | mic → capture → ASR | 🔴 缺凭证+未实测 | ✅ 凭证已配（P54）+ mic 实测（P63）+ 协议通（P69） |
| 5 | TTS → 播放 | 🔴 缺凭证 | ⚠️ 凭证已配，**上板未实测**（遗留） |
| 6 | UI 语音按钮 → voice_start | 🔴 无接口 | ✅ WS voice 控制（P53）+ 点按延录（P68） |
| 7 | 音频设备路径 | ✅ 无冲突 | ✅（P56/P57/P63） |
| 8 | 半双工切换时序 | ⚠️ 未验证 | ⚠️ 仍待压力验证（遗留） |

---

## 8. 踩坑实录（全量，P54~P69 上板实锤）

| # | 现象 | 根因 | 修复 | 节点 |
|---|------|------|------|------|
| 1 | LLM `ask` 404 InvalidEndpointOrModel | 模型 ID `deepseek-v3-241226` 下架 | 换 `deepseek-v4-flash-260425`（控制台 API 示例复制准确 ID） | P54 |
| 2 | 方舟 `list_models` 用不了 | 只支持 openrouter 后端 | 查模型 ID 去控制台「API 调用示例」复制 `model` 字段原文 | P54 |
| 3 | ASR 传统 v2/asr 必不通 | 用户开通的是**流式识别大模型**（Seed bigmodel） | 适配 `/api/v3/sauc/bigmodel` + X-Api-* 头 + format=pcm（与 v2 的 Bearer+raw 完全不同） | P54 |
| 4 | `arecord` 自动回放 -22 | 默认 channels=3，DAC 回放只支持 1~2 | 测 MIC 一律 `-c 1` | P63 |
| 5 | AI DEMO -16 EBUSY | voice_channel 占 capture 未释放 | B1 close_subpage 补发 stop | P63/P67 |
| 6 | 语音 0 字节 | 流式不可用线程早退 / 点按<period | 启动竞态重试 ≤1s + **点按延录 4s** | P65/P68 |
| 7 | `net_connect -0x52` | DNS 解析不过 CNAME 链 | IP 兜底直连 CDN | P65 |
| 8 | `WS upgrade HTTP 400` | Resource-Id 值错 seedasr→**bigasr** | agent_config.h 修正 + 删 .o 重编 | P68 |
| 9 | `Server error 55000000` | start_silence_time 发成 STRING | cJSON_AddNumberToObject | P69 |
| 10 | 语音按钮无 LOG | `wifi_is_connected_to_ap()` 在 wapi 自动连接下恒 0 | 改判 `netlib_get_ipv4addr("wlan0")` 有 IP | P66 |
| 11 | 栈溢出崩 | 12~16KB 栈内跑 mbedtls handshake | AGENT_VOICE_STACK 16K→32K | P60/P67 |

---

## 9. 调试方法论

### 9.1 串口锚点表（按出现顺序判断走到哪段）
```
[volc_asr] net_connect ... -0x0052        → DNS 段（看 IP fallback）
[volc_asr] TLS connected to ...:443       → TLS 段 OK
[volc_asr] WS upgrade failed: HTTP 400    → 握手段（查 Resource-Id/凭证）
[volc_asr] WebSocket upgrade OK           → 握手 OK
[volc_asr] full_client_request: reqid=... → full req 已发（查 55000000）
[voice] chunk#N: ... peak=xxx             → 录音送流中（peak 应百~千级）
[volc_asr] Server error 55000000          → full req 字段类型错
[voice] ASR: <text>                       → ✅ 识别成功
[voice] speak: "..." / TTS network done   → ✅ TTS 回复
```

### 9.2 strings 验证固件（坑3变体专用）
改**头文件**（agent_config.h / agent_secrets.h）后增量编译不重编依赖 .o：
```bash
find packages/ai_agent/src/voice -name "*.o" -delete   # 强制重编
./build.sh <config> -j$(nproc)
strings nuttx/vela.bin | grep <期望字符串>              # 验证进固件
```
实锤案例：Resource-Id 改后 strings 仍是 `volc.seedasr`，删 .o 重编后才变 `volc.bigasr`。

### 9.3 逐段排除法
1. CLI 直测（不经 UI）：`voice_test_tts` → `voice_test_asr`（agent 进程内）。
2. 再走 UI：点按语音圆圈（自动延录 4s）→ 串口锚点逐段对表。
3. 定位到段只查该段代码/凭证/协议，不跨段改。

### 9.4 网络判断（UI 侧）
`dm_net_wifi_connected()` = wlan0 是否有 IPv4（`netlib_get_ipv4addr`）——**勿用
`wifi_is_connected_to_ap()`**（wapi 自动连接路径恒 0，P46/P47/P66 教训）。

---

## 10. 遗留问题与下一步

- [ ] **TTS 上板未实测**（凭证已配，volc_tts_ws 已加 IP 兜底）——先 `voice_test_tts <text>` 直测
- [ ] batch ASR 大音频边界（>128KB pcm_buf 上限截断）
- [ ] 半双工（PTT 与 TTS 同 codec）长时压力测试（AEC workaround 已有）
- [ ] agent 侧 PTT 无超时（UI 已 B1 兜底补 stop；极端可加 120s 自动停）
- [ ] dm_ai WS client 自动重连/心跳（agent 重启后 UI 恢复）
- [ ] API Key 明文在固件风险（生产改运行时 config.json 持久化）
- [ ] 蓝牙 H4 110 硬件排查（挂起，用户暂缓，勿擅改驱动）

---

## 11. 上板验证清单

```bash
# 1. 确认联网：UI 状态栏 WiFi 蓝图标 / agent 日志 "Network connected: 10.0.0.2"
# 2. CLI 直测（不经 UI，agent 进程内）
voice_test_tts 你好                     # TTS 合成 → 播放（遗留项首次实测）
voice_test_asr /data/mic.wav            # ASR 识别文件
# 3. UI 链路：AI 子页点按语音圆圈（<1.5s 自动延录 4s）
#    串口预期：WebSocket upgrade OK → ASR: xxx → speak/TTS network done
# 4. 未联网验证：UI 显示 "No WiFi - connect first" 并拦截录音
```

---

## 12. 参考

- 豆包流式识别大模型（bigmodel）：`wss://openspeech.bytedance.com/api/v3/sauc/bigmodel`
  （双向流式；`_nostream` 整段识别 / `_async` 优化版可选，同一二进制协议换 URL）
- TTS：`https://openspeech.bytedance.com/api/v3/tts/unidirectional`（x-api-key 鉴权）
- 鉴权/协议实战文档：GitHub doubao-asr2-openai-proxy（proxy-setup/doubao_asr.md）
- 本板历史：`devlog.md` P54~P69 节点 + `project_docs/devlogs/8-11part2devlog.md`（语音链路全景）
- 前版审计：`AIYOUHUA.MD`（2026-08-11）——本文档为其整合更新版，过时判断已剔除

---
*DoubaoVoice Integration Guide by AtomCode (deepseek-v4-flash) · 2026-08-12 · AIYOUHUA.MD 审计 + 上板实测整合版*


