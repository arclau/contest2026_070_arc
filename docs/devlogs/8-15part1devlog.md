# DevLog 2026-08-15 — 语音对话全链路闭环：TTS/ASR 上板排障 P73~P82 + V0.0.3 归档

## 背景

- 8-14 完成 P72 双向流式 TTS 协议层（本地实测 FULL PASS，官方 protocols_.py + 用户 API Key），但 **C 实现从未真正端到端上板验证过**。
- 本日目标：按 8-14 §九 交接流程（T0~T5）上板验证语音全链路（WiFi→LLM→ASR→TTS→UI 播报）。
- 结果：**TTS/ASR/LLM/播报 全链路闭环达成**，历经 P73~P76 四轮根因修复 + V0.0.3 版本归档。
- 关键教训：**本地 PC 测试用官方 protocols_.py 通过 ≠ 板端 C 实现正确**——C 手写 WS/TLS 客户端与 Python websockets 库在缓冲管理/帧解析上有多处差异，全部靠上板诊断日志（hex dump）逐一揪出。

## P73 TTS 上板卡死根因修复（ok-20260815-1）

### 现象
`ai_agent --test tts hello` → `WebSocket upgrade OK` 后**无任何事件日志且进程挂死**（无 event 50、无超时、无错误码）。

### 排障（现象→多假设→最小验证）
1. 本地复刻 C 的 WS 层（c_ws_replica.py，100% 复刻 upgrade/掩码/解析）直连火山——**1 秒内收到 event 50**，证明帧构造/发送/解析逻辑本身正确。
2. 解压官方 `TTS Websocket Bidirection protocols.zip` → protocols_.py，逐字段对比 C 实现。

### 根因（volc_tts_ws.c 两处协议解析 bug）
1. **seq 判断用 `flags == VOLC_FLAG_POS_SEQ(0x1)` 精确相等**：服务端事件响应帧是组合位 `POS_SEQ|WITH_EVENT=0x5` / `NEG_SEQ|WITH_EVENT=0x7`，精确比较永远不成立 → sequence 未跳过 → event 从错误偏移读取 → ConnectionStarted(50) 匹配不上 → 死循环。
2. **`tls_read_all` 把 `MBEDTLS_ERR_SSL_WANT_READ` 当"继续读"空转**：阻塞 socket + SO_RCVTIMEO 超时后 recv 返回 EAGAIN，mbedtls 映射为 WANT_READ（并非旧注释声称的 -0x004C）→ 超时永不返回 → 无任何错误日志、进程挂死。

### 方案与验证
- 修复：①`recv_wait_event`/`recv_bidir_resp` 两处 flags 改位运算 `flags & VOLC_FLAG_POS_SEQ`（低 bit=带 seq，NoSeq/LastNoSeq 低 bit=0）②`tls_read_all` 连续 3 次 WANT_READ（≈30s 无数据）判 -ETIMEDOUT 返回。
- 验证：语法检查（riscv-none-elf-gcc -fsyntax-only）+ 编译/打包/产物 md5 一致（e41a7f65）+ 三仓 tag ok-20260815-1 + ai_agent 独立 commit 5d69901/tag 同名。
- 产物：镜像 `/data/dm/vendor/allwinnertech/lichee/out/r528s3/gemini-s1_nand/rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img`。

### 插曲：用户反馈字体变小 + WiFi 挂死
- 用户烧 ok-20260815-1 后报「UI 字体整体变小 + WiFi `download_fw FAIL 0x15`」。
- 排查：三仓（vendor/nuttx/apps）ok-20260815-1 与 ok-20260814-5 **diff 为空**（同一 commit）——代码零差异，AI 仅改了 ai_agent 的 volc_tts_ws.c，与 UI/WiFi 无关。
- 回退 ok-20260815-2（= ok-20260814-5）后用户确认 UI/WiFi 正常 → 在稳定版上 cherry-pick 修复为 ok-20260815-3。
- 二次出现字体小+WiFi 0x15 → 用户要求 distclean 全量重编 → ok-20260815-8；验证 res.fex 六项资源齐全（MiSans 字体 ×3 + FW_NIC_ACUT/BCUT.bin + wapi.conf）且镜像内块 md5 MATCH。
- **结论：非代码问题，是烧录时 res 分区未写全**（字体在 `/resource/fonts/`、WiFi 固件在 `/resource/etc/wifi/FW_NIC_ACUT.bin`，都在 res.fex）→ 整包烧录即可。

## P74 TTS 上板出声闭环：resp 缓冲截断最终根因（ok-20260815-10）

### 现象
烧 ok-20260815-3（含 P73 修复）后 TTS 仍失败：`no ConnectionStarted: -104`（-ECONNRESET，服务器主动 TLS close）。不再是卡死（P73 修复生效），但依然等不到 event 50。

### 排障（诊断日志实锤）
- 加诊断版 ok-20260815-9：ws_upgrade 每次 read 打印字节数+hex、last_hdr 偏移、extra 值。
- 上板关键日志：`upgrade read 1023B (rlen=1023) ... upgrade no \r\n\r\n yet` —— **1023B 里竟然没有 \r\n\r\n**，循环因 `rlen < sizeof(resp)-1` 达上限直接退出。

### 根因
**`resp[1024]` 缓冲截断**：火山服务器 101 响应头 + X-Tt-Logid 文本段（X-Tt-Logid/X-Api-Status-Code/X-Api-Message/server-timing/Via/Timing-Allow-Origin/gleId 等，**总长 >1023B**）使 `mbedtls_ssl_read` 一次读入 1023B 即达缓冲上限 → `\r\n\r\n` 未出现 → `last_hdr=NULL` 不缓存 → **真实 WS 帧残留在 TLS 流中** → recv 从流中间错位解析 → 误判 opcode → -104。

### 方案与验证
- 修复：`resp[1024]→[4096]` + 读满未找到 `\r\n\r\n` 显式报错（不再静默截断）。
- 本地模拟：verify_pend_fix.py 覆盖「大响应头 >1023B 分多次 read」场景 → PASS（正确缓存 112B 真实帧 → event 50 解析成功）。
- 上板 FULL PASS（ok-20260815-10，md5 c86ec3f6，ai_agent 374405b）：
  `upgrade read 1224B` → 续读 2B → `last_hdr@1221 b=0x89` → `leftover 2B cached` → RX event 50 → StartSession → event 150 → TaskRequest → TTSSentenceStart → **3 audio chunks (err=0)** → `TTS OK: 48410 bytes` → **喇叭出声（TTS: playback done）**——P72 双向流式 TTS 上板闭环。

