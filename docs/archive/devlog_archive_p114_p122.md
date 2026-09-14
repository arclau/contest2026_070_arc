# DevLog 归档 — P114~P122续（8-23 part1/2 + 8-26/8-27 part1/2/3）

> 从 `devlog.md` 节点表归档（2026-09-09 节点表超 80 行硬限，最早 21 行移此）。
> 不再日常参考。需要时按 详情列 读子文档。

| P | 主题（根因/结论加粗） | 固化 | 详情 |
|----|----------------------|------|------|
| 8-27 P122 续 | ✅ UART1 外部 rx=0 根因：drv_gpio IO 扩展器覆盖 UART1 mux4 → 移除 maps[] PD21/22 注册 | ok-20260827-23 | 8-27part3 |
| 8-27 P122 | ✅ UART1 外部 115200 接 rx=0：LOOP OK≠引脚通路；mux4/方向正确 → 加 pinloop 外部回环诊断 | ok-20260827-22 | 8-27part3 |
| 8-27 P120/121 | ✅ 1500000 档 FAIL 收尾：apb1=24M（0x524 锁 OSC24M），dl=1 极限 → 移除档回归 115200 | ok-20260827-20/21 | 8-27part3 |
| 8-27 P119 | ✅ **UART 调试工具**：Settings 串口 log 子 APP（工具栏/3 配色/子页不锁屏）；**UART0 与 TF 卡共用 PF2/PF4——P118 系误判，全链路禁用 UART0、换口 UART1=PD21/22 mux4**；**回环 LOOP OK 证 UART1 全通**；🔴 **1500000 档 FAIL 待挖**；无传感器不欢迎/不计时 | ok-20260827-6~19 | 8-27part1/2 |
| 8-26 P118 | ⏳ **LD2410B 上板排障**：路径改 /dev/uart0 + 协议配置下发 + 波特率 USB-TTL 改 115200 + 零字节排查（**驱动引脚 GPIOF2/4+mux3 已证正确，board.h 注释过时**，待查供电/接线） | ok-20260826-1~5 | 8-26part1 |
| 8-23 P114 | **天气 2h 周期 + 去 UI 时延**：开机同步一次每 2h 更新，w_debug_lbl 移除 | ok-20260823-1 | 8-23part1 |
| 8-23 P115 | **AI 语音本地命令**：音量/亮度 ±5%（agent 短路 LLM→cmd 事件→UI 执行+TTS） | ok-20260823-2 | 8-23part1 |
| 8-23 P116 | **语音命令扩展**：音乐播放/暂停/上一曲/下一曲 + 音量亮度绝对设置（中文数字） | ok-20260823-3 | 8-23part1 |
| 8-23 review-4 | **审查修复**：cn_num 中文数字解析越界读防护（strlen 边界检查） | ok-20260823-4 | 8-23part2 |
| 8-23 review-5 | **上板"播放"AI 不响应**：音乐命令 TTS 确认与 XPlayer 抢声卡 EBUSY → 不播确认 | ok-20260823-5 | 8-23part2 |
| 8-23 review-6 | **语音命令表驱动重构**：agent 匹配器数组 + UI 命令表（needs_ack 属性固化） | ok-20260823-6 | 8-23part2 |
| 8-23 review-7 | **锁屏语音命令不生效**：cmd 订阅/消费常驻化（移 deskmate_ui_create，退子页不停订阅） | ok-20260823-7 | 8-23part2 |
| 8-23 review-8 | **串口日志降噪**：volc_asr/codec dapm/writei/awplayer WARNING 四处关闭 | ok-20260823-8 | 8-23part2 |
| 8-23 review-9 | **死代码清理**：归档 .bak + 删 music_service/service 空壳/ui_manager + 清编译产物 | ok-20260823-9 | 8-23part2 |
| 8-23 review-10 | **音频仲裁层**：停音乐→释放→播→恢复，根治抢声卡 EBUSY（P103/P107/review-5） | ok-20260823-10 | 8-23part3 |
| 8-23 review-11 | **死机修复**：force_stop 后 XPlayerStart(STOPPED 态) 崩 → cleared 标志禁止 Start，恢复重建播放器 | ok-20260823-11 | 8-23part3 |
| 8-23 review-12 | **生命周期审计**：14 处 XPlayer 调用点全核，仅 Start 会崩；pause/position/duration 加 cleared 防护 | ok-20260823-12 | 8-23part3 |
| 8-23 review-13 | **UI 汉化收尾**：蓝牙子页标题/灯光/触摸窗口/HOME 无曲目 fallback 残留清零 | ok-20260823-14 | 8-23part4 |
| 8-23 review-14 | **关于区精简**：删构建号/序列号，留 设备名称/软件版本/型号/软件更新 | ok-20260823-15 | 8-23part4 |
| 8-23 P117 | **LD2410B 雷达替代 LTR553**：UART0 直读+帧解析，有人&≤2m 判在座（dm_health 不改） | ok-20260823-16 | 8-23part5 |
| 8-23 review-15 | **LD2410B 审查修复**：忙转→poll/无帧回退/2s 过期/LTR553 订阅清理/锁屏人体显示 | ok-20260823-17 | 8-23part5 |
