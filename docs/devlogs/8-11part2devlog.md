# DevLog 2026-08-11 — AI 子 APP 全链路落地笔记（P48~P55 全景）

> 本次会话主题：把 AI 功能从「审查文档」推进到「开机即用 + 语音对接 + MIC DEMO + 崩溃修复」。
> 覆盖：背景 / 整体架构 / 对接 API / 凭证 / 代码改动 / 测试与判断 / 崩溃排查 / 遗留问题。
> 固化 tag：ok-20260811-16 ~ ok-20260811-23（详见 §8）。
> 姊妹文档：`/data/dm/AIYOUHUA.MD`（P52 交付的链路审计 + P54 追加调优落地）。

---

## 1. 背景：本次对话在做什么

R528 openvela（nsh 配置）Desktop Mate 桌面伴侣（luncher_dm），P48 起按用户决策集成官方 `packages/ai_agent`（不抄选手代码），AI 子页（`create_ai_subpage`）作为语音+文本对讲入口。P52 交付了 `AIYOUHUA.MD` 链路审计（8 断点 / 12 调优方向），本次对话完成：

1. **P53**：按 BSP 方法论审计代码证据，修复文档遗漏的可靠性问题（JSON 注入/无超时/失败静默/回复截断/键盘输入）
2. **上板实测 LLM**：断点③（公网 HTTPS）验证通过；修复 404 模型 ID 根因，文本链路闭环
3. **方案 A（用户授权改官方）**：自扩 ws_server 加 voice/mic 控制消息，语音按钮从假联动变真触发
4. **P54**：开机即用（模型/语音凭证编译注入 + ai_agent 开机自启）+ ASR 大模型接口适配
5. **MIC DEMO**：录音快捷入口（UI 卡片 + CLI 命令 + WS 消息 + 自动回放）
6. **崩溃修复（P55）**：luncher_dm 刷新路径 LVGL circle cache double-free → 禁用缓存

---

## 2. 整体架构（三条链路）

```
┌─ 板子（R528, 1920x1200 横屏）────────────────────────────────────────────┐
│                                                                            │
│  luncher_dm（UI 进程）                        packages/ai_agent（进程）     │
│  ┌──────────────────────┐   WS client        ┌────────────────────────┐   │
│  │ AI 子页 create_ai_   │  127.0.0.1:28789   │ agent_main             │   │
│  │ subpage（deskmate_   │ ─────────────────▶ │  ├─ ws_server(28789)   │   │
│  │ ui.c）                │  {"type":"message" │  ├─ message_bus        │   │
│  │  ├─ textarea+键盘(C9) │   /"voice"         │  ├─ agent_loop(ReAct)  │   │
│  │  ├─ 语音按钮(按住说)  │   /"mic"}          │  ├─ llm_proxy ─HTTPS─▶│   │
│  │  ├─ Record 卡片(MIC)  │                    │  │   火山方舟(DeepSeek)│   │
│  │  └─ 状态四态          │                    │  └─ voice_channel     │   │
│  └──────────────────────┘                    │    ├─ audio_capture   │   │
│       dm_ai.c（WS client）                   │    ├─ volc_asr(流式)   │   │
│       dm_ai_poll 轮询                        │    ├─ volc_tts(流式)   │   │
│                                              │    └─ audio_playback  │   │
└──────────────────────────────────────────────┴────────────────────────┘
```

**链路① 文本问答（✅ 上板闭环）**：UI textarea/键盘 → `dm_ai_ask()` → WS `{"type":"message","content":...}` → `ws_server` → `message_bus` → `agent_loop` → `llm_proxy` → 火山方舟 chat/completions → 回复 JSON → UI 回复卡。

**链路② 语音对讲（🔴 待 mic 实测）**：按住说话 → `dm_ai_voice("start")` → WS `{"type":"voice","action":"start"}` → `ws_server` → `voice_channel_start()`（PTT 录音）→ `volc_asr` 流式识别 → `message_bus` → LLM → `voice_channel_speak()` → `volc_tts` → `audio_playback` 喇叭。

