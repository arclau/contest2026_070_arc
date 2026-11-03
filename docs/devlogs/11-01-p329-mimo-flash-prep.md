# P329 — V1 AI 后端准备切换 MiMo-V2.6-Flash

2026-11-01；目标平台 V1 `ynm3000`。用户希望优先使用小米服务，接受后续充值。
验收：板端文字问答得到真实回复，再单独核 ASR、LLM、TTS 三段语音链路。

## 现象与服务选择

- 10-31 烧录后串口证明 `ai_agent` 进程启动并联网，不证明问答可用。
- 11-01 00:01:59、00:02:08 的 LLM 请求日志为 HTTP 400，失败原因未定位；
  “当前服务额度耗尽”只是猜测，不能据此定因。
- 当前 NSH 中 `ask` / `router_status` 均报 command not found；不能用这两个
  命令对当前固件做交互测试，后续走设备 UI 或已接入的 WebSocket 客户端。
- 小米官方文档列 `mimo-v2.6-flash` 为当前文本模型；`mimo-v2.5-pro`
  已于 2026-10-21 下线。按量计费国内价格：输入未命中缓存 ¥1/百万 token，
  输出 ¥2/百万 token；ASR ¥0.5/小时，TTS 系列限时免费。
  新客赠金有条件与有效期，不是永久免费额度。

官方文档：
[模型列表](https://mimo.mi.com/docs/zh-CN/quick-start/summary/model)、
[按量计费](https://mimo.mi.com/docs/pricing)、
[活动权益](https://mimo.mi.com/docs/zh-CN/quick-start/faq/promotions)。

## 本轮实现

- `packages/ai_agent` 的 MiMo 预设统一为 `mimo-v2.6-flash`，并设为低成本档。
- 增加可选 `AGENT_SECRET_MIMO_API_KEY`：仅在没有运行时后端且该 Key 已配置时，
  启动默认后端选 MiMo；否则沿用原后端。被 Git 忽略的
  `include/agent_secrets.h` 是已有构建期私密配置入口，不在日志输出 Key。
- 更新 README，说明 `set_llm` 不带 Key 会沿用当前后端 Key，不能直接用来
  从另一服务商切到 MiMo。
- ASR/TTS 仍直接接火山流式实现；小米 ASR 接收 WAV/MP3 Base64，
  迁移需要单独适配音频格式、传输与播放，不随本轮文本模型改动冒充完成。

## 验证与下一步

- `git diff --check` 通过；官方 V1 编译打包与配置/入口/启动/121 资源/整包检查通过。
- 新镜像 `output/ynm3000/ynm3000.img` MD5 `282c7507d77d0d5bba0b3f7289108f72`；
  `vela.bin == nsh.fex` MD5 `1e198706d5d9aefd125bc6e43eed6e8b`。
- 前一包 P328 天气镜像备份为 `output/ynm3000/ynm3000-p328-weather.img`。
- 本轮未读取/写入 API Key，未证明当前有 MiMo Key；未刷板、未做真实 MiMo 请求。
- 下一步：由用户在私密配置入口准备 MiMo Key；核查板端是否已有运行时后端，
  避免优先级覆盖；用同一模型做短文字问答，记录 HTTP 状态与首字耗时，
  成功后再决定是否投入小米 ASR/TTS 适配。
