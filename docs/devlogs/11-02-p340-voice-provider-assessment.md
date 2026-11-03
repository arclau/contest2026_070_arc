# P340 — AI 语音多后端方向评估

2026-11-02；目标 V1 ynm3000；本轮为源码与官方接口评估，未切服务、未改固件。
用户希望评估替代 MiMo，并增加其他语音 API 选项；反馈此前豆包体验流畅。
此前体验来自本板还是手机 App 尚未确认，不将其作为当前固件通过证据。

## 结论与候选

建议先增加明确的语音服务选择：豆包/火山、小米 MiMo；对话大模型独立选择。
优先验证豆包 ASR/TTS + 当前选定 LLM，保留本地设备控制，避免同时更换三段链路。
若交付体验仍不满足，再评估豆包端到端实时语音；Qwen-Omni-Realtime 作为备选。
以上为建议，未将评估授权解释为已经选定供应商或完成迁移。

## 当前源码事实

- `packages/ai_agent/src/voice/voice_asr.c`、`voice_tts.c` 已有注册/选择接口。
  两套后端均保留；`cmd_voice.c` 的 `voice_provider` 能分别写入 ASR/TTS 选择。
- 豆包 ASR 支持边录边上传；当前 MiMo 后端回落为录完后整段识别。
- 流式接口尚未统一进 ops：ASR 绑定火山；TTS 除小米外直接派到火山。
  新增第三家不能只登记名称，否则可能错误调用火山。
- `volc_tts_ws_synthesize_stream()` 当前一次发送完整文本，随即 FinishSession，
  然后接收音频。已有双向协议代码不等于已接通 LLM 增量文本。
- `pair_server.c` 页面专用 MiMo，保存会联动覆盖 LLM/ASR/TTS。
  `llm_proxy_init()`、`llm_router_init()` 因 MiMo Key 存在而优先 MiMo。
  多服务选择必须以显式配置为准，各家凭据分别保存，重启后选择保持。
- 后端切换会 deinit/init，且现实现忽略 init 返回值；需要空闲时切换、校验失败保留旧选择。
- P339 播放欠载已观测；共用回调同步写声卡，接收/播放耦合只是待验证假设。
  更换云服务不能作为板端欠载已解决的证明。

## 官方接口核对