**链路③ MIC DEMO（✅ 代码闭环，待上板）**：点 Record 卡片 → `dm_ai_mic_demo()` → WS `{"type":"mic","action":"demo"}` → `ws_server` → `voice_channel_test_mic()` 录音 5s → WAV 落盘 `/data/ai_agent/mic_demo.wav` → 自动回放。

---

## 3. 对接的 API / 服务（三套，全火山系）

### 3.1 LLM — 火山方舟（DeepSeek-V4-Flash，免费额度）
| 项 | 值 |
|----|-----|
| 端点 | `https://ark.cn-beijing.volces.com/api/v3/chat/completions`（OpenAI 兼容） |
| 鉴权 | `Authorization: Bearer ark-REDACTED` |
| **模型 ID** | **`deepseek-v4-flash-260425`**（带日期后缀！裸名 `deepseek-v4-flash` 404） |
| 验证 | `ask 你好` → 1.8s 真实回答 ✅ |

### 3.2 ASR — 豆包语音「流式语音识别大模型」（Seed）
| 项 | 值 |
|----|-----|
| 接口 | `wss://openspeech.bytedance.com/api/v3/sauc/bigmodel`（**大模型版**，非传统 v2/asr） |
| 鉴权 | `X-Api-App-Key: <REDACTED-APPID>` / `X-Api-Access-Key: SQH1ny...` / `X-Api-Resource-Id: volc.seedasr.sauc.duration` / `X-Api-Connect-Id` |
| 协议 | 大模型版 full_client_request（无 app{} 段，`audio.format="pcm"`，`request.model_name="bigmodel"`） |
| 状态 | ⏳ 已适配待上板实测 |

### 3.3 TTS — 豆包语音「语音合成大模型」
| 项 | 值 |
|----|-----|
| 接口 | `https://openspeech.bytedance.com/api/v3/tts/unidirectional` |
| 鉴权 | `x-api-key: <REDACTED-KEY>` + `X-Api-Resource-Id: volc.service_type.10029` |
| 音色 | 默认 `zh_male_beijingxiaoye_emo_v2_mars_bigtts`（可 set_volc_speaker 换） |
| 状态 | ⏳ 待上板实测（预计通——代码与「语音合成大模型」完全匹配） |

---

## 4. 凭证清单（已编译注入 agent_secrets.h，开机免配置）

| 凭证 | 值 | 用途 | 控制台位置 |
|------|-----|------|-----------|
| ark Key | `ark-REDACTED` | LLM | 方舟 API Key 管理 |
| appid | `<REDACTED-APPID>` | ASR/TTS（应用级认证信息，两服务共用） | 豆包语音控制台 → 应用 openveladm |
| ASR token | `<REDACTED-TOKEN>` | ASR 鉴权（代码自动加 Bearer; 前缀） | 应用详情/服务接口认证信息 |
| TTS api_key | `<REDACTED-KEY>` | TTS x-api-key | API Key 管理页 |
| secret key | `yrjGd4POlfaVFSLoG7ciyUUeNvBZ8-Yo` | ❌ 不用（HMAC256 签名才需要） | 同上 |
| cluster | ASR 默认 `volcengine_streaming_common`；TTS `volcano_tts`（代码默认） | 集群 | 服务详情 Cluster ID |

> ⚠️ 控制台链接（console.volcengine.com）是 JS 渲染 + 登录墙，AI 无法代取凭证；模型 ID 需用户从「API 调用示例」复制 `model` 字段原文。

---

## 5. 代码改动清单（按节点，全仓）

### P53 — AIYOUHUA.MD 审计 + 可靠性修复（ok-20260811-17，luncher_dm 侧）
| 文件 | 改动 | 解决的问题 |
|------|------|-----------|
| `apps/luncher_dm/dm_ai.c` | 新增 `json_escape()`；`ai_worker` 提取 `ai_ws_connect()`；SO_RCVTIMEO 30s；fail 路径写错误提示；worker 缓冲转 static | JSON 注入损坏协议 / recv 无超时永久阻塞 UI 卡 Thinking / 失败静默清空回复区 / 4KB 栈缓冲溢出风险 |
| `apps/luncher_dm/dm_ai.h` | `AI_RESP_MAX` 1024→4096 | 长回复截断 |
| `apps/luncher_dm/deskmate_ui.c` | `ai_poll_cb` 缓冲转 static；新增 `ai_ta_focus_cb/defocus_cb/kb_ready_cb`；`ai_kb` 键盘挂接（聚焦弹/失焦收/OK 发送）；close_subpage 清理 ai_kb | C9 键盘输入缺口（纯触屏无法输入） |

