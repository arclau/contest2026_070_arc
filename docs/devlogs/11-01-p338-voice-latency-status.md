# P338 — 语音对话延迟与“播报中”状态错误

2026-11-01；平台 V1 `ynm3000`。用户报告普通 AI 对话经常无响应，
播放音乐时唤醒 AI 会卡住，且 AI 子页未实际播报也显示“播报中”。

## 现象与证据

- 串口会话：`logs/serial/20261101-081147_p334-board-check.log`。
- UI 语音一轮 LLM 调用耗时 `85585ms`，超过 60 秒 watchdog。
- 该轮回复随后尝试 TTS，`hw:audiocodec` 连续 10 次 open 返回 `-EBUSY(-16)`；
  此时音乐已被用户重新打开，最终 `playback open failed`。
- 同一板、同一 Key 的最小 `ai_agent --test llm` 请求 2 秒返回“你好”，
  说明 DNS、TLS、Key 和 MiMo 服务整体可用。
- 后续复测进一步确认：`mimo_api_key` 已存在且 ASR/TTS 均为 `xiaomi`，
  但 `llm_proxy_init()` 仍加载旧通用 `api_key/model/llm_host`，
  `llm_router_init()` 又加载持久化的旧 Ark slot，导致 LLM 返回
  `InvalidEndpoint.ClosedEndpoint` 后以通用失败话术播报。
- MiMo 官方文档确认 V2.6 系列默认开启 deep thinking，且会显著增加
  响应延迟；原请求未设置 `thinking`。
- 板端实查发现配置目录不一致：`ai_agent` 实际使用
  `/data/ai_agent/config/config.json`，设置页却检查
  `/data/agent/config/config.json`，导致已配置 Key 仍显示未配置。
- 新一轮板测“今天天气”：ASR 至首段 TTS 约 10 秒，期间出现两次
  `Parsed 1 tool call(s) from XML fallback`。说明系统提示词仍声明工具，
  即使请求未携带 tools schema，模型仍自发输出 tool call 并多跑 LLM 轮次。
- 该轮已能正常回复，最新日志不再出现 Ark 400；当前主矛盾已从“完全
  不通”收敛为“工具轮次导致首段语音延迟”。
- AI 子页松手时直接写“播报中”，仅靠 22 秒计时复位；没有消费真实的
  `pet_speak/done` 生命周期，因此失败或未进入 TTS 时会误报、卡状态。

## 本轮改动

- `packages/ai_agent/src/core/agent_loop.c`：`voice` 通道的 SIMPLE 复杂度输入
  不携带工具 schema，保留本地设备短路；复杂任务和文本通道仍走原工具链。
- `packages/ai_agent/src/llm/llm_proxy.c/.router.c`：启动时若存在
  `mimo_api_key`，LLM proxy 和 router slot 0 均优先使用 MiMo，拒绝旧 Ark
  持久化配置覆盖。
- 普通语音闲聊同时关闭 tools 与 thinking；复杂任务保持 tools + thinking。
  MiMo 请求显式发送 `thinking: disabled/enabled`。
- 快速语音回合的系统提示词也不再声明工具，并明确禁止输出 tool call；
  ASR、LLM 和 TTS 首包耗时提升为串口可见 NOTICE 日志。
- `agent_main.c`、`pair_server.c`、`ui_ai_cfg.c` 统一使用
  `/data/ai_agent`；配对页写盘失败不再假装“已保存”。
- 新增 `ai_agent --test config`，只打印配置路径、Key 是否存在及有效
  host/model/ASR/TTS，不输出 Key。
- `vendor/allwinnertech/apps/luncher_dm/dm_ai.c/.h`：记录
  `pet_listen/pet_think/pet_speak/pet_idle/done` 实际阶段。
- `vendor/allwinnertech/apps/luncher_dm/deskmate_ui.c`：状态由真实阶段驱动。
  松手先显示“思考中”，首块 TTS 写出才显示“播报中”，完成或失败回待命。
- 60 秒超时只作丢事件兜底，不再用固定 22 秒推断播报结束。
- 用户确认音乐仲裁采用简单语义：AI 通话开始时立即停止音乐并释放声卡，
  结束后不自动恢复；再点播放按钮时从当前歌曲开头重播。

## 验证状态

- V1 `ynm3000` 编译通过；既有 warning 未新增 error。
- `pack_deskmate.sh ynm3000` PASS：配置、入口、启动、121 资源、nsh、整包。
- 镜像 `output/ynm3000/ynm3000.img`，51,511,296 B，
  MD5 `5c98f6ec27d76d13f4ea0430060eb033`；
  SHA-256 `1b346a43b297c4824e6965f662b597ffc279cd3a7b0387ab0ed0bff0904e345e`。
- `vela.bin == nsh.fex`，MD5 `db06059779ba431a92086abeec833607`。
- 尚未刷板；状态修复和普通对话加速均未冒充板端通过。

## 下一步

1. 刷本次 `5c98f6ec...` 镜像；完整刷写若重置 `/data`，重新扫码一次。
2. 串口执行 `ai_agent --test config`，必须显示
   `/data/ai_agent/config/config.json`、`mimo_key=set`、
   `llm=api.xiaomimimo.com/v1/chat/completions`、
   `model=mimo-v2.6-flash`、`asr=xiaomi tts=xiaomi`。
3. 说“今天天气”，检查串口新增的 `ASR finish`、`LLM latency`、
   `TTS first chunk`、`speak done`，且普通语音回合不再出现
   `Parsed ... XML fallback`。
4. 再测音乐中按住说话、仅识别不播报、TTS 完成/失败；确认状态不误报、
   音乐立即停止且不自动恢复。

若 `ai_agent --test config` 仍显示 Ark，优先查烧录镜像身份，不要在应用层
继续改；若显示 MiMo 但仍出现 XML tool call，查本次快速提示词是否进镜像。

## 官方文档核对

- Chat 模型 `mimo-v2.6-flash` 当前有效；请求支持 Bearer、
  `max_completion_tokens`、tools、`thinking`。
- MiMo V2.6 默认开启 deep thinking，官方明确会增加响应延迟。
- ASR 使用同一 Chat Completions 端点、`input_audio` data URL、
  `mimo-v2.5-asr`、`asr_options.language`，当前实现一致。
- TTS 使用 assistant 消息承载正文、`audio.format=pcm16`、
  `mimo-v2.5-tts`、`stream=true`，当前实现一致。
- 当前 LLM→TTS 仍是“完整回答生成后再合成”；尚未实现
  LLM SSE 边生成边分句 TTS，这是后续独立性能改造，不混入本轮修复。
