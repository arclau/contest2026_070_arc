# AIYOUHUA — R528 Desktop Mate AI 子 APP 链路审计与调优文档

> 生成日期：2026-08-11 · 作者：AtomCode (deepseek-v4-flash)
> 用途：供高级 AI 审计并给出调优建议。本文覆盖 AI 子 APP 从 UI → 子 UI →
> AI APP → 语音识别 → 大模型 → 回程 → 音频输出的完整链路，含接口/背景/
> API/断点/调优方向。固件版本：ok-20260811-14/15。

---

## 一、背景与架构

### 1.1 项目定位

R528（openvela/nsh）上的 **Desktop Companion**（桌面伴侣）：Home UI（luncher_dm）
整合音乐/天气/提醒/AI，AI 子页（`create_ai_subpage`）是语音+文本 AI 对讲入口。
差异化：不是孤立的"问一句答一句" demo，而是与 Home 音乐/天气/提醒/UI 一体。

### 1.2 整体架构（独立 APP + WS 通道）

```
┌─ 板子（R528, 1920x1200 横屏）───────────────────────────────────────┐
│                                                                      │
│  luncher_dm（UI 进程）                packages/ai_agent（官方 AI 框架） │
│  ┌──────────────────────┐            ┌───────────────────────────┐   │
│  │ AI 子页 create_ai_   │  WS client │ agent_main（独立进程）      │   │
│  │ subpage（deskmate_   │  ────────▶ │  ├─ ws_server (28789)      │   │
│  │ ui.c）                │  127.0.0.1 │  ├─ message_bus           │   │
│  │  ├─ textarea 输入      │  {"type":  │  ├─ agent_loop (ReAct)    │   │
│  │  ├─ 语音按钮(按住)     │   "message",│  ├─ llm_proxy ──HTTPS──▶ │   │
│  │  ├─ 回复显示卡         │   "content":│  │   火山方舟(DeepSeek)   │   │
│  │  └─ 状态四态           │   "..."}    │  └─ voice_channel        │   │
│  └──────────────────────┘            │    ├─ audio_capture(mic)   │   │
│       dm_ai.c（WS 协议）              │    ├─ volc_asr(流式)        │   │
│       dm_ai_poll 轮询                │    ├─ volc_tts(流式)        │   │
│                                      │    └─ audio_playback(喇叭)  │   │
└──────────────────────────────────────┴───────────────────────────┘
```

### 1.3 关键设计决策（已定，勿轻易推翻）

| 决策 | 原因 | 状态 |
|------|------|------|
| 用官方 `packages/ai_agent`，不抄选手代码 | 用户明确要求 | ✅ |
| 独立 APP + WS(28789) 通道，非子模块集成 | 官方推荐形态，UI 完全自控 | ✅ |
| LLM 用火山方舟（ark 免费额度 deepseek-v3-241226） | 国内可达 + 免费 | ✅ |
| 语音 ASR/TTS 用官方火山引擎后端（Doubao） | ai_agent 内置 | ⚠️ 待配凭证 |

---

## 二、链路逐段审查（UI → AI → ASR → LLM → 回程 → 音频）

### 链路① UI 层 → dm_ai.c WS client → ws_server —— ✅ 通

**接口（luncher_dm 侧，我新增）**：
- `dm_ai.h`：`int dm_ai_ask(const char *text)`（非阻塞触发）、
  `int dm_ai_poll(char *buf, int buflen)`（轮询结果，消费式）、`int dm_ai_busy(void)`
- `dm_ai.c`：RFC6455 最小 WS client——base64 Sec-WebSocket-Key 握手、
  masked 文本帧发送、解掩码接收、`{"type":"message","content":"...","chat_id":"ui"}` 发送、
  后台线程 + 300ms UI 轮询、`AI_RESP_MAX=1024`