### 方案 A（用户授权改官方）— WS 语音/麦克风控制（ok-20260811-18）
| 文件 | 改动 |
|------|------|
| `packages/ai_agent/src/channels/ws_server.c` | 新增 `{"type":"voice","action":"start\|stop"}` → `voice_channel_start/stop()`；`{"type":"mic","action":"demo"}` → `voice_channel_test_mic()` |
| `apps/luncher_dm/dm_ai.c/h` | `dm_ai_voice("start"/"stop")`、`dm_ai_mic_demo()` 短连接 fire-and-forget |
| `apps/luncher_dm/deskmate_ui.c` | 语音按钮按住→`dm_ai_voice("start")`、松开→`stop`（真触发不再假联动）；新增 `ai_mic_demo_cb` + Quick Actions 第 7 项 **Record 卡片** |

### P54 — 开机即用 + 语音对接（ok-20260811-19/20）
| 文件 | 改动 |
|------|------|
| `packages/ai_agent/include/agent_secrets.h` | `MODEL` → `deepseek-v4-flash-260425`（上板实测通）；新增 VOLC_API_KEY/APPID/TOKEN/ASR_CLUSTER 四宏 |
| `packages/ai_agent/include/agent_config.h` | 新增 AGENT_SECRET_VOLC_* 默认空值；ASR 路径 → `/api/v3/sauc/bigmodel` + `AGENT_DOUBAO_ASR_RESOURCE_ID` |
| `packages/ai_agent/src/voice/volc_tts.c` | `volc_tts_init` config_store 空 → secrets 回退（开机免 set_volc_key） |
| `packages/ai_agent/src/voice/volc_asr.c` | init secrets 回退；`ws_upgrade` 鉴权 `Bearer;` → **X-Api-App-Key/Access-Key/Resource-Id/Connect-Id**；`send_full_client_request` 改大模型 JSON（无 app{}、format=pcm、model_name=bigmodel）；两调用点同步 |
| `boards/.../r528s3-gemini-s1/src/etc/init.d/rcS.nsh` | `ai_agent &` 开机自启（CONFIG_EXAMPLES_AI_AGENT_VELA 条件编译，sleep 3 等 WiFi） |

### MIC DEMO（ok-20260811-21/22）
| 文件 | 改动 |
|------|------|
| `packages/ai_agent/src/voice/voice_channel.c/h` | `voice_channel_test_mic(seconds, out)`：media_recorder 录音 → WAV 落盘（44B 头 + 16k/16bit/mono）→ **自动回放**（audio_playback 读 WAV data 段） |
| `packages/ai_agent/src/channels/cmd_voice.c/h` | `voice_test_mic [sec] [out.wav]` CLI 命令（补 stdlib.h） |
| `packages/ai_agent/src/channels/nsh_commands.c` | 注册 `voice_test_mic` + help |

### P55 — 崩溃修复（ok-20260811-23）
| 文件 | 改动 |
|------|------|
| `boards/.../configs/nsh/defconfig` | `CONFIG_LV_DRAW_SW_CIRCLE_CACHE_SIZE=4`→`0`（禁用 LVGL circle cache，消除 double-free 根因） |

---

## 6. 测试与判断（上板实测记录）

### 6.1 断点③ 公网 HTTPS + TLS —— ✅ 通过
日志铁证：`[vela_tls] Handshake OK: TLSv1.2 / TLS-ECDHE-RSA-WITH-CHACHA20-POLY1305-SHA256`。
**判断**：板子 → WiFi → DNS → HTTPS 443 → TLS → 火山方舟全程可达，网络层零问题。