## V0.0.3 版本归档（用户指定）

- tag **v0.0.3** = ok-20260815-10（三仓 + ai_agent 独立仓同 tag）。
- 镜像归档 `/data/vela/releases/V0.0.3/rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img`（md5 91770356，md5.txt 已生成）。
- 内容 = **AI 语音对话闭环上板达成（TTS 出声）**。
- 恢复稳点：`bash /data/vela/git_snapshot.sh -r v0.0.3`。

## P75 语音对话全链路闭环：ASR batch 修复（ok-20260815-12）

### 现象
TTS 出声后，**T2/T4/T5 ASR 仍失败**：
- T4 `ai_agent --test full 4` → `No text recognized (0ms)` → -61
- T5 UI 语音圆圈 → stream ASR -104（服务器 WS close）
- 用户质疑「是不是录音设备搞错了？之前录 WAV 播放是 OK 的」

### 排障（用户质疑 → 实测排除）
- `--test asr /data/mic.wav`：`pcm=160000B peak=3489 nonzeroratio=38.1%` + `aplay /data/mic.wav` 播放有声 → **录音链路完全正常**（用户判断正确，P63 已闭环 arecord 链路）。
- 关键疑点：batch `No text recognized (0ms)` —— **0ms 意味着服务器瞬间回了帧**，不是处理完音频。

### 根因（volc_asr.c batch 无条件 break）
大模型 ASR 接口对 full_client_request 先回**多个 duration 递增的中间帧**（seq=1~15，`{"audio_info":{"duration":0→1400},"result":{"text":""}}`），最后才回带文本的最终帧。而 batch 收响应后**无条件 `break`**（P71 修复假设"大模型只回一帧即最终"）→ 把首个空文本中间帧当最终结果 → 空文本 → -61。

### 方案与验证
- 修复（ok-20260815-12，ai_agent 039a15c，md5 5891eef3）：
  1. batch 收响应改为**拿到非空文本或明确最终帧（seq<0）才收**，空帧继续收（SO_RCVTIMEO 兜底不会阻塞）；
  2. 新增诊断 `parsed resp but no text (seq=... json=...)`（dump 服务器实际返回 JSON）。
- 上板 FULL PASS：`parsed resp but no text (seq=1~15)` 中间帧全部收到 → `Recognized: 你好 (infer=265ms e2e=1061ms)` → LLM 回复（88B）→ TTS 13 chunks → **喇叭播报（playback done 240596B）** —— **ASR→LLM→TTS→播报 语音对话完整闭环**。
- 附注：T5 stream 早期 -104 根因=录音能量低（peak 390~636 << 2000）已由捕获增益 ×12→×32 缓解（ok-20260815-11，ai_agent 5a47ce7；实测 chunk peak 2946/5640）。

## P76 T5 stream VAD 参数修复（ok-20260815-13）

### 现象
T5 UI 语音圆圈（stream 路径）仍 -104：
```
chunk#1: peak=3576  chunk#2: peak=798  chunk#3: peak=534
parsed resp but no text (seq=1~9, duration 0→800ms)
Server sent WS close → stream ASR failed: -104
```

### 根因
**服务器在 duration=800ms 处主动 close，恰好等于 `vad_silence_time=800ms`** —— 用户按住说话后段（chunk#2/3 peak 仅 798/534 < 2000）被 VAD 判为静音，800ms 静音即切断会话，还没等到识别结果。batch 成功是因为一次性发完整 5s 大声音频。

### 方案与验证
- 修复（ok-20260815-13，ai_agent 7c14881，md5 c6c29778）：放宽 full_client_request VAD 参数——`start_silence_time` 3000→**2000**（更快开始识别）、`vad_silence_time` 800→**3000**（容忍更长语音间隔，短暂停顿不切断会话）。
- 产物：镜像 md5 c6c29778，三仓 tag ok-20260815-13 + ai_agent 7c14881。
- **待上板验证**：烧 ok-20260815-13 后 T5 按住圆圈大声连贯说话 2-3s → 预期 `ASR: xxx` + 出声。

## P77 T5 stream PTT 关闭服务器端 VAD（ok-20260815-16）

### 现象
烧 ok-20260815-13（P76 VAD 放宽）复测 T5 仍 -104：
```
chunk#1: peak=630  chunk#2: peak=3912  chunk#3: peak=324
parsed resp but no text (seq=1~13, duration 0→1200ms, text 全空)
Server sent WS close → stream ASR failed: -104
```

### 排障（排除项）
1. **固件确认**：本地 vela.bin/nsh.fex md5 c6c29778 = ok-20260815-13，ai_agent tag 7c14881，`send_full_client_request` 内 2000/3000 确在固件中。
2. **客户端全链路正常**：ASR pre-connected → 20 chunks（2s/64000B）全 sent → 松开发空 last-chunk 结束帧（`volc_asr_stream_finish`）；**无 2s 录音上限**（recording_thread 仅因 state 离开 VOICE_RECORDING 退出=用户松手）；WS 短连接重连是 voice start/stop 正常控制消息模式。
3. **duration 800→1200ms 说明 P76 参数部分生效**，但远未到 vad_silence=3000 预期——服务器端仍按静音判定提前 close。

### 根因
**PTT 说话窗口由客户端显式定义（按住录音、松开发结束帧），服务器端 VAD 静音判定与之语义冲突**：vad_signal=1 时服务器端仍在 duration=1200ms 处主动 WS close（-104），未等客户端显式结束帧，识别结果被静音切断丢失。