**官方侧接收**（`packages/ai_agent/src/channels/ws_server.c`）：
- `do_ws_handshake()` 读 Sec-WebSocket-Key → 101 响应 ✅
- 帧解析：`type=="message" && content` → `message_bus_push_inbound()` ✅
- 回复：`ws_server_send()` 构建 `{"type":"response","content":...,"chat_id":...}` ✅
- `agent_main.c` L117/165：`ws_server_start()` 监听 28789 ✅
- `agent_main.c` L261：WEBSOCKET 通道消息 → `ws_server_send()` 回程 ✅

**审查结论**：协议完全匹配，文本链路闭环。潜在调优点：
- dm_ai.c 是**自写最小 WS client**（约 365 行），无自动重连/心跳 ping
- 单轮一问一答（busy 防重入），无流式（官方回复是整段 JSON）
- **无超时上限（原实现 recv 无 SO_RCVTIMEO，agent 不回复时 worker 永久阻塞，200 次轮询形同虚设——已由 P53 修复：30s 收包超时）**

### 链路② message_bus → LLM proxy → 火山方舟 —— ✅ 通（软件层）

**官方侧**（`src/llm/llm_proxy.c`）：
- `llm_http_direct()`：`Authorization: Bearer <key>` + `vela_https_post_json()`
- TLS：`vela_tls.c` mbedTLS 3.4.0（`CONFIG_CRYPTO_MBEDTLS=y`）✅
- OpenAI 兼容：host `ark.cn-beijing.volces.com`，path `/api/v3/chat/completions`
  （已在 `agent_config.h` 固化，Key/模型经 `agent_secrets.h` 官方钩子注入，
  **上板无需 set_llm**）
- 超时：`AGENT_LLM_SOCKET_TIMEOUT_SEC 120`
- 解析：`llm_parse.c` cJSON 提取 `choices[].message.content`（OpenAI 格式）✅
- 多后端：kimi/qwen/deepseek/glm/mimo/openai/openrouter 预设可切换

**审查结论**：软件层全通。风险点：
- **依赖公网 HTTPS 443 可达 ark.cn-beijing.volces.com**（板 WiFi 已验证可用，
  但需实测 DNS + TLS 握手；AGENTS.md 已知 DNS/UDP 曾有问题，HTTPS 未实测）
- `AGENT_SECRET_API_KEY` 编译进固件 = **Key 明文在固件里**（可提取），
  生产前应改运行时 `set_llm` 配置（config.json 持久化 /data/agent/config/）

### 链路③ 语音对讲 voice_channel → capture → ASR → LLM → TTS → 播放 —— ⚠️ 半通（缺凭证/缺触发）

**官方侧**（`src/voice/`）：
- `voice_channel_start()`：rec_thread 循环 `audio_capture_read()` → 
  `voice_asr_stream_send()`（流式 ASR）→ 识别文本 → message_bus → agent_loop →
  回复 → `voice_channel_speak()`（TTS）→ `audio_playback_write()`（PCM 播放）
- `audio_capture.c`：`media_recorder_open(MEDIA_SOURCE_MIC)`（AW 多媒体框架，
  **dev_path 被 `(void)dev_path` 忽略**，`CONFIG_MEDIA=y`/`CONFIG_AW_MULTIMEDIA=y` 已启用）
- `audio_playback.c`：`media_player` buffer 模式 `media_player_write_data()`
- `volc_asr`：Doubao 流式 ASR（`openspeech.bytedance.com/api/v2/asr`，需 appid/token/cluster）
- `volc_tts`：Doubao TTS V3（`/api/v3/tts/unidirectional`，x-api-key 鉴权，
  `voice_tts.c` L102 `if (s_api_key[0]=='\0')` 直接失败）
- 触发：`cmd_voice.c` 的 `voice_start` 是 **agent 进程内 CLI**（`agent_cli` 线程读
  stdin，非系统命令表，**popen 不可达**——跨进程无法触发）

**断点清单（语音链路）**：
1. 🔴 **缺火山语音凭证**：LLM Key 已注入（P51），但 ASR/TTS 需
   `set_volc_asr <app_id> <token> <cluster>` + `set_volc_key <api_key>`（运行时
   CLI 配置，持久化 config.json）——用户尚未提供火山"语音识别/语音合成"服务凭证
