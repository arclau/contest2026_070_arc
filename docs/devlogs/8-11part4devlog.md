# DevLog 2026-08-11 — P63 MIC 硬件上板验证闭环（arecord 命令行录放成功）

> 承接 8-11part3devlog.md §9 遗留事项 1：`nsh> arecord -d 5 -t` 上板实测。
> 结果：**MIC 硬件链路 OK**（P62 遗留项闭环）；顺带揪出 arecord 默认 channels=3 的回放坑。

## 1. 背景

P58 给 SDK arecord/aplay（CONFIG_AUDIO_TEST）补了 MIC1 使能，P60 把栈 4096→16384B。
P62 固化 ok-20260811-31 后唯一待上板项 = 命令行实测 MIC。用户指示：AI 录音 DEMO 不用管，重点命令行。

## 2. 现象（上板实测）

### 2.1 `arecord -d 5 -t`（录 5s 自动回放）——录成功、回放失败

```
nsh> arecord -d 5 -t
arecord start...
dump args:
card:      default        ← 注意是 "default"（映射 hw:audiocodec）
format:    2
rate:      16000
channels:  3              ← SDK 默认 3 声道！
datalen:   480000
[codec] dapm on / POST_ON / START / STOP / dapm off（capture 全流程正常）
aplay start...（回放阶段）
[codec] PRE_ON / dapm on / START / STOP / dapm off
[SND_ERR][snd_hw_constrains_check:1852]hw_params 4 type invalid.params min:3,max:2, hw_cons min:1,max:2
[SND_ERR][ksnd_pcm_hw_params:1998]hw params invalid
Unable to install hw prams! (return: -22)
```

**capture 成功**（无 EBUSY、无崩溃、DMA 正常）→ 反证 AI DEMO 的 -16 是 ai_agent 内部 voice_channel 占用冲突，**不是硬件问题**。

**回放失败 -22**：aplay 沿用 arecord 的参数 channels=3 → DAC playback 只支持 1~2 声道（`hw_cons min:1,max:2`），请求 3 声道越界。

### 2.2 `arecord -c 1 -d 5 /data/mic.wav`（显式 1 声道落盘）——成功 ✅

```
nsh> arecord -c 1 -d 5 /data/mic.wav
card: default / period_size: 1024 / buffer_size: 4096
[codec] dapm on / START / STOP / dapm off（正常）
riffType: RIFF
waveType: WAVE
channels: 1
rate:     16000
bits:     16
align:    2
data size: 160000     ← 5s × 16000Hz × 16bit × 1ch = 160000 字节，分毫不差
```

### 2.3 `aplay /data/mic.wav`（回放刚录的 WAV）——有声音 ✅

```
[codec] PRE_ON#1 / dapm on: RAMP=0x180001（Ramp 使能）...
[codec] START: CNT=0 ...
[codec] STOP: CNT=80896(delta=80896)   ← 播了 80896 帧 ≈ 5s @16kHz
```

**用户确认：有声音。MIC 硬件链路彻底闭环。**

## 3. 根因/结论

| 项 | 结论 |
|----|------|
| MIC 硬件（MICIN1P/MICIN1N + MBIAS → ADC1） | ✅ OK：capture open/DMA/落盘/回放全通 |
| P58 MIC1 使能（snd_ctl_set） | ✅ 生效（无 -22 的 adc_ch 报错） |
| P60 栈 16KB | ✅ 不再 Data abort |
| AI DEMO 的 -16 EBUSY | 非硬件问题：capture 被 ai_agent voice_channel（s_voice.cap）占用未释放，**用户指示不用管** |
| `-t` 回放 -22 | **arecord 默认 channels=3**（arecord.c `audio_mgr->channels = 3`）；ADC capture 支持 1~3 声道，**DAC playback 只支持 1~2 声道** → 自动回放需 `-c 1` |

## 4. 命令行测 MIC 正确姿势（沉淀给后续会话）

```bash
nsh> arecord -c 1 -d 5 -t              # 录 1 声道 5s → 自动回放（一条命令验证）
nsh> arecord -c 1 -d 5 /data/mic.wav   # 落盘 WAV
nsh> aplay /data/mic.wav               # 回放验证
```

**⚠️ 坑**：arecord 不带 `-c` 默认 3 声道，录音能成但 `-t` 自动回放必 -22（DAC 上限 2 声道）。
后续任何 capture 调用（ai_agent audio_capture.c 已用 channels=1~2，无此问题）注意同样约束。

## 5. 遗留 / 下一步（交给下一会话）

1. **AI 语音链路实测**（P54 三大缺口收尾，MIC 已确认）：
   - `ai_agent` 进程内：`voice_test_tts <text>` → TTS 合成（火山凭证 P54 已配）
   - `voice_test_asr <pcm>` → ASR 识别（流式大模型接口 P54 已适配）
   - `voice_start` → 录音→ASR→LLM→TTS→播报全链路
   - UI 侧：AI 子页语音按钮（按住说话）走 WS `{"type":"voice","action":"start|stop"}` 已接线（P53），可全链路验证
2. **AI 子页录音 DEMO（Record 卡）EBUSY**：voice_channel 占用 capture 未释放（ws_server.c:449 voice start 后 s_voice.cap 保持到 stop）。用户指示不用管；若以后修，test_mic 前先 voice_channel_stop 或复用 s_voice.cap。
3. **AI 文本链路**（ask → WS 收回复）仍待上板验证（P52 最小闭环）。
4. 蓝牙 H4 110 硬件排查挂起（用户暂缓）。

---

*DevLog by AtomCode (deepseek-v4-flash)*
