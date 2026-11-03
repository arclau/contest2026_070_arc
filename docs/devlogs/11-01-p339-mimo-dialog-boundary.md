# P339 — MiMo 语音慢：快速提示词与工具执行边界修复

2026-11-01；V1 ynm3000。用户确认已刷 P338，能回复但无法流畅对话。
验收目标：普通语音不产生额外工具轮次，记录 ASR / LLM / TTS 首包耗时。

## 基线与观察

- AGENTS、P330、P336～P338 已读。已有未提交修改全部保留，未 commit/push/tag。
- 仓与配置记录：`output/p339-mimo-dialog/workspace.txt`。
- 本轮修改前副本：同目录 `context_builder.c.before`、`llm_proxy.c.before`。
- 初始沙箱内未见 `/dev/ttyUSB*`；后续确认宿主 CH340 正常，已启动采集（见下文）。
- 旧串口记录有两次约 86 秒 watchdog，13:49:49 / 13:49:51 有 XML 工具调用。
  这些是历史证据，不等于刚刷 P338 的表现。
- 源码与 P338 文档矛盾：`include_tools=false` 仍添加 Capability Boundary、
  Skills 和 Node 工具指令，却错误地跳过本地天气/健康上下文。
- `llm_chat_tools` 未提供 tools 时仍解析原生工具调用和 XML fallback。

## E1：提示词边界

唯一变量：让 `include_tools` 控制工具说明、技能读取指令、技能摘要和 Node 说明；
本地只读数据与现有记忆保留。复杂任务原工具路径保留。
预测：快速提示词不访问工具/技能/Node 注册表，普通提示词仍包含这些能力。
结果：宿主编译真实提示词实现、真实 cJSON，使用隔离记忆/工具桩，修前失败、修后通过。
结论：确认 P338 提示词禁工具未落实；不能据此量化实际网络延迟。
补丁保留。

## E2：响应执行边界

唯一变量：根据实际生成的非空 tools 数组决定是否启用工具解析。
无工具请求收到原生 tool_calls 时明确返回错误并记日志；XML 文本保持文本，
不升级成可执行调用。启用工具时原生/XML 路径保留。
结果：普通文本、XML、原生 tool_calls 在启用/禁用工具两种模式下共 6 例通过。
提示词与响应合计：修前 9 个断言失败，修后 0 失败；UBSan 无报错。
测试：`contest2026_070_arc/tools/tests/voice_prompt/run.py`。
证据：`output/p339-mimo-dialog/before.log`、`after.log`；62 条本地指令回归通过。
补丁保留；未更改复杂度判定、LLM 流式架构或音频仲裁。

## 验证与缺口

- V1 编译通过；首次打包 dragon 被沙箱以 Bad system call 终止，整包校验失败。
  沙箱外重跑通过：配置、ELF 入口、启动、121 资源、nsh、整包校验 PASS。
- 镜像：`output/ynm3000/ynm3000.img`，SHA-256
  `ed9aabd3768cfe07499efa58c504b40e0c7afd3818178926515d5042e237916e`。
  `vela.bin == nsh.fex` SHA-256 `20a7856fc23032e4f0b681f6d0366461c8237e9a3c7343cbb24d295b91aa7668`。
  构建证据：`output/p339-mimo-dialog/build-pack-host.log`。
- 文档已单向导出；doc_check 有既存索引超限/历史孤立文档告警。
- goldfish 编译/运行未做；后续板端用户反馈仍慢、偶发不回复、播报断续，验收未通过。
- 目前 LLM 完整返回后才开始 TTS；消除工具轮次并不能消除串行首音延迟。
- 当前会话日志尚未确认入仓；不以历史日志或手写总结代替原始会话。

## 刷机后串口监听（14:45 UTC 起）

- CH340 已识别；沙箱内设备节点不可见，宿主采集确认 `/dev/ttyUSB0 @1500000` link up。
- 常驻原始串口记录：`logs/serial/20261101-144533_p339-mimo-dialog.log`。
- 收到刷机和桌面启动日志，1920×1200，尚未凭启动日志确认完整镜像哈希。
- 执行 `ai_agent --test config`：`mimo_key=missing`，
  `llm/model/asr/tts=(unset)`。需用户重新扫码配置后再测延迟。