### 方案与验证
- 修复（ok-20260815-16，ai_agent 08f6872，md5 72df7d80）：`send_full_client_request` 加 `vad_signal` 参数——**batch 传 1**（保留服务器端 VAD，P75 已闭环不变）、**stream（PTT）传 0**（服务器端不做静音判定，只跟随客户端显式结束帧）。
- 验证：编译/打包成功，nsh.fex 与 vela.bin md5 一致 72df7d80；三仓 tag ok-20260815-16 + ai_agent 08f6872 同 tag。
- **待上板**：烧 ok-20260815-16 后按住圆圈说话、松开 → 预期不再 -104，正常出 `ASR: xxx`；若 text 仍空则=语音过短问题（与连接切断无关，可对照 `--test asr /data/mic.wav` batch）。

## P78 T5 stream 根因实锤：-104 误报 + 音频削波（ok-20260815-18）

### 现象
烧 ok-20260815-17（P77 诊断版）复测 T5 仍失败：`Server sent WS close` → -104，text 全空。但诊断日志给出决定性证据。

### 诊断实锤（三处）
1. **JSON 确认**：`full_client_request json: {"request":{...,"vad_signal":false,"start_silence_time":2000,"vad_silence_time":3000}}` —— P77 参数真实发出，VAD 方向整体排除。
2. **close code=1000（正常关闭）**：`Server sent WS close (len=22, code=1000, reason=finish last sequence)` —— 服务器**正常完成会话**（收到客户端结束帧后正常关闭），**不是** 1013 音频静音错误。客户端把正常关闭一律当 -ECONNRESET → **-104 是误报**。
3. **音频削波实锤**：chunk#9/10 `peak=32768`（16bit 满幅）——×32 增益 + 大声说话触发硬 clamp ±32767 削平波形 → ASR 识别不出 → text 空。其余 chunk 500-1300（噪声级），语音段集中在 #4=8082 / #9/#10 削波段。

### 根因
- **-104 误报**：`recv_volc_response` 对任何 WS close 一律返回 -ECONNRESET，服务器正常结束（code=1000 "finish last sequence"）被误判为连接错误。
- **识别空文本**：`audio_capture.c` 硬 clamp ±32767 + 增益 ×32，大声说话削波失真（波形削平），ASR 无法识别。

### 方案与验证
- 修复（ok-20260815-18，ai_agent 4ce4967，md5 bdbe68b1）：
  1. `recv_volc_response`：close code=1000 / 无 code → 返回 1（正常结束，调用方按 text_out 判成功或 no-text），其余错误码（1013 等）仍 -ECONNRESET——stream/batch 共用。
  2. `audio_capture.c`：硬 clamp 改**折线软限幅**（>20000 部分衰减），保留 ×32 增益（batch P75 能量阈值 <2000 不受影响），stream 大声不再削平波形。
- 验证：编译/打包成功，nsh.fex 与 vela.bin md5 一致 bdbe68b1；三仓 tag ok-20260815-18 + ai_agent 4ce4967 同 tag。
- **待上板**：烧 ok-20260815-18 后按住圆圈大声连贯说话 → 预期不再 -104，`ASR: xxx` 正常出（削波已防，正常音量识别）；若仍空文本则需进一步查语音内容/距离。

## P79 TTS 播报中途断：WS 缓冲 32KB→128KB（ok-20260815-19）

### 现象
烧 ok-20260815-18（P78）复测：**ASR 全链路首次闭环**——`stream: recognized: 你好啊。` → LLM 回复（180B）→ TTS 出声。但 **TTS 播报中途断**：
```
volc_tts_ws: WS frame too large: 33045
volc_tts_ws: 14 audio chunks delivered (err=-75)
voice: TTS network done: 10712ms
audio_pb: closing (254284 bytes written) → playback done
voice: TTS stream failed: -75
```

### 根因
`volc_tts_ws.c` 的 `WS_BUF_SIZE` 仅 **32KB（32768B）**，火山 TTS 音频帧最大 **33045B > 32KB** → `ws_recv_frame` 报 `WS frame too large` → -EOVERFLOW(-75) → `recv_bidir_resp` 提前退出 → **话没说完就断**（254KB/14 chunks 后中断，实测文本约 5.3s+）。

### 方案与验证
- 修复（ok-20260815-19，ai_agent abe74d1，md5 4facf2f2）：`WS_BUF_SIZE` 32KB→**128KB**（24kHz 16bit ≈ 2.6s 音频/帧上限，远超实测 33045B；两处 malloc 复用宏，峰值堆 2×128KB 可接受）。
- 验证：编译/打包成功，nsh.fex 与 vela.bin md5 一致 4facf2f2；三仓 tag ok-20260815-19 + ai_agent abe74d1 同 tag。
- **待上板**：烧 ok-20260815-19 后完整对话应**播报到底**（不再中途断）。**ASR→LLM→TTS 链路至此全部闭环 ✅**（T5 语音圆圈可用）。

## P80 唤醒词「你好 openvela」免按键唤醒（ok-20260815-21）

### 背景
T5 语音链路 P73~P79 全闭环后（`ASR: 你好啊。` → LLM → TTS 出声，err=0 完整播报），用户反馈「没说完就没了」= 单轮交互无后续（非技术截断）。用户拍板大改方向第一项：**唤醒词「你好，openvela」**——说唤醒词即可免按键对话。

### 设计（先跑通 → 稳定 → 抽象）
agent 侧 `voice_channel.c` 新增唤醒监听线程 `wake_thread`：
1. **空闲周期短录**：每轮录 1.5s 窗口（`audio_capture` 复用）；
2. **能量门控**：窗口 peak < 1200 视为静音，**跳过 ASR**（省 API 调用）；
3. **stream ASR 匹配**：有语音才 `voice_asr_stream_open → send → finish`，文本去空白/小写后匹配 `openvela`；
4. **命中闭环**：播提示音「我在，请说」（`voice_channel_speak` 阻塞播完）→ 自动聆听指令（录到静音 1.2s 自动结束或 8s 上限）→ batch ASR（复用 `asr_and_dispatch` 打包）→ LLM → TTS；
5. **与 PTT 互斥**：PTT 录音/播报中（`state != VOICE_IDLE` 或 `tts_pb` 活跃）跳过监听窗口。

