# 9-5 Part3 Devlog — V0.0.7 上板验证 + P135/P133/蓝牙现状评估

> 日期：2026-09-05｜P173~P174

## P173 ⏳ V0.0.7-20260905 上板验证

**烧录**：ok-20260905-29（镜像 md5 c9452fd5）

| 项目 | 结果 | 说明 |
|------|------|------|
| P144 锁屏"人体"乱码 | ✅ 已修复 | cache 1024 + 文本去重生效 |
| P141 AI 语音尾音 | ❌ 仍有 | TTS 播完后仍有残留尾音，根因：`dm_sound_destroy` 直接 `snd_vela_pcm_close` 无 fade-out（dm_tone_play_one 有30ms fade-out 但 XPlayer sink 没有）。需确认尾音类型：爆音（PA 断电）vs 语音片段残留（fade-out 缺失） |
| P142 语音开场音效 | ✅ 已修复 | tone_play_seq(intro+voice) 正常 |
| P143 欢迎语去"主人" | ✅ 已修复 | greet_hello/greet_first 重合成，无"主人" |
| P133 天气注入 | ⏳ review 完成 | dm_weather.c 已落盘 `/data/dm_weather.state`（温度+湿度+中文），dm_ai.c 侧没问题。**需 ai_agent 服务端配置读取该文件** |
| P135 编排听感 | ⏳ 无测试命令 | dm_voice.c 无全天编排模拟命令（纯事件驱动：坐下→场景分类→选 wav→播放）。需上板实测或写一个 UART 测试命令 |
| 蓝牙功能 | ⏳ 不可用 | AGENTS.md 记录"蓝牙 H4 110 挂起（勿擅改驱动）"；ui_bt.c + dm_net.c 代码已完整（扫描/配对/Discoverable），依赖 bluetoothd 服务；驱动层 H4 协议 byte 110 挂起是阻塞项 |

**结论**：V0.0.7 核心 UI + WiFi + 键盘 + UART 调试工具 + 语音开场 + 锁屏乱码均已修复。遗留：P141 尾音 + 蓝牙驱动 + P135 测试命令。

## P174 ✅ TTS 尾音修复（dm_sound_destroy 加30ms 静音+drain）

**问题**：上板验证 P141 尾音仍存在——TTS 播完后有语音片段残留。
**根因**：`dm_sound_destroy`（XPlayer sink 回调）直接 `snd_vela_pcm_close` 关 handle，无 fade-out 无 drain。`dm_tone_play_one`（本地提示音）有30ms fade-out 但 TTS 路径没有。
**修复**：`dm_sound_destroy` 关闭前补30ms 零填充（确保 DAC 输出归零）+ `snd_vela_pcm_drain` + `snd_vela_pcm_close`。
**固化**：ok-20260905-30