- 采集器未主动停止；新会话需先查 status，不因沙箱节点不可见判断 USB 未连接。


## 会话收尾：实机仍不通过，明日评估实时语音（用户要求暂停）

用户最新反馈：延迟大、有时不回复、AI 播报中断严重。随后询问是否换掉独立 TTS，
改用其他对话模式；尚未选定供应商、未授权迁移到某一服务。用户决定换会话，明天继续。

### 已有运行证据（同上串口日志；时间为 UTC）

- 14:47:02：语音后端为 volcengine，自检 MiMo Key missing。
- 14:47:12：LLM 请求旧 Ark 模型，返回 400 `InvalidEndpoint.ClosedEndpoint`。
  该轮不能作为 MiMo 延迟测试；未获得用户重新扫码保存后的明确确认。
- 14:47:55：TTS 首块约 990ms；截至 14:48:07 的约 12.5 秒播放中，
  `audio_pb xrun, recovering` 出现 6 次。确认有播放欠载，具体诱因尚未实验证明。
- 录音开始附近多次 `audio_cap readi fail -77`，对识别失败的影响待核对。
- 日志 `LLM latency: 42960ms`、`151867ms` **不能直接当作模型请求耗时**：
  `s_asr_done_ts` 仅流式 ASR 成功分支更新，MiMo 的批量 `asr_and_dispatch`
  没更新；其他主动播报也会沿用旧起点。需修正观测再判断模型慢多少。

### 代码检查与未执行事项

- TTS 接收回调直接同步调用 `audio_playback_write` → 阻塞式 `snd_vela_pcm_writei`；
  等待声卡期间同一线程不能继续接收网络音频。8192 帧在 24kHz 下约 341ms。
  “接收/播放耦合导致断粮”为待验证假设，不能写为已证根因。
- 发现正数短写仅返回已写字节数，TTS 回调未补写剩余部分；实际是否出现短写未知。
- `ai_agent --test config` 在分支前调用 `voice_channel_init()`，会重置语音状态、
  信号量及后端；本树 `CONFIG_BUILD_FLAT=y`。此前称其“只读”不准确，
  后续先修此诊断副作用或选择无状态读取，避免用诊断干扰正在运行的语音。
- 本轮最后仅讨论收包/播放双线程缓冲、修正计时，**均未实施、未编译新包**。
  实际新改动仍只有前述提示词/工具边界与宿主回归；已有音频差异属于前轮基线。
- 未执行新的 TTS A/B 实验，未切换供应商、未安装新服务、未 commit/push/tag。

### 对话模式讨论（查阅官方资料，非迁移决定）

- MiMo 当前公开模型列表分为文字生成、ASR、TTS；本次未找到直接可接的实时
  语音到语音接口。不能把多模态音频输入能力当成实时音频输出能力。
- 留 MiMo 可考虑文字流式生成 → 分句 TTS → 连续播放，尚未实现。
- 可替代候选：Qwen-Omni-Realtime、豆包端到端实时语音，官方均有 WebSocket
  语音交互接口。需要新的服务配置与实测；不承诺换服务能自动修复板端欠载。
- 官方入口：
  - [MiMo 模型列表](https://mimo.mi.com/docs/en-US/quick-start/model)
  - [Qwen 实时对话](https://help.aliyun.com/en/model-studio/realtime)
  - [豆包实时语音](https://docs.volcengine.com/docs/doubao-voice/end-to-endreal-time-speech-large-model-api-access-document?lang=zh)

### 明日续接入口

先与用户确定继续 MiMo 分句流式链路，还是评估实时语音到语音服务；
没有决定前不继续叠 TTS 缓冲补丁，也不默认切换某家。
确定后只做一条固定短句的最小对照实验：核实实际后端、有效计时、首音及欠载次数。
若云端延迟下降但仍欠载，下一实验才隔离板端连续 PCM 播放。
会话原始对话入仓未核实，LOGS 缺口保留；串口原始证据与会话归档不能互相代替。