2. 🔴 **跨进程触发缺口**：UI 语音按钮（按住说话）只做了状态联动（Listening/
   Speaking），真正触发 `voice_start` 需官方 WS 语音控制消息或自扩
   （ws_server 现仅支持 `type=message` 文本）
3. ⚠️ **media 框架实测未知**：`CONFIG_MEDIA=y` 编译进了，但板上 mic 录音
   （media_recorder → pcm0c）从未实测过——硬件 mic 接线也是未知（AGENTS.md
   记录 mic 未验证）
4. ⚠️ 半双工：录音/播放共用 codec，需确认切换时序

### 链路④ 音频设备路径 vs 板上实际 —— ✅ 无冲突

| 项 | ai_agent 默认 | 板上实际 | 结论 |
|----|--------------|----------|------|
| 录音 | `/dev/audio/pcm0c`（被 `(void)` 忽略） | AW media 框架（media_recorder, MEDIA_SOURCE_MIC） | ✅ 走 AW 栈，与播放同源 |
| 播放 | `/dev/audio/pcm0p`（同上） | AW media 框架（media_player buffer 模式） | ✅ |
| 依赖 | 不依赖 NuttX audio 框架 | `CONFIG_DRIVERS_AUDIO` 未启用，项目走 AW 栈 | ✅ 无冲突 |
| 既有播放 | libcedarx XPlayer（音乐链路已闭环） | 与 media_player 共存（同 codec，需注意并发） | ⚠️ 半双工注意 |

---

## 三、接口/API 总览（供调优引用）

### 3.1 luncher_dm 侧（我的代码）

| 文件 | 接口 | 说明 |
|------|------|------|
| `dm_ai.h` | `dm_ai_ask/poll/busy` | WS 文本问答 |
| `dm_ai.c` | 内部 ws 握手/帧编解码/worker | RFC6455 最小实现 |
| `deskmate_ui.c` | `create_ai_subpage` / `ai_send_cb` / `ai_poll_cb` / `ai_voice_*_cb` | AI 子页 UI + 状态四态 |
| 全局 | `ai_ask_ta/ai_resp_lbl/ai_status_lbl/ai_voice_btn/g_ai_timer` | 子页私有，close_subpage 清理 |

### 3.2 官方 ai_agent 侧（勿改，只对接）

| 模块 | 接口 | 说明 |
|------|------|------|
| WS Server | `ws_server.c` 28789 | `type=message` 收 / `type=response` 回 |
| LLM | `llm_proxy.c` | OpenAI 兼容，Bearer 鉴权 |
| Voice | `voice_channel.c/h` | init/start/stop/speak/test_tts/test_asr |
| ASR/TTS | `volc_asr.c` / `volc_tts.c` | 火山引擎后端，ops 抽象可换 |
| 配置 | `agent_secrets.h`（官方钩子）| 编译期 Key/模型注入（已用） |
| 持久化 | `/data/agent/config/config.json` | 运行时 set_* 覆盖 |

---

## 四、断点汇总（通/不通/缺）

| # | 链路段 | 状态 | 说明 |
|---|--------|------|------|
| 1 | UI → WS client → ws_server | ✅ 通 | 协议匹配（P49/P50 已实现） |
| 2 | message_bus → LLM proxy → 火山 | ✅ 通（软件层） | Key/模型已编译注入（P51） |
| 3 | **公网 HTTPS 443 实测** | ⚠️ 未验证 | 需上板测 `ask` 通不通 |
| 4 | mic → capture → ASR | 🔴 缺凭证 + 未实测 | 需火山语音 appid/token；media_recorder 未上板验证 |
| 5 | TTS → 播放 | 🔴 缺凭证 | 需 `set_volc_key`（x-api-key） |
| 6 | UI 语音按钮 → 触发 voice_start | 🔴 无接口 | 官方 WS 仅文本；需自扩或等官方 |
| 7 | 音频设备路径 | ✅ 无冲突 | 走 AW media 框架 |
| 8 | 半双工录音/播放并发 | ⚠️ 未验证 | 同 codec 切换时序 |