### 改动（ai_agent 3 文件 + luncher_dm 2 文件）
| 文件 | 改动 |
|------|------|
| `voice_channel.c/h` | `voice_wake_start/stop` + `wake_thread` + `wake_record_window` + `wake_text_match`（+261 行） |
| `ws_server.c` | WS 控制消息 `{"type":"voice","action":"wake_start\|wake_stop"}` 接入 |
| luncher_dm `dm_ai.c` | `dm_ai_voice` 支持 `wake_start/wake_stop`（action 缓冲 8→16） |
| luncher_dm `deskmate_ui.c` | 进 AI 子页自动发 `wake_start` + 状态提示 "Say: Hello openvela"；退出子页发 `wake_stop` |

### 验证与产物
- 编译/打包成功，nsh.fex 与 vela.bin md5 一致 **d0eb0114**；三仓 tag ok-20260815-21 + ai_agent 0a8c828 同 tag。
- **待上板**：烧 ok-20260815-21 后进 AI 子页（状态显示 "Say: Hello openvela"）→ 直接说「你好 openvela」→ 预期：提示音「我在，请说」→ 说指令（如「今天天气怎么样」）→ ASR → LLM → TTS 播报，**全程免按键**。

## P81 语音交互修复：Speaking 状态复位 + LLM 43s 延迟（ok-20260815-22）

### 现象（用户反馈）
1. **UI Speaking 状态不消失**：按下 AI 按钮显示 Listening，松开后一直 Speaking，话说完播完也不复位；
2. **语音延迟厉害**：`[trace] latency=42927ms llm=ok`（43s），日志 `ssl_read (header) ret=0x4c` → `Pooled connection stale, reconnecting` → 重连后才拿到 LLM 响应。

### 根因
1. **Speaking 无复位路径**：语音链路（PTT→ASR→LLM→TTS）播报完成后 agent 不回调 UI，`ai_poll_cb` 只处理文本链路的 `dm_ai_poll`/`dm_ai_busy` → Speaking 永久停留；
2. **LLM 43s 延迟=死连接复用挂满**：连接池复用的连接已被服务器静默关闭（半开），drain 检测只认 `0/PEER_CLOSE_NOTIFY`，`WANT_READ` 漏检 → 复用死连接 → `tls_read_response` 在 120s SO_RCVTIMEO 下**空转等服务器 RST**（实测 41s 后 `ret=0x4c`=NET_RECV_FAILED）→ 才 stale 重连 → 43s。

### 修复（luncher_dm 1 文件 + ai_agent vela_tls.c）
| 文件 | 改动 |
|------|------|
| `deskmate_ui.c` | Speaking 超时复位：`ai_speaking_ts` 计时（release/auto_stop 记录、press 清零），`ai_poll_cb` 超时 22s 兜底复位（正常链路 LLM 2s+首包 5.6s+播放 11.5s≈19s） |
| `vela_tls.c` | ①drain 判定加强：除 WANT_READ/WANT_WRITE 外负值（RECV_FAILED 等）视为死连接立即重连；②`tls_read_response` header/body 循环 WANT_READ 连续计数（2 次判超时），防空转；③复用连接读响应临时短超时 10s（读后恢复 120s）——死连接快速判定 stale 重连，新连接仍 120s 兼容 kimi 长思考 |

### 验证与产物
- 编译/打包成功，nsh.fex 与 vela.bin md5 一致 **1d5339b0**；三仓 tag ok-20260815-22 + ai_agent 5caa6cd 同 tag。
- **待上板**：烧 ok-20260815-22 后：①按 AI 圆圈说话 → 松开 Speaking → 播完 ~19s 内自动复位 "Say: Hello openvela"；②连续多次对话 LLM 延迟应回到 ~2s（不再 41s+）；③唤醒词复测（P80 未上板项）一并验证。

## P82 res.fex 打包源目录纠偏：打包产物验证正确，板端问题在烧录（无代码改动）

### 现象
用户烧 ok-20260815-22（整包镜像）冷启动后 WiFi `download_fw FAIL 0x27` + Settings 页 `FT_Load_Glyph error` 刷屏依旧（1970 时间戳=新烧录），质疑「打包有问题」。

### 排障（本次纠正此前错误结论）
1. **res.fex 真实源目录**：`pack_img.sh` 的 `make_res_image` → `genromfs -f res.fex -d ${dir_name}`，`dir_name = ${PACK_TOPDIR}/board/${PACK_PROJECT_PATH}/data/res`；板级目录不存在 → fallback **`board/common/data/res/`**（lichee 仓内）。此前查的 `/data/dm/vendor/openvela/boards/vela/resource` 是**另一仓的错误目录**——正是 P78 时「res.fex 内字体 md5 与源不匹配」误判的根因（比错目录）。
2. **目录结构匹配**：真实源目录 `fonts/`（复数）+ `etc/wifi/`，与板端代码路径 `/resource/fonts/MiSans-Normal.ttf`、`/resource/etc/wifi/FW_NIC_ACUT.bin` **完全一致**。
3. **内容完整性验证链**：
   - 源 `fonts/MiSans-Normal.ttf` = 7,943,504B md5 **07823a7d** == res.fex 内提取字体 md5（完全一致）；
   - `etc/wifi/FW_NIC_ACUT.bin` = **120,424B**（此前 size=1 是 romfs 目录项解析错误，非真实大小）；
   - 最终整包镜像内完整 res 块 md5 **fe8c8c3b** == res.fex **字节级一致**（`img.find` 定位 + 全块比对 True）。

### 结论
**打包产物完全正确**（源目录 → res.fex → 整包镜像三段一致，六项资源齐全、路径匹配），**无代码改动、无需重新打包**。板端 WiFi 0x27 + 字体刷屏的根因仍在**烧录环节**：res 分区未烧全/烧的是旧 res（P74 插曲同款）——烧录须确认整包镜像含 res 分区并烧后 `ls /resource/` 验证。

### 产物
无代码改动，无新 tag；仅文档更新（AGENTS.md 坑 #3 补充真实源目录，防再次查错目录）。

## P83 弃用云端唤醒词 → 改回纯按钮 PTT（ok-20260815-23）