### 6.2 LLM 404 排查 —— 模型 ID 是真实坑（根因+修复）
| 尝试 | 结果 | 判断 |
|------|------|------|
| `deepseek-v3-241226`（P51 注入） | 404 `InvalidEndpointOrModel.NotFound` | 模型已下架/未开通 |
| `doubao-seed-1-6-251015` | 404 同样 | 豆包模型未开通（方舟必须先在控制台开通模型） |
| 用户开通 DeepSeek-V4-Flash 后 `deepseek-v4-flash` | 404 同样 | 裸模型名不带日期后缀不行 |
| **`deepseek-v4-flash-260425`**（控制台 API 示例复制） | ✅ 1.8s 真实回答 | **最终模型 ID** |

**沉淀**：①`list_models` 命令只支持 openrouter 后端，方舟用不了 ②方舟模型 ID 带日期后缀（260425=2026-04-25），以控制台「API 调用示例」`model` 字段为准 ③`/api/v3/chat/completions` 与 `/api/v3/responses` 共用同一批模型 ID。

### 6.3 ai_agent 启动 —— ✅ 全绿
`AI Agent ready` / WS 28789 / voice backends（volcengine ×2）/ network 192.168.*.* / 36 builtin tools 全部初始化成功。

### 6.4 MIC DEMO —— ⏳ 代码闭环，待上板实测
`voice_test_mic` 命令 + Record 卡片已编译进固件（strings 验证：`voice_test_mic`/`mic_demo.wav`/`MIC: recording`/`MIC: playing back` 全命中）。

### 6.5 崩溃 dump 排查 —— LVGL circle cache double-free（详见 §7）

---

## 7. 崩溃排查（P55）：luncher_dm 刷新路径 double-free

### 现象
用户刷 ok-20260811-22 固件后，08:00:50 打开 WiFi 子页 → 08:00:53 串口打出**大量内存 dump + 全任务 backtrace**（PID79 luncher_dm 为主），系统疑似恢复继续运行（08:01 仍有 weather/standby 日志）。

### 定位（现象 → 假设 → 验证）
1. **dump 性质**：`mm_forcefree` 断言 → 这是 NuttX 堆管理器在 `mm_free.c:185 DEBUGASSERT(MM_NODE_IS_ALLOC(node))` 失败 = **double-free 检测**（同一内存被 free 两次，或 free 野指针）。
2. **addr2line 解析 backtrace**（用 strip 前的 `nuttx/nuttx.elf`，不是 strip 后的 `nuttx`）：
   ```
   mm_forcefree                    ← mm_free.c:190  堆 free 校验失败
   _lv_draw_sw_mask_cleanup        ← lv_draw_sw_mask.c:135  lv_free(_circle_cache[i].buf)
   _lv_display_refr_timer          ← lv_refr.c:433  每帧刷新
   lv_nuttx_uv_disp_poll_cb        ← lv_nuttx_libuv.c:308
   lv_nuttx_uv_loop                ← luncher_dm.c:96  主循环
   ```
3. **根因机制**：LVGL 软件绘制（SW）的 **circle cache**（`_circle_cache[4]`，缓存 1/4 圆周长数据供圆角 AA 复用）在 `_lv_draw_sw_mask_cleanup` 遍历槽位时 `lv_free` 一个**已被释放的 buf**。`lv_draw_sw_mask_radius_init` 的缓存复用/临时分配（life<0）与 `lv_draw_sw_mask_free_param` 的释放逻辑存在指针残留，特定绘制时序（打开 WiFi 子页大量圆角卡片 + 阴影）触发。
4. **与我们 AI 改动的关联性**：**低**——崩溃在 LVGL 全局刷新路径，非 AI 子页代码直接调用；但新固件 AI 子页圆角卡片更多、刷新更频繁，触发概率上升。属 LVGL 9.x SW 渲染固有缺陷。

### 修复
`defconfig`：`CONFIG_LV_DRAW_SW_CIRCLE_CACHE_SIZE=4` → `0`（Kconfig 官方注释明确 "Set to 0 to disable caching"）。圆角 AA 每次重算，性能略降，但从根上消除 double-free。
**验证**：distclean 全量重编（增量构建不重编 LVGL，vela.bin 时间戳没变——坑 3 变体）→ `.config` 确认 =0 → 打包 → 产物一致 7758552B → 固化 ok-20260811-23。**待上板确认不再崩**。

