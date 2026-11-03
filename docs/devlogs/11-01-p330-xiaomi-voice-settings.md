# P330 — V1 AI 语音整体切小米 MiMo + 设置页扫码配 Key

2026-11-01；目标平台 V1 `ynm3000`。承接 P329（只改了文本模型预设，语音仍走火山）。
验收：设置→AI 语音 出二维码 → 手机扫码贴 Key → 板端问答（ASR→LLM→TTS）全链路小米。

## 起因

P329 之后仍有两个硬伤：

1. **语音不是小米的**：ASR/TTS 还是火山流式（`volc_asr.c` / `volc_tts_ws.c`），
   用户要求"语音 AI 全改为小米的服务、小米的模型、小米的语音"。
2. **改 API/换 Key 要改源码重编**：Key 只能写在被 Git 忽略的
   `include/agent_secrets.h`（构建期），或进 NSH 敲 `set_llm` / `set_volc_key`。
   板子没有键盘，现场没法改。用户要求"到 setting 加一个项目来更新这个 API"。

## 小米接口事实（官方文档，2026-11-01 核对）

全部走 OpenAI 兼容的 `https://api.xiaomimimo.com/v1/chat/completions`，
认证 `Authorization: Bearer <key>`（也支持 `api-key:` 头）。

| 能力 | 模型 | 请求要点 | 响应 |
|---|---|---|---|
| 文本 | `mimo-v2.6-flash` | `messages` | `choices[0].message.content` |
| 识别 | `mimo-v2.5-asr` | `content=[{type:"input_audio",input_audio:{data:"data:audio/wav;base64,…"}}]` + `asr_options.language` | `choices[0].message.content` |
| 合成 | `mimo-v2.5-tts` | 文本放 `role:"assistant"`；`audio={format:"pcm16",voice:"冰糖"}`；`stream:true` | SSE `delta.audio.data`（base64，24kHz PCM16LE） |

- ASR 只支持 wav/mp3，base64 后 ≤10MB；**没有流式**，只能整段识别。
- TTS 有低延迟流式；预置音色 `冰糖/茉莉/苏打/白桦/Mia/Chloe/Milo/Dean`。
- 计费：ASR ¥0.5/小时，TTS 限时免费。
- **平台没有 OAuth 设备授权端点**，唯一凭据是控制台创建的 API Key ——
  所以"扫码登录"只能是"把 Key 送进设备"，不是账号授权（见下）。

## 本轮实现

### 1. 小米语音后端（`packages/ai_agent`）

- 新增 `src/voice/xiaomi_asr.c`：实现 `voice_asr_ops.recognize()`，
  现场拼 44 字节 WAV 头 + 流式 base64（不复制整段 PCM），POST 一次拿文本。
- 新增 `src/voice/xiaomi_tts.c`：SSE 流式合成，边收边解 base64 →
  24kHz PCM 直接喂播放端（`AGENT_TTS_WS_SAMPLE_RATE` 就是 24kHz，**不需要重采样**）。
- 新增 `vela_https_post_stream()`（`infra/vela_tls.c`）：带 chunked 解帧的
  流式 POST，4KB 读缓冲，内存不随音频长度增长（否则长回复要几百 KB 缓冲）。
- `voice_asr.c`：非 `volcengine` 后端 `voice_asr_stream_open()` 返回 NULL →
  走既有的批量回落路径（录完再识别），符合 PTT"按住说话"。
- `voice_tts.c`：`speak_stream` 按当前后端名分派（xiaomi / volcengine）。
- `voice_channel.c`：注册四个后端后调用 `voice_channel_reload_backends()`，
  按 `voice_asr_backend` / `voice_tts_backend` 或"有没有小米 Key"自动选。

### 2. 运行时配置（不再改源码）

新增 config key：`mimo_api_key`、`mimo_tts_voice`、`mimo_asr_lang`、
`voice_asr_backend`、`voice_tts_backend`（落在 `/data/agent/config/config.json`）。

NSH 新命令：

```
set_mimo_key <key>        # 存 Key，LLM+ASR+TTS 一起切小米
voice_provider [asr] [tts]
voice_voice <voice_id>
```

### 3. 设置页 + 扫码（`vendor/allwinnertech/apps/luncher_dm`）

- 新增 `src/infra/pair_server.c`（ai_agent 内）：局域网 HTTP 配网页
  `http://<设备IP>:9000/`，表单提交 Key + 音色 + 模型 ID → 写 config → 热切后端，
  同时把 LLM 也切到 MiMo（一个 Key 打通三段）。页面材质按 V1 设计语言实现：
  背景 `#DCE7FA→#F4EDF6` 洗色、白卡 70% + 白边 + `#40516A/30%` 柔影、圆角 22px、
  大标题 + 分组卡 + 图标行，与设置页同构（token 取自 `dm_glass.h` / `deskmate_ui.h`）。
- 新增 `ui/ui_ai_cfg.c`：设置→「AI 语音」子页，显示当前服务/Key 状态，
  并用 LVGL 自带 `lv_qrcode`（`CONFIG_LV_USE_QRCODE=y`）渲染配对页二维码。