### 背景（用户反馈）
P80 唤醒词上板后用户实测：
1. **无时无刻在监听，占用资源**——日志显示每 ~2s 一轮完整云端 ASR 会话（TLS→WS upgrade→full_client_request→17 条 seq→close 1000），wake 线程循环录 1.5s 窗口，能量门控 `peak<1200` 从未拦截（MIC 底噪/增益偏高，P57 MIC_PGA_EN 后 peak 恒 ≥1200），每轮都建 TLS 连接做全量 ASR；
2. **说「你好 openvela」不触发**——每轮 ASR 返回空文本 `ret=-61`（=ENODATA，`volc_asr.c:1268` 空文本路径），未到匹配阶段；1.5s 固定窗口与说话时机错位/截断 + 服务器端 VAD 已关（P77 vad_signal=false）→ 识别不出词；且 `wake_text_match` 只认英文 `openvela` 子串，中文音译也匹配不上。

### GitHub 参考检索（用户建议，结论诚实）
- 比赛队伍仓（contest2026_xxx）多为**空模板**，无实质语音代码可抄；
- openvela 官方正道 = **media_trigger 媒体触发器**（本地 DSP 声学模型关键词检测，`load_sound_model→start_recognition→事件回调`，零云端连接）——但本仓无模型文件、defconfig 未开 `CONFIG_MEDIA_TRIGGER`、R528 能否拿模型未知，成本高风险大；
- openWakeWord 是 Python/TFLite，不适合嵌入式直接移植。
- **结论：无现成可抄，务实改现有链路。**

### 方案（用户拍板：放弃唤醒词，纯按钮 PTT）
只有按住 AI 圆圈才录音/对话，不按不录。板端负担最小、最简单。

### 改动（luncher_dm 1 文件 + ai_agent 2 文件）
| 文件 | 改动 |
|------|------|
| luncher_dm `deskmate_ui.c` | ①进 AI 子页**不再自动发 wake_start**（原 P80 逻辑删除）；②初始提示/复位提示改回 **"Hold to talk"**（原 "Say: Hello openvela"）；③退出子页 wake_stop 保留为**防御性**（防旧固件残留监听抢声卡） |
| ai_agent `voice_channel.h` | `voice_wake_start/stop` 声明标注 **P83 已弃用**（保留仅供防御性 wake_stop/旧固件兼容，勿再启用） |
| ai_agent `ws_server.c` | wake_start/wake_stop 分支标注已弃用（UI 不再发 wake_start） |

### 验证与产物
- 编译/打包成功，nsh.fex 与 vela.bin md5 一致 **42da0eeb**（整包镜像 0e1cb031）；三仓 tag ok-20260815-23。
- **待上板**：烧 ok-20260815-23 后：①AI 子页初始显示 "Hold to talk"；②按住圆圈说话 → Listening → 松开 → Thinking → Speaking → 播完复位 "Hold to talk"；③不按任何键时**无 TLS/ASR 连接日志**（不再每 2s 刷屏）。

## P84 小智式交互：对话文字显示 + 播报打断（ok-20260815-24）

### 背景（用户需求）
用户希望做成小智（xiaozhi-esp32）的交互逻辑：**按按钮 → 说话 → 显示文字；AI 回答 → 说话 → 显示文字**。要求先搜小智源码学习其逻辑（它资源占用小）。

### 小智源码调研（克隆 /tmp/xiaozhi-ref，78/xiaozhi-esp32）
1. **协议**：WebSocket 上 JSON 文本 + Opus 二进制音频帧。握手 `{"type":"hello","audio_params":{opus 16k 1ch 60ms}}` → `listen start/stop` → `stt`（用户文字）→ `tts start/sentence_start/stop`（AI 文字 + 音频流）。
2. **按键**：BOOT 键 toggle 状态机——Idle 按 = 开通道+开始听；Speaking 按 = `AbortSpeaking()` 打断；Listening 按 = 关通道。
3. **文字显示**：`display->SetChatMessage("user", stt文本)` / `SetChatMessage("assistant", tts文本)` 上屏。
4. **资源小的本质**：端侧只做录音采集 + Opus 编解码 + 显示 + WS，ASR/LLM/TTS 全在服务器端（`xiaozhi-esp32-server` Python），端侧零本地推理。

### 与现有实现对比结论
我们架构与小智同构（火山 ASR/LLM/TTS 全云端，端侧零推理，P83 弃用唤醒词后零常驻连接）。**缺两块**：①对话文字不上屏（P64 去回复区后只剩状态小字）；②播报中不能打断。链路中 ASR 文本（`text[512]`）与 LLM 回复（`msg.content`）已存在，只是没回传 UI。

### 方案（用户拍板：显示文字 + 播报打断）
复用现有 WS 短连接基础设施，新增**事件广播 + 长连接订阅**通道：
- agent 侧：ASR/LLM 文本产生时 `ws_server_broadcast_voice_evt(kind, text)` 广播 `{"type":"voice_evt","kind":"stt|llm","text":"..."}`；新增 `voice_channel_interrupt()`（小智 AbortSpeaking 式：置 tts_abort + 停 playback，与 speak 内 abort 同路径）。
- UI 侧：进 AI 子页启动常驻订阅线程收 voice_evt（`dm_ai_voice_evt_subscribe/poll`，独立线程变量与 fire-and-forget worker 隔离）；AI 子页新增可滚动对话文字区（"你：…/AI：…" 滚动追加）；Speaking 时按按钮先发 `interrupt` 再录音。

### 改动（ai_agent 4 文件 + luncher_dm 2 文件）
| 文件 | 改动 |
|------|------|
| ai_agent `ws_server.h/c` | `ws_server_broadcast_voice_evt(kind,text)` 广播 + voice action 新增 `interrupt` 分支 |
| ai_agent `voice_channel.h/c` | `voice_channel_interrupt()` 实现；ASR 文本两处（stream finish / batch asr_and_dispatch）广播 stt |
| ai_agent `agent_main.c` | LLM 回复（AGENT_CHAN_VOICE speak 前）广播 llm |
| luncher_dm `dm_ai.h/c` | `dm_ai_voice` 白名单加 `interrupt`；新增 `dm_ai_voice_evt_subscribe/unsubscribe/poll`（长连接订阅线程 + seq 消费） |
| luncher_dm `deskmate_ui.c` | ①AI 子页新增可滚动对话文字区（ai_chat_lbl）②ai_poll_cb 消费事件追加 "你：/AI：" ③进子页 subscribe、退出 unsubscribe + 清指针 ④press 时 Speaking 先 interrupt 再 start |