**当前可上板验证的最小闭环**：文本问答（断点 1+2）——`ai_agent &` → `ask` 测试 → AI 子页 WS 发问收回复。

---

## 五、调优方向（请高级 AI 审计）

### A. 链路可靠性
1. dm_ai.c WS client：补**自动重连**（agent 未启动/重启时 UI 应能恢复）、
   心跳 ping（WS 空闲保活）、请求超时上限（避免 UI 状态卡死 "Thinking"）
2. 回复长度：`AI_RESP_MAX=1024` 可能截断长回复 → 评估分页/滚动
3. 火山 LLM 依赖公网 443：**实测 DNS+TLS**，失败时提示网络状态（复用
   dm_net_status_refresh 状态机）

### B. 语音链路（当前最大缺口）
4. **补火山语音凭证**：set_volc_asr/set_volc_key 运行时配置（用户提供 appid/token）
5. **跨进程语音触发方案**（三选一，请审计）：
   - 自扩 ws_server 支持 `{"type":"voice","action":"start/stop"}` 控制消息
   - luncher_dm 用 IPC（socket/管道）写 ai_agent cli_thread 的 stdin
   - 官方后续支持
6. **mic 硬件验证**：先 `voice_test_tts`（TTS 出文件）→ `voice_test_asr`
   （ASR 识别文件）→ 再 `voice_start` 全链路，逐段定位

### C. 体验/差异化
7. AI 子页接 Home 数据：让 AI 能答"现在几度/播放音乐/提醒我"（官方 tools
   已支持 get_weather/get_time/music_play/cron_add）——**这是你的差异化**
8. 状态四态细化：Ready/Listening/Thinking/Speaking 已有，补 Speaking 时长/
   "网络不可用"态
9. UI 调优（skill 审计项）：语音按钮按压反馈、回复卡滚动、**键盘弹出（已由 P53 补：lv_keyboard 挂接 ai_ask_ta，聚焦弹出/失焦收起/OK 发送——原 textarea 纯触屏无法输入）**

### D. 安全/工程
10. **API Key 明文在固件**（agent_secrets.h 编译期）——生产建议改运行时
    config.json（/data/agent/config/），或至少文档提示
11. `CONFIG_EXAMPLES_AI_AGENT_VELA_STACKSIZE=32768` 是否够（LLM 流式解析 +
    cJSON + TLS）——上板观察内存
12. 语音对讲与音乐播放并发：media_player vs XPlayer 同 codec，需互斥策略

---

## 六、上板验证清单（当前固件 ok-20260811-14）

```bash
# 1. 启动 agent（Key/模型已编译注入，无需 set_llm）
ai_agent &

# 2. 直测 LLM（验证公网 HTTPS + 火山方舟）
ask 现在几点了？

# 3. 验证 WS 链路（UI）
#    进 AI 子页 → 输入 → 发送 → 收回复

# 4. 语音（需先配火山语音凭证）
set_volc_key <api_key>
set_volc_asr <app_id> <token> <cluster>
voice_start    # agent 进程内 CLI，验证 mic→ASR→LLM→TTS→播放
```

---

## 七、P54 调优落地（2026-08-11，固化 ok-20260811-19）——开机即用 + 语音对接

> 应需求将 AI 链路做到「开机自启、免配置、语音对接好」。上板实测修复 LLM 模型 ID 后全部落地：

### 7.1 上板实测关键结论（修正 P52 的断点判断）