### 排障方法沉淀（复用）
- NuttX assert dump 的 backtrace 是**地址数组**，用 `arm-none-eabi-addr2line -e <strip前的.elf> -f -C <addr...>` 批量解析（strip 后的 bin 解析全是 `??`）。
- double-free 特征：`mm_free.c` 里 `DEBUGASSERT(MM_NODE_IS_ALLOC(node))` 失败 + `mm_trace_diag(DIAG_MM_FREE_FAIL, "double-free")`。
- 改 Kconfig/defconfig 里的**库级配置**（LVGL/驱动）后，增量构建可能不重编对应库 → 必须 `distclean` 全量重编并核对 `vela.bin` 时间戳/strings。

---

## 8. 遗留问题 / 没做好的东西（如实清单）

### 🔴 语音链路待上板实测（最大悬念）
1. **mic 硬件从未验证**：media_recorder（MEDIA_SOURCE_MIC）录音是否出数据未知；mic 接线/DMIC 通道未知（AGENTS.md 记录 mic 未验证）
2. **ASR 大模型接口适配未实测**：volc_asr.c 已改 `/api/v3/sauc/bigmodel` + X-Api-* 鉴权，但 voice_test_asr 未跑过，协议细节（full_client_request 字段、Resource-Id `volc.seedasr.sauc.duration`）可能还要调
3. **TTS 未实测**：代码与「语音合成大模型」匹配（/api/v3/tts/unidirectional + x-api-key），预计通但无实锤
4. **半双工切换时序**（断点8）：录音/播放共用 codec，voice_channel 有 AEC workaround（PTT 前停 TTS），但 mic→TTS 连续切换未验证

### ⏳ 待上板验证
5. **开机自启**：rcS 加了 `ai_agent &`，刷 ok-20260811-23 后是否自动起、不抢 stdin 待确认
6. **崩溃修复**：circle cache=0 后 WiFi 子页/AI 子页频繁切换是否不再崩
7. **MIC DEMO 全流程**：点 Record 卡片 → 录音 5s → WAV 落盘 → 自动回放（听感验证）

### ⚠️ 已知工程/安全项（记录未改）
8. **ws_server 绑 0.0.0.0 无鉴权**：局域网内任何设备可连 28789 发消息驱动 agent（官方代码，生产需绑 127.0.0.1 或加鉴权）
9. **API Key 明文在固件**（agent_secrets.h 编译注入）：可被 strings 提取，生产建议改运行时 config.json 或文档提示
10. **shtc3 传感器报错**（`Failed to send measure command`）：既有遗留问题，与 AI 无关

### 📌 下一步建议
- 刷 ok-20260811-23 → 验证：开机自启 → AI 子页键盘问答（文本已通）→ Record 卡片 MIC DEMO → voice_test_tts → voice_test_asr（ASR 大模型接口实测）
- 若 ASR 报错：按报错原文调整 volc_asr.c 的大模型协议细节
- mic 无数据：查 DMIC 通道配置 + audio_capture 6x 增益（CONFIG_AI_AGENT_AUDIO_CAPTURE_GAIN）

---

## 9. 固化 tag 时间线（本次会话）

| tag | 内容 |
|-----|------|
| ok-20260811-16 | P52 AIYOUHUA.MD 交付 |
| ok-20260811-17 | P53 可靠性修复（JSON 转义/超时/错误提示/4096/键盘） |
| ok-20260811-18 | 方案 A：WS voice/mic 控制消息 + UI 真触发 |
| ok-20260811-19 | P54 开机即用（模型 ID + 语音凭证编译注入 + ASR 适配） |
| ok-20260811-20 | 文档更新 |
| ok-20260811-21 | MIC DEMO 代码（voice_test_mic 命令） |
| ok-20260811-22 | AI 子页 Record 卡片 + 自动回放 |
| ok-20260811-23 | P55 崩溃修复（LVGL circle cache=0） |

---

*DevLog by AtomCode (deepseek-v4-flash) · 2026-08-11 · AI 全链路落地笔记（P48~P55）*