### 验证与产物
- 编译/打包成功，nsh.fex 与 vela.bin md5 一致 **d657c104**（整包镜像 9fed52d8）；三仓 tag ok-20260815-24。
- **待上板**：烧 ok-20260815-24 后：①AI 子页出现对话文字区；②按住说话 → 松开 → 用户话语显示"你：…"→ AI 回复同时播报+显示"AI：…"；③AI 播报中再按按钮 → 立即打断（状态 Interrupted）→ 可继续说新指令。

## P85 AI 语音子页 UI 重构：去 6 卡 + 玻璃对话框（deskmate-ui skill）

### 背景（用户需求）
用户要求调用 deskmate-ui skill 重新设计 AI 语音子页：
1. 去掉 6 个用不上的功能卡片（Voice/Chat/Schedule/Weather/Reminder/Translate）；
2. aibg PNG 下移到屏幕中下方；
3. 语音按钮居中；
4. 按钮上方是**玻璃对话框**（对话文字显示区）；
5. 追问 xiaozhi 文字显示机制（常驻文本框 vs 有文字才弹出）。

### xiaozhi 文字显示机制（源码实证 lcd_display.cc SetChatMessage）
**常驻容器 + 按需气泡**：屏上有常驻消息列表容器 `content_`（一直在），每条消息到达时动态创建气泡（`msg_bubble`+`msg_text`）：宽度按文本自适应（min 20px / max 85% 屏宽）、`LV_LABEL_LONG_WRAP` 换行；用户消息右对齐绿底、AI 左对齐（role 区分）；上限 20~40 条超限删最旧；空消息跳过；有对话时隐藏居中 logo。→ 玻璃对话框采用同思路：**容器常驻 + 消息逐条追加**。

### 设计（deskmate-ui skill：浅色 Liquid Glass）
- 全部子元素改 **lv_obj_align 绝对定位**（不再 flex column）：
  - 玻璃对话框 chat_cont：宽 70%、高 DM(130)，Liquid Glass 配方（半透明白底 LV_OPA_60 + 白边 3px/50% + 蓝紫阴影 20px/20%），定位 `LV_ALIGN_CENTER, 0, -DM(135)`（按钮上方）；
  - 语音按钮 ai_voice_btn：`LV_ALIGN_CENTER` 屏幕正中；
  - 状态文字 ai_status_lbl：按钮下方 `LV_ALIGN_CENTER, 0, DM(45)`；
  - aibg PNG：中下方 `LV_ALIGN_CENTER, 0, DM(140)`。
- 删除 6 快捷卡（actions[] + grid + for 循环）+ 其回调 ai_action_cb（避免 unused 警告）。

### 改动（luncher_dm 1 文件）
| 文件 | 改动 |
|------|------|
| luncher_dm `deskmate_ui.c` | `create_ai_subpage` 重构：去 6 卡；bg/按钮/状态/玻璃对话框全部绝对定位；chat_cont 改 Liquid Glass 玻璃配方（P84 文字区升级为玻璃对话框）；删除 ai_action_cb（含 hints[]） |

### 验证与产物
- 编译报错 1 次：`LV_OPA_25` 不存在（LVGL 只支持 10 的倍数枚举）→ 改 `LV_OPA_20`，重编通过。
- 编译/打包成功，nsh.fex 与 vela.bin md5 一致 **86d42001**（整包镜像 a9f60399）；三仓 tag ok-20260815-25。
- **待上板**：烧 ok-20260815-25 后：①AI 子页无 6 卡，仅「玻璃对话框（上）+ 语音按钮（中，居中）+ aibg（中下方）」三元素；②按住说话 → 松开 → "你：…" 出现在玻璃对话框，AI 回复同时播报+"AI：…"上屏；③播报中按按钮打断；④玻璃对话框随对话滚动。

## P86 修复 P85 UI 重构引入的 Data Abort（ok-20260815-26）

### 现象（用户上板实测）
点击 AI 图标打开 AI 子页后立即崩溃：
```
show_subpage: Open subpage: AI done
[ws] Client connected/disconnected
Data abort. PC: 415fb678 DFAR: 00e3a018 DFSR: 00000005
```

### 根因（addr2line/System.map + 反汇编 + LVGL 源码三重定位）
1. **崩溃点**：`415fb678` = `lv_obj_get_child_count+0x4`（`ldr r0,[r0,#8]` 读 `obj->spec_attr`），DFAR=0x00e3a018 → **obj 指针为野指针 0x00e3a010**。
2. **调用链**（backtrace 全解析）：`_lv_display_refr_timer` → `lv_obj_update_layout` → `layout_update_core` → 递归遍历 `obj->spec_attr->children[i]` 读到野 child → `lv_obj_get_child_count(child)` 崩溃。
3. **根因**：P85 在 `create_ai_subpage` **对象树构建中途**逐个调用 `lv_obj_align(bg/btn/status/chat_cont)`——LVGL 的 `lv_obj_align_to` **第一行就是 `lv_obj_update_layout(obj)`**（源码实证 lv_obj_pos.c），立即触发**递归整棵 screen 树**的 `layout_update_core`；此时 `chat_cont` 等对象尚未完全构建、`lv_pct(70)` 嵌套百分比宽度未解析 → 遍历到半初始化对象的 children 数组 → 野指针。P84 用 flex column 无此问题（flex 子元素不显式 align）。

### 修复（luncher_dm 1 文件）
| 改动 | 说明 |
|------|------|
| 4 处 `lv_obj_align` 从创建中途**延迟到函数末尾统一执行**（bg/btn/status/chat_cont 全部子对象创建完成后一次定位） | 布局只跑一次且对象树完整，杜绝构建中途触发整树布局 |
| `chat_cont` 宽度 `lv_pct(70)` → **固定像素 1920*70/100** | 消除嵌套百分比在未布局容器内的解析问题 |

### 验证与产物
- 编译/打包成功，nsh.fex 与 vela.bin md5 一致 **2282faf5**；三仓 tag ok-20260815-26。
- **待上板**：烧 ok-20260815-26 后复测：①点 AI 图标进子页不再崩溃；②玻璃对话框/按钮/aibg 三元素定位正常；③对话文字显示 + 播报打断回归。