- [MiMo ASR](https://mimo.mi.com/docs/en-US/api/audio/Speech-Recognition)：
  请求传入 WAV/MP3 Base64；`stream` 描述识别结果 SSE 输出，不等于录音流式上传。
- [MiMo TTS](https://mimo.mi.com/docs/en-US/quick-start/usage-guide/audio/speech-synthesis-v2.5)：
  标准 TTS 提供低延迟音频流；不能据当前板端表现断言 MiMo 服务整体不可用。
- [豆包双向 TTS](https://docs.volcengine.com/docs/doubao-voice/websocket-bidirectional-streaming-v3?lang=zh)：
  支持增量文本输入与音频输出；官方建议直接接 LLM 文本流，减少客户端额外攒句。
- [豆包实时语音](https://docs.volcengine.com/docs/doubao-voice/end-to-endreal-time-speech-large-model-api-access-document?lang=zh)：
  WebSocket 音频输入/输出，可选 16kHz PCM16 输入、24kHz PCM16 输出。
  属于新增对话会话适配，需要处理取消、断线、上下文及本地命令与云回复竞争。
- [Qwen 实时语音](https://help.aliyun.com/zh/model-studio/realtime)：
  提供实时音频交互；当前工程未见对应实时会话适配，暂不优先于已有火山后端。

## 建议实施边界与验收

第一步做可选服务与可信观测；随后每次只换 ASR 或 TTS，固定 LLM、短句、网络和板包。
按回合记录松手、ASR 完成、LLM 请求开始/首字/完成、TTS 首包及首个播放写入。
网络首包和写入声卡不是实际可听首音；结合现场听感验收，不再沿用失真的 LLM latency。
建议验收目标（非已测性能）：固定短问答 10 轮无丢回复、无可听断音，首音中位数 ≤2秒，
最慢轮 ≤4秒；同时记录 xrun、错误码和后端身份。未达标按分段数据选择下一实验。
若换豆包后仅首音改善、播放仍断续，下一步隔离本地连续 PCM 播放，不再轮换云厂商。
若播放正常但主要等待 LLM 完整输出，再接 LLM 增量文本到豆包双向 TTS。
端到端实时模式单列能力，首版仍可按键说话；全双工还依赖麦克风、回声消除和打断验证。

## 本轮状态

- CODE：只读评估；保留各仓已有差异，未构建/打包/刷板，未调用付费语音接口。
- DOCS：本详情与索引；依流程检查并导出。既存索引超限不在本轮整理。
- LOGS：本次 session 身份与入仓未核实，标记缺失，不以评估文档替代原始会话。
- 工作区检查器 board 默认仍指旧 gemini 配置，不能用其输出证明 ynm3000 包身份。

## 用户要求复核昨晚原始日志（同日补充）

用户明确要求靶向读取，不整份展开。按后端/耗时/音频错误定位，随后读取字节窗口。
不发送板端命令、不重置语音状态。以下时间均为采集器 UTC；板内显示约为 UTC+8。

- `logs/serial/20261101-081147_p334-board-check.log`：
  第10527～10533行，12:50:23，明确记录 `asr=xiaomi tts=xiaomi`、MiMo Key set，
  并出现 MiMo ASR/TTS 模型及 LLM 的 MiMo host/model。确有 MiMo 启用证据，
  不能以最新一次启动未配置来否定整晚曾使用 MiMo。
  第10726行，12:53:12，LLM watchdog 85585ms；随后声卡 open 返回 -16。
  第12247～12258行，13:22:51，ASR/TTS 为小米，但同次诊断 LLM 返回旧 Ark ClosedEndpoint。
  所以 ASR/TTS 后端不能单独证明 LLM 实际请求路由。
- `logs/serial/20261101-144533_p339-mimo-dialog.log`（latest）：
  第996～1031行，14:47:02～12，先记录火山后端、MiMo Key missing、旧 Ark 400。
  第1089～1105行，14:47:54～14:48:07，TTS 首包990ms，整段12501ms，播放欠载6次。
  第1134～1166行，14:49:43～14:51:23，LLM watchdog 85039ms，TTS 首包1030ms，
  整段100191ms，播放欠载8次；14:51:08起另见声卡 open 返回 -16。
  该窗口没有新的后端身份记录，不能把两轮 TTS 厂商直接标为 MiMo。
  14:48:13附近还记录录音 `readi fail -77`，识别前路径也有异常。
- `LLM watchdog` 在当前源码按 `llm_chat_tools()` 调用前后独立计时；
  它与已知失真的 `[voice] LLM latency` 不是同一个计数。约85秒等待有独立证据。

结论：用户反馈的严重体验问题有原始证据，无须要求重新证明现象。
确认问题至少跨 LLM 等待、播放欠载/声卡占用；尚不支持单独归因为 MiMo TTS 云端架构。
改为可选服务、优先对照豆包的建议保留；换后端与板端音频修复的效果应分别验证。

## 用户指定回查首次豆包开发归档

靶向读取 V1 的 P72～P81、8月16日音频修复、P132 及后续交接，未加载全量历史。

- [8月15日 P74](../archive/v1/devlogs_v1/8-15part1devlog.md)：
  明确记录板端3块音频、48410字节、喇叭出声；P79记录流式识别→LLM回复→TTS出声。
  [同日后续交接](../archive/v1/devlogs_v1/8-15part2devlog.md)记载 ASR/LLM/TTS 全链路可用。
  这是本项目 R528 历史实现，不能再把用户此前豆包体验默认理解为手机 App。
- 当时是火山流式 ASR + 方舟文字 LLM + 豆包 seed-tts-2.0 双向 WS TTS，
  不是端到端实时语音模型；LLM 可独立于语音服务。
- P79 大帧33045字节超过32KB导致截断，补丁为128KB；该节修后板测仍标待验。
  P81记载43秒 LLM等待与连接复用问题，修后约2秒为待验预期，不能冒充稳定统计。
- [8月16日](../archive/v1/devlogs_v1/8-16part1devlog.md)记载声卡占用和尾音丢失处理：
  PTT停音乐释放设备、正常结束drain、打断drop；仍明确列出“LLM完整返回才TTS”的遗留。
- [8月28日 P132](../archive/v1/devlogs_v1/8-28part1devlog.md)明确记载用户验证LLM/TTS全通，
  录音prepare EBUSY须drop清残留状态，单纯等待重试无效；修后上板录音OK。
- 当前源码静态确认：TTS的128KB帧缓冲、4096B升级响应缓冲、flags位判断仍在；
  TLS复用短超时、正常播放drain路径仍在。`volc_tts_ws.c`相对HEAD无未提交差异，
  最近提交323d0c4（9月10日）另修升级完成后空等服务端的问题。
  修复代码存在不等于当前固件已加载或当前运行正常。

方向收敛：优先复用已有豆包路径并提供独立服务选择，有历史板端基础；
无需为恢复旧体验先迁移端到端实时模型。归档未提供足以证明稳定零断音/低延迟的连续统计。
本轮只读历史与源码，未恢复旧快照、未执行归档中的旧构建或固化命令。
