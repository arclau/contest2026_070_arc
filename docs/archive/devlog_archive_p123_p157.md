# DevLog 归档 — P123~P157（2026-09-09 从 devlog.md 节点表移入，减负硬限80行）

> 详情列相对 `project_docs/devlogs/`。

| P | 主题 | 固化 | 详情 |
|----|------|------|------|
| 9-04 P157 | ⏳ **暗光弹窗加"确认门"**：**必须见过真实亮度（lux≥200 确认阈值）后，低亮度读数才可信→暗光检测才启用**；恒 0 坏数据（未就绪/遮光/I2C 挂）永不弹（用户：有确切低亮度才弹，不要一开始就弹）；保留 20s 稳定期双保险 | ok-20260904-27 | 9-4part1 |
| 9-04 P156 | ⏳ **开机误弹窗 + 亮度 0 诊断升级**：弹窗=数据链路全通铁证（驱动推 lux=0 被应用收到才触发暗光弹窗）→ 问题收窄 CH0 读出 0；**开机即弹=驱动首个有效转换前推 0** → dm_als 加 20s 启动稳定期；弹窗 UX 补太阳图标+"光线有点暗"大标题+说明（原"小得不知道干啥"）；defconfig 开 SENSORS_INFO（**上板看 sninfo 的 CH0/CH1/status 原始值定案**）；minidisplay defconfig 对比确认 LTR553 配置两侧相同非根因 | ok-20260904-26 | 9-4part1 |
| 9-04 P155 | ✅ **"亮度 0"真根因（datasheet+Android 同芯片驱动交叉验证）**：**STATUS(0x8C) ALS 数据有效位=bit7(0x80) 低有效（0=有效），P154 误用 bit1（那是 PS 中断位）** → 有效数据被误判丢弃；修：状态有效才读 CH0/CH1（读取会清就绪标志）；**寄存器配置 ALS_CONTR=0x01/MEAS_RATE=0x08 经 datasheet 证实正确非根因**；defconfig 开 DEBUG_SENSORS_ERROR/WARN（下次上板 I2C/激活错误可见） | ok-20260904-25 | 9-4part1 |
| 9-04 P154 | ⏳ **上板"亮度 0"初修（部分错误，被 P155 纠正）**：读失败仍无条件 push 0 → 加 continue+有效位检查，但**有效位用错 bit1**（应 bit7），且漏掉"先查状态再读寄存器" | ok-20260904-24 | 9-4part1 |
| 9-04 P153b | ✅ **P153 全链路审查修复（用户要求提前处理 BUG）**：①**ALS 初始化独立于温湿度**（原 shtc3 订阅失败提前 return 会连带跳过 dm_als_init）；②dm_als_init timer 创建失败时 orb_unsubscribe 防 fd 泄漏；③亮度 label 创建先于首次 refresh（原锁屏首帧无亮度行）；④暗光状态机注释修正（实际=连续暗光只弹一次，恢复后按冷却限频） | ok-20260904-23 | 9-4part1 |
| 9-04 P153 | ⏳ **锁屏亮度显示 + 暗光开灯提醒**：LTR553 ALS 驱动已发布 uORB sensor_light（**现采未订阅**）→ 新增 dm_als.c/h（订阅+1s 轮询+暗光状态机 50lux/5s 防抖/10min 冷却）；锁屏健康卡人体右侧新增「亮度 N」（文本去重防乱码）；**暗光弹窗：有人才弹（LD2410B 无人不打扰）+「知道了」+ 5s 自动消失** | ok-20260904-22 | 9-4part1 |
| 9-04 P152 | ⏳ **状态栏日期上移贴住时钟**：flex gap=0 但 **label 行高（freetype ≈1.4×字号）内嵌上下空白**致视觉间距 → update_layout 实测 date顶-time底 间距 + translate_y 反向抵消，日期 glyph 顶部贴住时间 glyph 底部（两行合成一个时钟块） | ok-20260904-19 | 9-4part1 |
| 9-04 P151 | ⏳ **状态栏时间/日期视觉调整**：时间 48→**56px 专用档 FONT_CLOCK**（独立档不动全局 title 的 26 处引用；283PPI≈5.0mm 达正文可读）+ 右移 DM(8)；日期 30→36px(FONT_ICON) + COL_SEC→COL_TEXT 与时间同深；**两行水平中心对齐**（update_layout 实测宽度 + translate 补偿 (tw-dw)/2） | ok-20260904-18 | 9-4part1 |
| 9-04 P150 | ✅ **状态栏时间/日期「左上角」定位**：**根因链=flex-grow(1) 把 clk_cont 沿主轴(横向)强制拉伸到「左缘~屏幕中点」(lv_flex.c area_set_main_size 硬设宽度) + column 交叉轴 CENTER 把两行横向居中于该大容器**→ 交叉轴改 START 贴容器左缘（高度=TOP_BAR_H 填满已生效） | ok-20260904-17 | 9-4part1 |
| 9-04 P149 | ✅ **WiFi 状态栏图标三态**：未连（含扫描失败）=灰常驻/已连=蓝/关=隐藏；**SSID 判定**（dm_net_wifi_cur_ssid）替代 IP 判定（wlan0 残留 IP 误报）；fa-solid/fa-brands 符号字体 check_res 加校验 | ok-20260904-7~13 | 9-4part1 |
| 9-04 P148 | ✅ **布局调整**：时间/日期入左上角状态栏两行（hero 大时钟移除，对时功能迁移）；锁屏健康卡贴底（删 sp_bot）；双圈不顶状态栏（sp_top 保底 DM24）；内容组居中偏上（grow 3:2）；home 卡片区居中（sp_top DM56）；日期 FONT_CAPTION→FONT_LABEL；图标组 pad DM12；bar pad EDGE_PAD→DM12 | ok-20260904-6/9~11/14 | 9-4part1 |
| 9-04 P147 | ✅ 状态栏「室内」+ 天气卡「室外」：shtc3 传感器 vs open-meteo API 数据源区分 | ok-20260904-5 | 9-4part1 |
| 9-04 P146 | ✅ 音乐曲目联动：**dm_now 解耦曲库**（dm_now_path/title 独立标题源）；外来文件试听播完即停（music_pause 复位 inited）；播放键行为修复；state 路径化 | ok-20260904-2~4 | 9-4part1 |
| 9-04 P145 | ✅ 文件浏览器复制/粘贴：空白长按粘贴栏 + 重名弹窗自动改名 + **后台线程复制进度条**（files_copy_worker+lv_arc）+ MP3 点播走 music_play_path | ok-20260904-1 | 9-4part1 |
| 9-01 P144 | ✅ 锁屏「人体」体字乱码：**glyph cache 仅 256 条目 LRU 淘汰竞态 + 每秒无条件 set_text** → cache 1024 + 文本去重 | ok-20260901-4 | 9-1part1 |
| 9-01 P143 | ✅ 欢迎语去「主人」：重合成 greet_hello/greet_first + cfg 模板同步 | ok-20260901-3 | 9-1part1 |
| 9-01 P142 | ✅ 语音开场背景音效：dm_tone_play_seq(intro+voice) + 4 bg_reward_* + 喝6次水夸奖事件 + tone_test 命令 | ok-20260901-2 | 9-1part1 |
| 9-01 P141 | ✅ AI 语音尾音再修：**P140 只调 DAC 静音/关断顺序，PA 仍在静音前掉电** → 两处 OFF 分支先静音→5ms→关 PA | ok-20260901-1 | 9-1part1 |
| 8-31 P140 | ✅ AI 语音 POP/CLICK 根治：**根因 close 时 DAC 未静音先断输出**（route 关闭顺序反）→ 驱动层先静音→再关 DAC | ok-20260831-4 | 8-31part1 |
| 8-31 P139 | ✅ 站立自动检测（Phase A）：**离座又回来自动记起立**——离开 ≥3s 记一次（防抖，回来重置）；h3 RING 自动刷新 | ok-20260831-3 | 8-31part1 |
| 8-31 P138 | ✅ 时段 bug：**dm_voice localtime_r 取 UTC 小时**（time=UTC）→ 改 gmtime+8 对齐 UI | ok-20260831-2 | 8-31part1 |
| 8-31 P137 | ✅ AI 语音尾音修复：**根因 snd_vela_pcm_close 内部无条件 drop 硬切** → drain 前 30ms fade-out | ok-20260831-1 | 8-31part1 |
| 8-31 P136 | ⏳ P129~P135 上板验证：P129/130/128/131/132 ✅；P133 健康✅ 天气注入未生效（答"天气API没接入"）待修；P134 ✅；P135 听感未验 | — | 8-31part1 |
| 8-28 P135 | ✅ **语音编排**：Phase6 全闭环（dm_voice/欢迎卡/银月化/18 wav 含12合成）+ Step5 未对时退化；**P181 voice_sim 测试命令已补** | ok-20260828-27/28 + ok-20260906-1 | 8-28part1 |
| 8-28 P134 | ✅ AI 称呼用户：USER.md 姓名写入"主人"（memory_store default_user + 模板同步） | ok-20260828-17 | 8-28part1 |
| 8-28 P133 | ✅ AI 本地上下文注入：天气落盘 /data/dm_weather.state + system prompt 注入健康/天气（直接引用不联网）；用户名走 USER.md | ok-20260828-16 | 8-28part1 |
| 8-28 P132 | ✅ PTT 录音 EBUSY 根治：capture state 残留 RUNNING（未 close-drop）→ prepare EBUSY 加 snd_vela_pcm_drop 清状态重试；API 全通额度未耗尽 | ok-20260828-15 | 8-28part1 |
| 8-28 P131 | ✅ AI 语音挂修复：PTT 录音 prepare EBUSY（force_stop 释放声卡异步 vs prepare 跨进程竞争）→ audio_capture_start 加 EBUSY 重试（20×50ms） | ok-20260828-13 | 8-28part1 |
| 8-28 P130 | ✅ UI 文案巡检汉化：状态栏/锁屏日期 Sun/Aug 英文 → 周日 8月28日（luncher_dm 旧窗口已弃用不显示；Wi-Fi/TF/GB/°C/Vela Pad 专名保留） | ok-20260828-12 | 8-28part1 |
| 8-28 P129 | ✅ 锁屏健康卡收尾：人体距离字号统一 FONT_BODY 24px（宽 210→260）+ WATER→喝水 / STAND→起立 汉化（初始+refresh） | ok-20260828-11 | 8-28part1 |
| 8-28 P128 | ✅ 锁屏人体距离显示闭环：P127 补 dist≥0 路径 set_text + 去 CLIP（P126"人体"乱码嫌疑，全项目唯一 CLIP 用家）+ 清 [prox] 诊断日志，全流程固化 | ok-20260828-10 | 8-28part1 |
| 8-28 P126 | ✅ 锁屏人体距离修复：关 LOG/整数拼接/flex 宽缓存 WRAP→显式宽+CLIP | ok-20260828-1~9 | 8-28part1 |
| 8-27 P125 | ✅ 锁屏人体距离显示 + 在座阈值定稿：1.5m(2门)配置化、离开 30s 只暂停、删 prox_seat_cm 死配置 | ok-20260827-26/27 | 8-27part4 |
| 8-27 P124 | ✅ UART 分工定稿：调试工具迁回 UART1（BSN20 外设侧），LD2410B 迁 UART3（/dev/uart3，恢复 init） | ok-20260827-25 | 8-27part3 |
| 8-27 P123 | ✅ 改 **UART3（PD10/11 mux5）**：SPI 未启用无冲突，工具换到 /dev/uart3；**上板验证 LOOP OK + PINLOOP OK 闭环** | ok-20260827-24 | 8-27part3 |