## P87 修复 P86 上板仍崩溃：订阅线程栈溢出（ok-20260815-27）

### 现象（用户烧 ok-20260815-26 复测）
点 AI 图标进子页**仍崩溃**，但线程与上次不同：
```
show_subpage: Open subpage: AI done
[ws] Client connected: ws_4 (fd=4)
Assertion failed panic: task(CPU0): luncher_dm 0x4169769d   ← PID 165（非主线程 113）
PC: 4145931f = _assert+0x67
User Stack: base 0x421f3570 size 0x00000009                ← 栈信息异常（9 字节！）
```

### 根因（地址解析 + 代码审查）
1. **崩溃线程**：PID 165 入口 0x41697691 = `ai_voice_worker` 相邻的 **`ai_evt_worker`（P84 新增的语音事件订阅线程）**；panic 地址 0x4169769d = `ai_evt_worker+0x1`。
2. **User Stack size=9 字节异常**：订阅线程用默认 pthread 栈创建（`pthread_create(&g_ai_evt_thread, NULL, ai_evt_worker, NULL)` 无 attr），而 `ai_evt_worker` 内 `char text[AI_RESP_MAX]`（**4096B 栈上数组**）+ `char kind[16]` + `char buf[AI_RESP_MAX]`（static）→ **栈溢出破坏 TCB 栈字段** → 后续 `_assert` 失败崩溃。
3. **⚠️ 教训复发**：这正是 P53 教训（"AI_RESP_MAX 1024→4096 后栈上 4KB 缓冲可能压爆默认 pthread 栈"）——P84 新代码 `ai_evt_worker` 里 `char text[AI_RESP_MAX]` 又写成栈上数组。同函数 `buf` 已是 static，唯独 `text/kind` 漏了。
4. **R0=41bd4250 与 P86 相同是误判**：该地址是内核全局 `g_last_regs`（panic dump 公共值），并非同一对象——P86 修的是主线程布局野指针（真实有效），P87 修的是订阅线程栈溢出（第二个独立崩溃点）。

### 修复（luncher_dm 1 文件）
| 文件 | 改动 |
|------|------|
| luncher_dm `dm_ai.c` | `ai_evt_worker` 内 `char kind[16], text[AI_RESP_MAX]` 改 **static**（与同函数 buf 一致，防默认线程栈溢出） |

### 验证与产物
- 编译/打包成功，nsh.fex 与 vela.bin md5 一致 **6c2de44c**；三仓 tag ok-20260815-27。
- **待上板**：烧 ok-20260815-27 后复测：①点 AI 图标进子页不再崩溃（P86 主线程布局 + P87 订阅线程栈双修复）；②订阅线程能稳定收 voice_evt（对话文字上屏）；③其余回归同 P86 清单。

## P88 AI 子页三项优化：布局下移 + 文字去重 + openvela 健康助理人设（ok-20260815-28）

### 背景（用户需求）
1. **布局下移**：语音按钮往下挪，Hold to talk 紧贴下方 PNG 图，对话框随之扩大；
2. **文字重复**：说"你好"后对话框出现两次"你：你好。"；
3. **人设预设**：给 agent 设名字 openvela，角色为"牛马健康助理"（打工人健康助理）。

### 根因与改动
1. **文字重复（dm_ai.c）**：`dm_ai_voice_evt_poll` 只在 seq 变化时写输出缓冲；ai_poll_cb 的 `evt_stt/evt_llm` 是 **static**——第二轮只有 llm 新事件时，残留的旧 stt 仍非空 → 被重复追加。修复：**seq 未变化时显式清空输出缓冲**（stt[0]='\0' / llm[0]='\0'）。
2. **布局下移（deskmate_ui.c）**：统一 align 段整体下移——PNG 中心 DM(155)≈972（底部）、按钮 DM(55)≈732、Hold to talk DM(90)≈816 贴 PNG 顶 857、对话框中心 -DM(75)≈420 且高 DM(130)→**DM(185)**（顶 198 底 642，扩大约 130px）。
3. **人设（ai_agent context_builder.c）**：系统提示词头部注入「你是 openvela——打工牛马健康助理，关心久坐/喝水/护眼/作息，轻松幽默但专业」，全频道生效（Feishu/WS/CLI/Voice）。

### 改动（ai_agent 1 文件 + luncher_dm 1 文件）
| 文件 | 改动 |
|------|------|
| luncher_dm `dm_ai.c` | `dm_ai_voice_evt_poll` 无新 stt/llm 事件时清空输出缓冲（防 static 残留重复） |
| luncher_dm `deskmate_ui.c` | 布局整体下移：PNG DM(155)/按钮 DM(55)/Hold to talk DM(90)/对话框 -DM(75)，对话框高 DM(185) |
| ai_agent `context_builder.c` | 系统提示词头部注入 openvela 牛马健康助理人设 |

### 验证与产物
- 编译/打包成功，nsh.fex 与 vela.bin md5 一致 **af1e4693**；三仓 tag ok-20260815-28 + ai_agent 99d0eca（手动同步，git_snapshot REPOS 不含本仓）。
- **待上板**：烧 ok-20260815-28 后：①AI 子页对话框更大且整体下移，按钮/Hold to talk 贴 PNG；②说"你好"只显示一次"你：你好。"（不再重复）；③问"你是谁"→ 回复自称 openvela 健康助理。

## P89 Hold to talk 再下移贴 PNG（ok-20260815-29）

### 背景（用户反馈）
P88 布局上板"好像可以用了"，但要求 Hold to talk 继续往下调，更贴下方 PNG。

### 改动（luncher_dm 1 文件）
| 文件 | 改动 |
|------|------|
| luncher_dm `deskmate_ui.c` | `ai_status_lbl` align 偏移 DM(90)→**DM(100)**（中心 816→840，距 PNG 顶 857 仅 17px） |

### 验证与产物
- 编译/打包成功，nsh.fex 与 vela.bin md5 一致 **51f28736**；三仓 tag ok-20260815-29（仅 luncher_dm 改动，ai_agent 无变动无需同步）。
- **待上板**：烧 ok-20260815-29 后确认 Hold to talk 紧贴 PNG 顶部（间距 ~17px）。

