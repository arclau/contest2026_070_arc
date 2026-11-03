# P354 本地 API 与反馈离线化

## 目标与边界

- 用户要求：本地 API 调用不走互联网、不请求语音 AI。
- 平台：V1 `ynm3000` / `luncher_dm`；V2 不改。
- 验收：无 Wi-Fi、无 AI Key、AI 正忙时，本地按钮仍执行；本地动作不请求 ASR/LLM/TTS。
- 麦克风入口仍需云端 ASR。本次没有引入离线识别，不宣称断网能说话控制。

## 观察、实验与结论

O：源码中 `voice_local_cmd_ack()` 为宠物/开页请求云端 TTS；UI 分发器为音量、亮度、闹钟请求 TTS。
本地音乐虽已绕过网络门禁，仍受 `dm_ai_busy()` 拦截。旧交接中“直接分发完全离线”的结论不完整。

H：本地执行器可离线，入口门禁和执行前后确认引入了云端依赖。

E1 / 宿主：从修改前源码提取宠物分发路径，只计数 TTS 与命令广播。
结果 `pet:sit` 产生 `cloud_tts=1 cmd_events=1`；证据 `output/p354-local-api/baseline-ack.log`。
改动变量为本地入口/反馈路由，八类底层执行器保留；修后调用边界测试通过。
结论：这些路径的额外云端确认调用已消除；真实设备执行与音效仍待验证。补丁保留。

## 改动

- `deskmate_ui.h/.c` 开放 `dm_local_cmd_dispatch()`，仅供 LVGL 线程内可信命令调用。
  支持 app/music/volume/brightness/pet/scene/health/alarm；返回值仅表示前缀被接管，不代表硬件成功。
- 音量/亮度/闹钟确认改文字气泡与已有本地 `bg_reward_std.wav`。
  音乐、录音、AI 播报期间跳过提示音，不因音量确认停掉音乐；音效失败不回退云端。
- 宠物先显示“好的”再执行；agent 的本地分支仅发命令与 idle，不等待云端确认。
- AI 页面新增坐下/睡觉/散步五分钟/回来四个按钮，直接调用上述 API，不经过 agent。
- 时间、音乐快问在联网与 AI 忙碌门禁之前执行。时间按首页 UTC+8 显示，音乐继续走本地曲库。
- 天气、笑话与麦克风保留原联网路径；页面提示明确语音需联网、本地按钮可离线。

## 验证与产物

| 项 | 结果 / 证据（工作区相对路径） |
|---|---|
| 本地调用边界 | PASS，八类分发、离线/忙碌组合、提示音仲裁、UTC+8 跨日；`output/p354-local-api/local-api-test.log` |
| 命令解析回归 | 62/62；`output/p354-local-api/voice-commands-test.log` |
| LVGL 宿主构建 | 实际页面构建函数，八按钮边界/文字、三次创建销毁、十二次本地点击 PASS；`output/p354-local-api/layout-test.log` |
| goldfish | 构建失败，GNSS `NAN/sqrtf` 与 libcxx 数学声明错误；未运行新版，不计视觉验收 |
| V1 编译/打包 | PASS，121 资源、入口/启动/整包校验通过；`output/p354-local-api/build-pack-host.log` |
| 镜像一致性 | `vela.bin == nsh.fex`；`output/p354-local-api/delivery.json` |
| 板端运行 | 未刷板；声音、实机动作、UI 观感、联网流量待验 |

镜像：`output/ynm3000/ynm3000.img`，MD5 `14ef2705fec53b059f7690b2b6badebb`。

构建时沙箱先阻止 ccache 写入和 SDK dragon 系统调用，获准在沙箱外重试后 V1 通过。
goldfish 临时移开的板配置和头文件已恢复。未修无关 math/libcxx 问题。
宿主测试替换了硬件/网络边界，不等于板端抓包或完整固件运行。

## 基线与收尾

- vendor/allwinnertech HEAD `4c122d896b8ca786ad3f9a6768f37eeecdfe71e8`，已有改动保留。
- packages/ai_agent HEAD `5918940a7c089a7525cc15c763696bb4d716514e`，已有改动保留。
- 修改前涉及文件在 `output/p354-local-api/baseline/`；镜像导出前保留 14 个差异文件于 `mirror-backup/`。
- 官方源码导出已执行；四个旧 V2 映射源不存在，镜像全量检查仍告警，未补造文件。
- workspace_check 的 board 默认配置仍指旧 gemini；本次以显式 ynm3000 构建与 check_deskmate 为准。
- doc_check 的既有索引长度/历史未引用节点告警保留，本次不展开文档体系整改。
- 本次 Codex session `01a19ab8-7c3a-77f3-aba1-7ea4224451df` 未找到入仓记录：LOGS MISSING。
  未以其他会话或手写总结替代原始日志；未运行跨工作区当日全量导出。无 commit/push/tag。
- 现存日志扫描 190 文件，16 文件命中凭据/订阅模式；仅记路径与计数，未输出正文、未改历史。
  命中不等同于已确认有效凭据；清单在 `output/p354-local-api/log-scan.json`，未提交或上传。

下一步：刷本镜像后断网点四个宠物按钮、时间与音乐，检查动作/文字/音效；
若失败，先核对实际固件身份，再检查本地执行器日志，不恢复云端确认作为兜底。