- `dm_net.c` 新增 `dm_net_wifi_ip()`（`netlib_get_ipv4addr`）供二维码拼 URL。
- `deskmate_ui.c` 子页分派新增 `"AICfg"`。

### 4. 为什么不是"真·扫码登录"

参考过的两条路：

- **小智（xiaozhi.me）**：设备显示 6 位码 → 手机登录控制台绑定 → 设备轮询拿 token。
  这需要一个自己的后台服务，本项目没有。
- **ESP32 配网（esp32-wifi-manager 等）**：设备自己起 HTTP 页，手机连上填表。

小米 MiMo 没有 OAuth/设备码端点，所以选第二条：**设备本地配网页**，
零外部依赖、断网可用、不经过第三方。二维码内容就是那个 URL。

### 5. 模型 ID / 端点也做成运行时配置（2026-11-01 追加）

参考 `open-vela/contest2026_264_VelaSightsuixingAIzhinengyanjing`（随行 AI 眼镜队）
的做法后补的一刀：他们不在设备上做 ASR/TTS，而是 WebSocket 连自建 console，
并把 **host + model + key 三元组**一起存卡（`conv_net.h` 注释明确写了
"agent 内置的 MiMo 预设指向已下线的模型，所以必须三个一起给，跳过预设"）。

我们直连厂商 API，但"模型名会下线"这个风险是一样的。因此新增
`mimo_asr_model` / `mimo_tts_model` / `mimo_host` 三个 config key，
`xiaomi_asr.c` / `xiaomi_tts.c` 启动时读取（留空回退默认宏），
配对页加了「高级（模型 ID）」折叠区。以后官方换名字，扫码改一行即可，不重刷固件。

## 验证与下一步

### 板端反馈修复（2026-11-01 09:20）

用户实测两个现象：二维码**太大**（占掉半屏）、**关不掉**；随后又指出
「手机扫码配置」六个字位置不对。前两轮我按"太大/手势被吃"改，**方向错了**。

**真根因**：`ui_ai_cfg_create()` 忘了把子页 `content` 设成纵向 flex。
`show_subpage()` 建出来的 content 只是普通容器，子页必须自己
`lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN)`——`ui_settings` /
`ui_wifi` / `ui_pet` / `ui_datetime` 全都这么写，只有我这份漏了。
后果是所有子对象落在同一原点互相重叠：**最后创建的二维码卡盖住整页**
（看起来像"弹了个二维码"），分组标题被压在卡片下面（"位置不对"），
点按也打不到返回键（"关不掉"）。

修复：

- 补 `lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN)` + `pad_row(4)`，
  与其他子页一致。
- 二维码 `DM(190)`→`DM(95)`（≈456→228px，用户要求小一倍，8px/模块仍可扫）。
- 内层容器 `clear_flag(SCROLLABLE|CLICKABLE)`，手势交还 content。
- 二维码卡片可点 → `close_subpage()`，底部标注「点按此处关闭」。
- 版式压缩（内边距 16→10、行距 10→6、删两行说明），整页回到一屏内。

教训：改 UI 前先对照同类页面的构建函数（AGENTS §〇.2 已写明"布局修改必读
该屏完整构建函数"），这次是没照做。

- 编译：`./build.sh vendor/allwinnertech/boards/r528/ynm3000/configs/deskmate -j8`
  通过（含新增 4 个 .c：`xiaomi_asr.c` / `xiaomi_tts.c` / `pair_server.c` / `ui_ai_cfg.c`）；
  `nuttx.map` 里 `ui_ai_cfg_create` / `pair_server_start` 各有 7 处引用，确认已链接。
- 官方流程打包（`pack_deskmate.sh ynm3000`）PASS：配置/ELF 入口/启动/121 资源/
  nsh/整包全项通过，`vela.bin` 与 `nsh.fex` sha256 一致（`2da2e714…`）。
  新镜像 `output/ynm3000/ynm3000.img` MD5 `31ca542a240e98444304c25532363c0a`
  （端口 9000 + DM 设计语言页面 + flex 布局修复 + 二维码减半），51,503,104 B；
  P329 原版备份为 `ynm3000-p329-mimo.img`（`282c7507…`）。
- 配网页视觉：把同一份 HTML/CSS 抽成独立文件用 Edge 无头渲染核对过
  （430/520/900 三种视口），确认卡片圆角+柔影+洗色渐变、图标行对齐、
  底部脚注与卡片同左边界；**真机手机浏览器观感仍待用户复核**。
- **未刷板、未做真实请求**：手上没有 MiMo Key，ASR/TTS 的真实 HTTP 状态与首字
  延迟待验；`vela_https_post_stream` 的 chunked 解帧只有静态审查，无实测。
- 下一步（需用户）：刷 `2a865d0e…` → 设置→AI 语音 扫码贴 Key → 说一句话，记录
  `[xiaomi_asr] POST … bytes` / `[xiaomi_tts] stream done` 与耗时。
- 已知限制：配对页同网段可访问（写入的是访问者自己的 Key）；小米 ASR 非流式，
  PTT 松开后要等整段上传+识别，长句延迟明显高于火山流式。