## P90 布局微调：按钮下移 + Hold to talk 贴 PNG 顶 + 对话框扩大（ok-20260815-30）

### 背景（用户讨论）
用户问 Hold to talk / 按钮还能否下拉，要求 Hold to talk 直接停在 PNG 上边缘。先算空间账：Hold to talk 底边 862 已越过 PNG 顶 856.5 约 5.5px（轻微压图），按钮底 787→文本顶 818 有 31px 空隙。结论：Hold to talk 不能再下（应微上移贴边），按钮可下移。

### 方案（用户拍板：微调 + 对话框再扩大）
| 元素 | 旧（P89） | 新（P90） |
|------|-----------|-----------|
| 玻璃对话框 | 中心 420，高 DM(185) | **中心 396（-DM(85)），高 DM(210)**：顶 144 底 648 |
| 语音按钮 | 中心 732（DM(55)） | **中心 744（DM(60)）** |
| Hold to talk | 中心 840（DM(100)） | **中心 830（DM(96)）**：文本底 852，PNG 顶 856.5，间距 4.5px 恰好贴边 |
| aibg PNG | 中心 972（DM(155)） | 不变 |

### 改动（luncher_dm 1 文件）
| 文件 | 改动 |
|------|------|
| luncher_dm `deskmate_ui.c` | chat_cont 高 DM(185)→DM(210)、align 段：按钮 DM(55)→DM(60)、Hold to talk DM(100)→DM(96)、对话框 -DM(75)→-DM(85) |

### 验证与产物
- 编译/打包成功，nsh.fex 与 vela.bin md5 一致 **e01c0b26**；三仓 tag ok-20260815-30（仅 luncher_dm，ai_agent 无变动）。
- **待上板**：烧 ok-20260815-30 后：①Hold to talk 文本底边恰好贴 PNG 上边缘（不再压图）；②按钮下移至 744；③对话框更高（DM(210)）文字区更大。

## P91 Hold to talk 再下移一个身位压入透明 PNG（ok-20260815-31）

### 背景（用户澄清）
PNG 是透明的，Hold to talk 可以放心压到图上区域——再往下移一个身位（FONT_LABEL 44px ≈ DM(18)），按钮不动。

### 改动（luncher_dm 1 文件）
| 文件 | 改动 |
|------|------|
| luncher_dm `deskmate_ui.c` | `ai_status_lbl` align 偏移 DM(96)→**DM(114)**（中心 830→**874**，文本底 896 压入透明 PNG 区）；按钮/对话框不动 |

### 验证与产物
- 编译/打包成功，nsh.fex 与 vela.bin md5 一致 **38602b13**；三仓 tag ok-20260815-31（仅 luncher_dm，ai_agent 无变动）。
- **待上板**：烧 ok-20260815-31 后确认 Hold to talk 压在 PNG 上部透明区域（身位下移），按钮仍 744 不动。

## P92 打包后资源检查纪律固化（写死 AGENTS.md）

### 背景（用户反馈）
板端 WiFi 0x27 + 字体 glyph 刷屏 + DNS fail 老毛病反复出现（P74/P82 已实证 = res 分区未烧全）。用户明确要求：**下次不要再出这种问题，打包之后必须检查**，并写死进 AGENTS.md。

### 调查（本次重新自查，排除打包侧问题）
1. **分区表**：sys_partition.fex 中 res 分区定义正确（size=51200、downloadfile=res.fex）。
2. **res.fex 时序**：res.fex 11:24 生成，比源目录资源（8-3）新 → 每次打包重新生成，无旧缓存。
3. **镜像内 res 内容**：res.fex 完整字节位于整包镜像偏移 0x9a1400，md5 **fe8c8c3b**（与 P82 基准一致），字体 07823a7d / WiFi 固件完整包含 → **打包产物正确，问题在烧录环节**。
4. **用户最终确认**：最新一版已处理好（烧录工具需确认 res 分区被实际写入——P74 插曲同款）。

### 固化内容（AGENTS.md 写死）
| 位置 | 写入 |
|------|------|
| §二 必做动作 | 新增**第 4 步「打包后资源检查」**（python 脚本：res.fex 含字体/固件 + 镜像含完整 res 块，失败 assert 禁止固化）→ 原第 4 步固化变第 5 步；并加"打包后不跑资源检查 = 任务未完成" |
| §四 已知坑 #3 | 强化：打包后必跑 §二 第 4 步脚本；**全志烧录工具默认可能不烧 res 分区，分区勾选界面必须确认 res 被写入**（P74 插曲实证：镜像没问题，烧录工具没写 res） |

### 验证
- 脚本在本次调查中实跑通过（三段 md5 一致、镜像含完整 res 块 True）。
- 无代码改动，无新 tag（纪律文档固化，当前代码仍 ok-20260815-31）。

## 遗留事项

- **烧录验证（P74/P82 实证，最高优先）**：整包镜像烧录（**必须含 res.fex 分区**，勿只烧 nsh.fex）→ 烧后 NSH `ls /resource/etc/wifi/ /resource/fonts/` 确认资源在板端 → WiFi 0x27 + 字体刷屏应消失（打包产物已三段 md5 验证正确：源目录 `lichee/board/common/data/res/` → res.fex → 整包镜像）。
- **T5 语音交互复测（P81/P83 修复后）**：①按 AI 圆圈说话 → Speaking 播完 ~19s 自动复位回 "Hold to talk"；②连续多轮对话 LLM 延迟应回 ~2s（不再 41s+）；③纯按钮 PTT：不按任何键时无 TLS/ASR 连接日志（P83 弃用唤醒词后不再每 2s 刷屏）。
- T4 `--test full 4` 端到端复测（batch 修复后应 `ASR→LLM→TTS→出声`）。
- 蓝牙 H4 110 硬件排查挂起（用户暂缓，勿擅改驱动）。
- 状态栏改进需求（中文年月日/实时前缀/WiFi 图标，P46 回退保留）。
- AI 语音 APP 大改方向（唤醒词已弃用 P83 改纯按钮 PTT；剩播报打断/多轮对话/参数调优，待用户拍板）。

*DevLog by AtomCode (deepseek-v4-flash)*