| 项 | P52 判断 | P54 上板实测 | 修正 |
|----|---------|-------------|------|
| 断点3 公网 HTTPS | ⚠️ 未验证 | **✅ 通**（TLSv1.2 握手 OK，`ssl_read`/pooled connection 正常） | 已闭环 |
| LLM 模型 ID | deepseek-v3-241226 编译注入 | **404 InvalidEndpointOrModel.NotFound**——该模型 ID 已下架/未开通；换 `deepseek-v4-flash-260425`（控制台 API 示例确认的准确 ID）**1.8s 返回** | 模型 ID 是真实坑 |
| 豆包模型 ID | 未评估 | `doubao-seed-1-6-251015` 同样 404（未开通）——**方舟必须开通模型后才可调用，且用控制台 API 示例的准确 ID** | 新增坑 |
| 语音凭证 | 缺（用户未提供） | 用户提供：appid <REDACTED-APPID> / token <REDACTED-TOKEN> / api_key <REDACTED-KEY>... / cluster volcano_tts | 已补齐 |
| ASR 接口版本 | ⚠️ 传统 v2/asr | 用户开通的是**流式语音识别大模型**（Seed，`/api/v3/sauc/bigmodel`，X-Api-* 头鉴权）——v2/asr 必然不通 | 已适配 |

### 7.2 落地改动（本次，全仓）

| 文件 | 改动 |
|------|------|
| `agent_secrets.h` | MODEL `deepseek-v3-241226`→`deepseek-v4-flash-260425`（上板实测通）；新增 VOLC_API_KEY/APPID/TOKEN/ASR_CLUSTER 四个语音凭证宏 |
| `agent_config.h` | 新增 AGENT_SECRET_VOLC_* 默认空值；ASR 接口改 `/api/v3/sauc/bigmodel` + `AGENT_DOUBAO_ASR_RESOURCE_ID "volc.seedasr.sauc.duration"`（Seed 小时版） |
| `volc_tts.c` | `volc_tts_init` 加 config_store 空→secrets 回退（开机即用，免 set_volc_key） |
| `volc_asr.c` | `volc_asr_init` 加 secrets 回退；`ws_upgrade` 鉴权从 `Authorization: Bearer;` 改 **X-Api-App-Key/X-Api-Access-Key/X-Api-Resource-Id/X-Api-Connect-Id**；`send_full_client_request` 改大模型版 JSON（无 app{} 段、audio.format="pcm"、request.model_name="bigmodel"）；两个调用点同步 |
| `rcS.nsh` | `ai_agent &` 开机自启（CONFIG_EXAMPLES_AI_AGENT_VELA 条件编译，sleep 3 等 WiFi）——**用户需求：不再手动命令行启动** |

### 7.3 验证状态

- ✅ 编译/打包/产物一致（7754464B）
- ✅ strings 验证 8 项进固件：模型 ID / 3 个语音凭证 / X-Api-App-Key / Resource-Id / `Starting ai_agent` / `voice start via WS`
- ✅ 上板实测：`ai_agent &` 启动全绿（WS 28789 / voice backends / network 192.168.2.87）；`ask 你好` **1.8s 真实回答**（LLM 链路闭环）
- ⏳ **待上板**：开机自启（刷 ok-20260811-19 后不敲任何命令直接 AI 子页可用）；语音链路 `voice_test_tts`→`voice_test_asr`→`voice_start`（TTS 预计通；ASR 已适配大模型接口待实测；mic 硬件未知）

### 7.4 新增坑（P54 沉淀）

1. **火山方舟模型必须先在控制台开通**：`list_models` 命令只支持 openrouter 后端，方舟用不了——查模型 ID 去控制台「API 调用示例」复制 `model` 字段原文
2. **模型 ID 带日期后缀**：`deepseek-v4-flash-260425`（不是 `deepseek-v4-flash`）；过期模型 ID 报 404 而非友好错误
3. **方舟 OpenAI 兼容端点** `/api/v3/chat/completions` 与 Responses 端点 `/api/v3/responses` 共用同一批模型 ID；agent 用 chat/completions 即可
4. **ASR 大模型版 vs 传统版协议不同**：X-Api-* 头鉴权 + full_client_request 无 app{} 段 + audio.format="pcm"，与 v2/asr 的 Bearer + raw 完全不同

---

*AIYOUHUA.MD by AtomCode (deepseek-v4-flash) · 2026-08-11 · 供高级 AI 审计调优*
