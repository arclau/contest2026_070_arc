# DevLog 2026-08-28 — 锁屏人体距离显示修复全链路（P126）

## 背景

P125（8-27）上板复测：LD2410B 接 UART3（PD11=RX）数据正常（锁屏距离显示 + 在座判定 + 欢迎语），但暴露两个问题：
1. **LD2410B 距离 LOG 刷屏**（`state=%d dist=%dcm` 状态变化打印 + 5s `rx=%uB frames=%u` 统计）
2. **锁屏右下角"人体"只显示"有人"不显示距离** → 用户要求显示距离

本会话共固化 ok-20260828-1~9（9 个稳点），最终数据链路全通但**"人体"二字渲染乱码遗留**。

## 排障记录（按「现象→根因→方案→验证」）

### 1. 关刷屏 LOG（ok-20260828-1）
- **现象**：state 2↔3（静止↔运动&静止）抖动触发状态变化打印每帧刷屏 + 5s rx 统计
- **方案**：删 `ld2410b_parse_frame` 状态打印、`ld2410b_reader` 5s rx 打印；清理调试变量（`g_ld2410b_log_state`/`g_ld2410b_rx_bytes`/`g_ld2410b_frame_cnt`/`last_report_ts`）；保留 `init ok`/`cfg sent` 一次性日志
- **验证**：串口不再刷 ld2410b 日志

### 2. dist=0 也显示距离（ok-20260828-1）
- **现象**：日志大量 `dist=0cm` 帧，锁屏退化成"人体：有人"
- **根因**：dist=0 = 目标在最近距离门（<0.75m）内的真实值（协议上报 0），原 `dist > 0` 判断丢弃
- **方案**：ui_home.c `dist > 0` → `dist >= 0`

### 3. %.1f → %d.%d 整数拼接（ok-20260828-2）
- **现象**：锁屏仍无距离数字
- **根因**：LVGL 用 **builtin lv_snprintf**（lv_conf_kconfig.h 默认 `LV_STDLIB_BUILTIN`），`%f` 支持由 `LV_USE_FLOAT` 控制（lv_sprintf_builtin.c:41 `PRINTF_DISABLE_SUPPORT_FLOAT = !LV_USE_FLOAT`）；本项目 **`CONFIG_LV_USE_FLOAT is not set`**（defconfig:4515）→ `%.1f` 输出为空。项目温度/湿度显示全用 `%d.%d` 整数拼接（luncher_dm.c:706/763/791）即此惯例
- **方案**：`lv_snprintf(buf, ..., "人体：有人 %d.%dm", dist/100, (dist%100)/10)`
- **验证**：`strings vela.bin` 见 `%d.%dm`

### 4. WiFi 0x15 插曲（ok-20260828-4）
- **现象**：用户烧录后 `download_fw FAIL 0x15`（老毛病）
- **排查**：check_res.sh 通过（res.fex 固件/字体/12 提示音路径+字节全对）；解析镜像分区表（sys_partition.fex：res=51200 扇，下载 res.fex）确认 res 落位与 V0.0.5 一致
- **结果**：重打包后用户烧录 WiFi 正常——镜像/打包侧无问题，疑似烧录时效

### 5. 诊断 [prox] refresh（ok-20260828-5）
- **现象**：仍"人体-"无内容
- **方案**：standby refresh 加 `[prox]` 每秒日志（lbl/p/dist/seated）
- **验证**：**数据链路全通**——`lbl=1 p=1 dist=48~57 seated=1` 每秒刷新正常

### 6. %s 打印致死机（ok-20260828-6→7）
- **现象**：加 `[prox] set ... txt='%s'`（lv_label_get_text）后**死机**（WiFi init 后无输出）
- **根因**：`lv_label_get_text()` 指针 + `%s` 打印（该版唯一改动；用户上一版 ok-20260828-5 正常）
- **方案**：移除 `%s` 打印，保留 `%d`（ok-20260828-7）
- **验证**：恢复正常（WiFi 连 wifi-home 成功）

### 7. 坐标诊断：lv_obj_get_x/y 是相对父坐标（ok-20260828-8）
- **现象**：lbl y=91 一度误判"label 在屏幕顶部"
- **根因**：`lv_obj_get_x/y` 返回**相对父对象**坐标——y=91 是 label 在 health 卡内垂直居中（卡高 230、内容区居中 ≈91.5）
- **方案**：打印 health 卡（父=overlay=屏幕）坐标
- **验证**：`hp x=0 y=1030 w=1920 h=230`（卡在屏幕底部，**底部 60px 超屏 1200**）；`lbl w=52 h=19`

### 8. flex 缓存宽度 WRAP 截断根治（ok-20260828-9）
- **根因**：**lbl w=52 = 初始文本"人体：--"的宽度**——LVGL flex 布局缓存 label 初始宽度，文本更新为"人体：有人 0.5m"（需 ~110px）后**不重排** → 52px 内 WRAP 换行，label 高只够一行 → **只显示"人体："，"有人 0.5m"被裁**（用户见"人体-"无内容，与此完全吻合）
- **方案**：`lv_obj_set_width(standby_prox_lbl, 210)`（≥ 最长"人体：有人 2.2m"）+ `lv_label_set_long_mode(..., LV_LABEL_LONG_CLIP)`
- **验证**：`fmtlen=20`（文本完整"人体：有人 0.5m"）、`lbl w=210 h=19`（宽度修复生效）、`hp x=0 y=1030`（右下角）

### 9. ⚠️ 遗留：锁屏右下角"人体"二字渲染乱码（用户最后反馈）
- **现象**：宽度修复后（fmtlen=20 文本完整、位置右下角），**"人体"两个字显示乱码**
- **状态**：数据链路已全通（文本/尺寸/位置均正确），乱码为**渲染层新现象**——疑 FONT_CAPTION（MiSans 14px）固定宽度 + CLIP 下字形渲染异常，或 UTF-8/字形缓存问题
- **处理**：用户放弃本次调试，写 devlog 换会话

## 改动文件表

| 文件 | 改动 |
|------|------|
| `dm_ld2410b.c` | 删刷屏 LOG（state/dist 变化 + 5s rx 统计）+ 调试变量清理 |
| `ui_home.c` | dist>=0 显示距离；%.1f→%d.%d 整数拼接；`[prox]` 诊断日志（refresh/fmtlen）；prox label 显式宽 210 + LV_LABEL_LONG_CLIP |

## 验证命令与产物

```bash
cd /data/dm && ./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh -j$(nproc)
cd vendor/allwinnertech/lichee && source envsetup.sh && lunch_nuttx 2 && pack
bash /data/vela/check_res.sh        # 通过
bash /data/vela/git_snapshot.sh     # ok-20260828-1~9
```

上板日志关键行：
```
ld2410b_apply_config: [ld2410b] cfg sent: maxgate=3 nodelay=2s
dm_ld2410b_init: [ld2410b] init ok (/dev/uart3 @115200)
[prox] refresh lbl=1 p=1 dist=78 seated=0
[prox] fmtlen=20 lbl w=210 h=19 | hp x=0 y=1030
```
（p=1 dist 32~78cm 每秒刷新；fmtlen=20 文本完整；w=210 修复生效；hp y=1030 屏幕底部）

## 遗留事项

- 🔴 **锁屏右下角"人体"二字乱码**（渲染层；数据链路已全通：文本/尺寸/位置均正确）——下会话优先处理
- `[prox]` 诊断日志（refresh/fmtlen 每秒 2 条）待显示确认后清理
- health 卡底部超屏 60px（y=1030+230=1260 > 1200，布局偏高）可顺手优化
- h1/h2/h3（在座/Water/Stand）未确认是否也有 flex 宽度缓存问题（用户未反馈异常）

*DevLog by AtomCode (deepseek-v4-flash)*

---

# DevLog 2026-08-28（part1 续）— 锁屏人体距离收尾 + 健康卡汉化 + UI 巡检 + AI 语音链路修复 + 本地上下文（P128~P134）

> 承接上文（P126/P127 锁屏人体显示）。本段覆盖 ok-20260828-10~17（8 个稳点）。
> **下一 part（新会话）主题：语音本地化**——本地编译机多录音频进 nand，替代云端 API 省额度。

## 节点总览

| P | 主题 | 固化 |
|---|------|------|
| P128 | 锁屏人体距离显示闭环：补 set_text + 去 CLIP（乱码嫌疑，全项目唯一 CLIP 用家）+ 清 [prox] 诊断日志 | ok-20260828-10 |
| P129 | 锁屏健康卡收尾：人体字号 FONT_BODY 24px（宽 210→260）+ WATER→喝水 / STAND→起立 汉化 | ok-20260828-11 |
| P130 | UI 文案巡检汉化：状态栏/锁屏日期 Sun/Aug → 周日 8月28日 | ok-20260828-12 |
| P131 | AI 语音挂修复①：PTT 录音 prepare EBUSY → 重试 20×50ms | ok-20260828-13 |
| P132 | AI 语音挂修复②（根治）：capture state 残留 RUNNING → prepare EBUSY 时 snd_vela_pcm_drop 清状态重试 | ok-20260828-15 |
| P133 | AI 本地上下文注入：天气落盘 /data/dm_weather.state + system prompt 注入健康/天气（直接引用不联网） | ok-20260828-16 |
| P134 | AI 称呼用户：USER.md 姓名=主人（default_user + 模板） | ok-20260828-17 |

## 排障记录

### 1. P126 遗留"人体"乱码 → P128 去 CLIP 根治（ok-20260828-10）
- **根因**：standby_prox_lbl 是全项目**唯一**用 `LV_LABEL_LONG_CLIP` 的 label（其余全 WRAP/DOT），CLIP 对 FreeType 位图字形（MiSans 14px）渲染有裁切风险
- **修复**：去掉 CLIP，保留显式宽 210（WRAP 下 210px 足够容纳最长文本"人体：有人 2.2m"）；同时 P127 补 dist≥0 路径漏写的 `lv_label_set_text`（ui_home.c:942）
- **验证**：用户上板确认"距离已经显示了"

### 2. P129 健康卡字号统一 + 汉化（ok-20260828-11）
- 用户反馈人体字太小 → FONT_CAPTION(14px) → **FONT_BODY(24px)** 与在座/喝水/起立一致；宽度 210→260（24px 下最长文本≈224px）
- 锁屏 WATER→喝水、STAND→起立（初始文本 + 每秒 refresh 两处，用 LV_SYMBOL_TINT/UP）

### 3. P130 全仓 UI 文案巡检（ok-20260828-12）
- 唯一确凿 UI 可见英文：状态栏/锁屏日期 `en_weekdays/en_months`（"Sun  Aug 28"）→ 汉化"周日 8月28日"（cn_weekdays/cn_months，`%s %s%d日`）
- 已排查保留：Wi-Fi/TF/GB/°C/cm/Gemini-s1/Vela Pad/.txt/X（关闭符号）、Books/Music 等 show_subpage 调度 key
- 不显示未动：luncher_dm 旧窗口（create_main_screen 已被 deskmate_ui_create 替代，`luncher_dm.c:1388`）、LED 颜色名（无 UI 调用）、dm_weather 月份表（HTTP Date 解析）

### 4. WiFi 0x27 + "人体"乱码同源 = res 分区未烧全（烧录环节，非代码）
- 日志：`download_fw FAIL status=0x27` + `rtl8723f_hal_init Download Firmware from file failed` → 板端 /resource/etc/wifi/FW_NIC_BCUT.bin 缺失；字体同理（res 同分区）
- 镜像侧 check_res.sh 通过（FW 固件/字体/12 提示音字节全对）→ **镜像无问题，烧录工具跳过 res 分区**
- 处理：整包烧录 + 板端验证 `ls /resource/`；重打包 ok-20260828-14 确认镜像

### 5. AI 语音"挂" → P131/P132 两层修复（ok-20260828-13/15）
- **现象**：PTT 录音 `[audio_cap] prepare fail -16`（EBUSY）
- **P131**：怀疑与音乐 force_stop（XPlayerStop 异步收尾）跨进程竞争 → prepare EBUSY 重试 20×50ms——**上板无效**
- **关键排查**：
  - ai_agent build 时间戳 `2026-08-27 13:16:16` 曾误导"修复没进固件"——实为 agent_main.c 未重编（build 时间戳在该文件），audio_capture.o 已 08-28 重编（P131 在内）
  - API 验证（用户跑 `--test`）：LLM ✅ / TTS ✅（合成+播放全通）→ **API/额度完全正常**，playback 声卡也正常；`--test asr` 仅缺测试文件（/data/mic.wav）
  - 驱动源码（snd_pcm.c:2134-2140）：prepare EBUSY = `snd_pcm_running(substream)` = **substream state 残留 RUNNING**（上次会话异常退出/PTT 中断未正常 close-drop）；dai 层打印未出现 → EBUSY 确定在 PCM 层
- **P132 根治**：prepare EBUSY 时先 `snd_vela_pcm_drop()`（stop+复位 SETUP，与驱动 close 同语义，pcm.c:850 可用）再重试——**上板录音 OK 确认**
- 教训：跨进程声卡竞争"纯等待"重试无效，需主动 drop 清驱动残留状态

### 6. P133 本地上下文注入（ok-20260828-16）
- 用户需求：AI 应知道"坐多久 / 我叫什么 / 天气"，不要联网搜
- **健康**：ai_agent 读 /data/dm_health.state（luncher_dm 落盘，格式 `日期 喝水 起立 在座秒`）→ system prompt 注入"在座 X 分钟/喝水 X 次/起立 X 次"
- **天气**：luncher_dm dm_weather.c 新增 `weather_state_save()`（fetch 成功写 /data/dm_weather.state，格式 `温度 湿度 中文描述`）→ ai_agent 读注入"晴，28°C，湿度 85%"——直接引用本地，system prompt 明令禁止联网搜天气
- **名字**：USER.md（/data/ai_agent/config/USER.md）已有 User Info 注入机制（context_builder.c:157）；用户拍板称呼"主人"

### 7. P134 用户名写入（ok-20260828-17）
- memory_store.c 的 `default_user`（ensure_file 生成源）加"姓名：主人" + agent_skills/templates/USER.md 同步
- ⚠️ ensure_file 不覆盖已存在文件：板端旧 USER.md 需 `rm /data/ai_agent/config/USER.md` 后重启 ai_agent 生效

## 改动文件表

| 文件 | 改动 |
|------|------|
| `ui_home.c` | P127 set_text 补全；去 CLIP；prox 字号 FONT_BODY 24px/宽 260；WATER→喝水 / STAND→起立；日期汉化 cn_weekdays/cn_months |
| `audio_capture.c`（ai_agent） | P131 prepare EBUSY 重试 20×50ms；P132 EBUSY 时 snd_vela_pcm_drop 清残留再重试 |
| `dm_weather.c`（luncher_dm） | P133 weather_state_save() 落盘 /data/dm_weather.state（fetch 成功写 温度 湿度 描述） |
| `context_builder.c`（ai_agent） | P133 system prompt 注入"本地健康/本地天气"权威数据段（禁止编造/联网） |
| `memory_store.c`（ai_agent） | P134 default_user 加"姓名：主人" |
| `agent_skills/templates/USER.md` | 姓名同步"主人" |

## 验证命令与产物

```bash
cd /data/dm && ./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh -j$(nproc)
cd vendor/allwinnertech/lichee && source envsetup.sh && lunch_nuttx 2 && pack
bash /data/vela/check_res.sh        # 每次均通过
bash /data/vela/git_snapshot.sh     # ok-20260828-10~17
```

- 产物一致：8127176B（P128~P132）→ 8131272B（P133/P134 增加 system prompt 注入段后）
- 上板确认：PTT 录音 OK（`[audio_cap] capture started`）、`--test llm/tts` 全通（额度未耗尽）

## 遗留 / 下一 part 预告（语音本地化）

- 🔴 **下一 part（语音方面）**：用户明确——**不希望浪费 API**（本地+上板测试消耗免费额度），**nand 有空间**，计划**本地编译机多录一些音频**，支持本地各种响应（本地提示音/本地语音替代云端 API）
  - 现有基础：12 个提示音（/resource/tones/，PC 端 tts_proto 合成）已验证可播；dm_tone_play 直读 wav 不占 XPlayer
  - 可扩展：欢迎语/健康提醒/天气播报等更多本地音频；涉及 res 分区扩容、ai_agent 本地音频播放路径、与 TTS 播报仲裁（dm_audio_fg_acquire 已有）
- 板端旧 USER.md 需清理（见 P134 注意）
- Files/Books/Games 回归、蓝牙 H4 110 挂起（勿擅改驱动）

---

# DevLog 2026-08-28（part1 续二）— 语音编排启动：难度评估 + VOICEDESIGN.md 指导流程（P135）

> 用户提供 `voicedesight.txt`（ChatGPT 声音交互分析：5 项交付物 + Voice Director 架构方向）。
> 用户拍板：**先评估难度、分步骤按逻辑实现**；新建 VOICEDESIGN.md 作为 AGENTS.md 下级指导文件，承载整个流程（应对 512K 会话限制，每 Phase 固化+devlog 续接）。

## 结论摘要（难度评估）

- **设计阶段（Phase 1~4）难度低-中**：5 项交付物全是文档（交互设计/资产规划/场景状态机/Director 逻辑/台词），零代码风险
- **实施阶段（Phase 6）难度中-高**：核心 = Context 扩展（dm_health.state 字段升级，需向后兼容）+ 加权随机决策引擎 + UI 联动；音频链路零改动（dm_tone_play 保持，Director 只做决策）
- **资源落地依赖外部**：wav 需 PC 端 tts_proto 合成（已有工作流），AI 不能直接生成音频；第一批 ≤30 个，先查 res 分区容量
- **关键风险**：g_tone_busy 串行丢弃（播中丢新请求）、res 空间、上板验证每轮 5~10min 贵

## 本阶段产出（Phase 0）

| 产物 | 说明 |
|------|------|
| `/data/dm/VOICEDESIGN.md` | 指导文件：7 阶段路线图 + 会话续接协议 + 硬约束 7 条 + 决策引擎要点 + 进度表（AGENTS.md 读取协议 ② 已加引用） |
| devlog.md 节点表 | 新增 P135 行（⏳ 规划中） |
| 素材包 | 12 个 wav 清单（时长/文案/触发场景）+ 播放机制 + 调度逻辑 + 约束，已在会话内整理 |

## 下一步（Phase 1，下一会话）

产出 `VOICE_INTERACTION_DESIGN.md`（project_docs/specs/）：①保留/重录判断 ②坐下场景细分 ③该说/该闭嘴规则 ④防重复策略 ⑤情绪体系 ⑥上下文利用 ⑦资源库设计 ⑧句长分层 ⑨加权随机+历史+冷却 ⑩陪伴感原则——重点是把 ChatGPT 分析落实为**可判定参数**（阈值/冷却/次数全走 dm_health.cfg）

---

# DevLog 2026-08-28（part1 续三）— P135 推进：人设定稿银月 + Phase 1 交互设计文档完成

> 用户拍板三项：①银月人设**全出口统一**（本地语音+弹窗+AI 对话 system prompt）②**全清牛马梗**（弹窗/欢迎语/产品名"牛马健康助理"→建议"银月健康助理"待评审）③甜:贱≈**3:7**（贱多一点）。

## 产出

| 产物 | 说明 |
|------|------|
| `project_docs/specs/VOICE_INTERACTION_DESIGN.md`（270 行/10 节） | §一人设银月 · §三架构蓝图 · §四坐下 12 场景状态机（判定参数全 cfg 可配）· §五闭嘴检查链 7 条+防重复选择流程+7 级情绪体系+句长分层 · §六银月台词（粒子5/回来6/吐槽4/关心4/欢迎2/提醒重录+弹窗 UI 文案表+AI prompt 银月化要点）· §七旧 12 wav 全重录处理 · §八 UI 联动（欢迎回来卡走 UI 承载动态数据）· §九 Director 伪代码（dm_voice.c 落点建议）· §十评审顺序 |
| VOICEDESIGN.md 进度表 | Phase 1 完成 ✅，下一步 Phase 2 |
| devlog.md P135 行 | 更新为"Phase 0+人设+Phase 1 完成" |

## 关键设计决策（评审时请重点看）

1. **动态信息不进语音**：预合成 wav 无法插词（主人/天气/坐姿分钟）→ 动态信息走"欢迎回来卡"UI，语音只播预合成通配句；避免穷举组合录音成本爆炸
2. **闭嘴检查链**：AI 对话后 5min / 音乐中 / 距上次语音 10min / 今日完整语音 12 条软上限 / 吐槽配额 4 次日 / 深夜不完整句——7 条全可配
3. **提醒不无限升级**：被拒 3 次后进入"沉默+UI 持续提示"，只说一次"行。"
4. **greet_morning~night 降级**：从"时段必播"降为"回来池候选"，时段只影响情绪权重

## 下一步（Phase 2）

产出 `VOICE_ASSET_PLAN.md`：第一批录音清单（粒子 5 + 回来 6 + 吐槽 4 + 关心 4 + 欢迎 2 + 提醒 6 ≈ 27 个新增 + 旧 12 个重录），每项含 文件名/台词/场景/情绪/时长/优先级/cooldown/多版本；重录走 PC 端 tts_proto 工作流

---

# DevLog 2026-08-28（part1 续四）— P135 Phase 2 完成：VOICE_ASSET_PLAN.md 资产规划

> 用户拍板："就这样了，先开搞，有问题了再修改"——设计文档（Phase 1）不逐句评审，直接进入资产规划。

## 产出

| 产物 | 说明 |
|------|------|
| `project_docs/specs/VOICE_ASSET_PLAN.md`（约 110 行/5 节） | §一总览（20 新增+12 重录=32 任务）· §二新增清单（粒子 5/回来 6/吐槽 4/关心 4/欢迎 1，全字段：台词/时长/优先级 P0~P1/cooldown/重复/多版本）· §三重录清单（文件名不变只换内容，greet_morning~night 降级回来池）· §四容量（新增≈32s≈1.55MB 净增；res 需 ≥1.6MB 余量，实施前查 sys_partition.fex）· §五 tts_proto 合成工作流 |
| VOICEDESIGN.md 进度表 | Phase 2 完成 ✅，下一步=评审台词 → Phase 3/4 |
| devlog.md P135 行 | 更新为"Phase 0~2 全完成" |

## 关键约定（后续实施按此）

1. **新增 20 个用新文件名**（tiny_*/back_*/tease_*/care_*/greet_hello），**重录 12 个保持原名**（greet_*/popup_*）——代码零改动即可替换音频，Director 候选表只加新文件
2. cooldown 约定：粒子 5min / 短句 30min / 提醒 15min / 欢迎场景限定
3. 优先级：P0=回来/提醒/欢迎 15 个（第一批必须），P1=粒子/增强 5 个（可第二批，容量紧张先砍）
4. 弹窗 UI 文案（6.6 表）随 Phase 6 代码改，与 wav 重录同步

## 下一步（Phase 3/4 或直接合成）

- 选项 A：**先合成重录**（用户拿资产表去 tts_proto 跑，回填 wav 到 res/tones）——音频先行，代码后接
- 选项 B：**先 Phase 3/4**（坐下状态机伪代码 + Director 逻辑细化）——代码设计先行，音频跟上
- 两个都可独立推进；建议 A（音频耗时，先跑起来）

---

# DevLog 2026-08-28（part1 续五）— P135 用户评审 9 点修正（不推翻，修正并入）

> 用户已看 Phase 1/2 设计，方向认可，进入下一阶段前提出 9 点修正。**全部在不推翻现有文档的前提下修正并入。**

## 修正落实对照表

| # | 用户要求 | 修正落点 |
|---|----------|----------|
| 1 | 银月=语感/人格参考，不过度二次元；核心人格=灵动/聪明/俏皮/贱兮兮/有分寸/偶尔温柔；"贱"=自然对话感非卖萌 | VOICE_INTERACTION_DESIGN.md §二 重写（含分寸/粒子定位行） |
| 2 | 旧 12 wav 不全部重录，四类分类（保留直接/保留降级/建议重录/必须重录）+ 具体原因；先解决编排非数量 | VOICE_ASSET_PLAN.md §三：保留 5 / 保留降级 1（popup_sit_3）/ 建议重录 0 / 必须重录 6（全含牛马梗） |
| 3 | 动态数据不进语音改 V1/V2 策略（V1 UI 为主；V2 少量高价值字段；禁止组合爆炸） | 设计文档 §5.5 新增 V1/V2 策略节 |
| 4 | 每日 12 条改可配置 voice budget / category budget（greeting/health_reminder/teasing/caring/system_response），12 仅初始测试值 | 设计文档 §5.1.2 新增 budget 表 |
| 5 | 音乐中改"降低交互等级"非绝对禁止：普通→静默、高优健康提醒→允许抢占（dm_audio_fg_acquire）、PTT→正常、紧急→正常 | 设计文档 §5.1.1 新增交互等级表 |
| 6 | AI 对话后 5min 仅默认 cooldown 参数（recent_ai_interaction_cooldown=300s），全进 cfg | 设计文档 §5.1 表 cfg 化 |
| 7 | 粒子=调味料，不成为主要交互；优先级 静默>短句>完整句>少量粒子（防 NPC 感） | 设计文档 §二 沉默人格行 + §5.4 粒子行低频化 |
| 8 | 新增 Daily Experience / 一日体验剧本章节（早→深夜完整时间线） | 设计文档 §八.5 新增：16 事件时间线（6 次说话/10 次闭嘴） |
| 9 | 资源最小闭环：最少资源验证 防重复/cooldown/silence/context/personality/UI 联动 | VOICE_ASSET_PLAN.md §六 新增：最小闭环 18 个 + 验证矩阵 + 通过/失败标准 |

## 产出

- `VOICE_INTERACTION_DESIGN.md`：§二/§五（5.1/5.1.1/5.1.2）/§5.4/§5.5 修正 + §八.5 一日体验剧本
- `VOICE_ASSET_PLAN.md`：§一 分类总览 + §二 闭环必需子集标注 + §三 四类分类表 + §六 资源最小闭环
- VOICEDESIGN.md 进度表 / devlog.md P135 行同步更新

## 下一步

- Phase 3/4：状态机细化 + Director 逻辑设计（伪代码级别）
- 或先合成**最小闭环 18 个**（保留 6 + 重录 6 + 新增 6），上板验证矩阵后再增量

---

# DevLog 2026-08-28（part1 续六）— P135 Phase 3 完成：VOICE_DIRECTOR_SPEC.md 实现规格

> 用户"继续搞"→ 进入 Phase 3/4，产出 Phase 6 实施的**直接输入规格**（纯设计，不动代码）。

## 产出

| 产物 | 说明 |
|------|------|
| `project_docs/specs/VOICE_DIRECTOR_SPEC.md`（272 行/8 节） | §一 模块结构（dm_voice.c/h，无 LVGL 依赖可单测）· §二 对外接口（6 函数签名 + voice_decision_t/category 定稿）· §三 voice_ctx_t 定稿（含 budget[5] + recent_ids[8]）· §四 cfg 参数全表（S 场景 5/Q 静默 5/B budget 5/C cooldown 3/W 时段权重 6 组）· §五 决策伪代码（on_seated/decide/classify_scene/pass_silence_checks/weighted_select 5 函数）· §六 FSM 细化（g_away_s/g_sit_cnt_today/g_decline_cnt 新增统计 + state 8 字段升级向后兼容）· §七 集成点清单（7 处接线）· §八 实施小步（5 步，每步编译打包固化） |
| VOICEDESIGN.md 进度表 / devlog.md P135 行 | Phase 3 完成 ✅ |

## 关键实现决策（Phase 6 按此执行）

1. **dm_voice 纯决策层**：不碰 dm_tone_play/dm_tone_worker/dm_audio_fg/PTT 打断（VOICEDESIGN.md §四铁律）
2. **提醒弹窗保持直通**：popup_* 现链路不动，只加"音乐中 L2+ 抢占"判断（dm_audio_fg_acquire 已具备）
3. **state 格式升级向后兼容**：旧文件 fscanf 4 字段后 EOF → 新增 4 字段=0，不炸旧数据
4. **Step 1~4 用现有 wav 可先跑通机制**（最小闭环第 0 版），Step 5 才依赖 tts_proto 合成回填

## 下一步（Phase 6 实施，5 小步）

- Step 1：数据结构 + cfg 参数 + state 升级（dm_voice.h/dm_health.c/dm_health_cfg.c）
- Step 2：决策引擎纯函数（dm_voice.c，可加 uartdbg 式自测）
- Step 3：接线（standby_health_cb→on_seated、提醒抢占、AI/音乐时间戳）
- Step 4：UI 欢迎回来卡 + 弹窗文案银月化
- Step 5：AI prompt 银月化 + wav 资源落地（依赖用户合成回填）
- 每步：编译 → 打包 → check_res.sh → git_snapshot

---

# DevLog 2026-08-28（part1 续七）— P135 Phase 6 全闭环：Step 5 完成（AI prompt 银月化 + 12 wav 合成落地）

> 用户："Step 5 还有多少 step 来着全跑完再说。你跑就完事了。"——一次性跑完 Step 5 全部剩余工作。

## Step 5 完成内容

| 项 | 结果 |
|----|------|
| AI system prompt 银月化 | context_builder.c 角色段：openvela/打工牛马健康助理 → **银月**（灵动/俏皮/贱兮兮/有分寸，称呼主人，静默也是陪伴）；禁牛马梗完成 |
| wav 合成 | **复刻 ai_agent volc_tts.c 的 v3 unidirectional 接口**（x-api-key + X-Api-Resource-Id: seed-tts-2.0 + req_params.speaker）写 /tmp/tts_synth.py；12 个 wav 全部合成成功（1ch 24kHz 16bit，粒子 0.6s~欢迎句 3.8s） |
| 资源落地 | 重录 6（greet_first/morning/noon/night + popup_water_2/3）+ 新增 6（back_01/02、tease_01、care_01、greet_hello、tiny_01）回填 res/tones/ → **18 个 wav** |
| check_res.sh | "期望 12"→"期望 18"（脚本维护，/data/vela/） |
| 编译打包固化 | vela.bin 8135368B 一致，check_res 通过（提示音 18/18 True），**ok-20260828-27** |

## 踩坑记录

1. **火山 TTS 401**：初用 /api/v1/tts + Bearer 鉴权 → 401。实锤：**v3 unidirectional（/api/v3/tts/unidirectional）+ x-api-key + X-Api-Resource-Id: seed-tts-2.0**，响应为逐行 JSON（code=0/20000000，data=base64 PCM，需拼装 + 写 wav 头）
2. **ai_agent 是独立 PROG**（MODULE=CONFIG_EXAMPLES_AI_AGENT_VELA）：编译进 apps 不编入 vela.bin，strings vela.bin 找不到银月 prompt 属正常；.o 时间戳验证已重编
3. **音色**：沿用 P103 同款 `zh_male_sophie_uranus_bigtts`（男声，已验证 2.0 音色）；**银月理想是女声**——待用户上板听感后决定是否换（合成成本极低，重合成即可）

## Phase 6 全闭环总览（Step 1~5）

- Step 1：dm_voice.h 数据结构 + dm_health 统计/state 8 字段 + cfg 35 参数（ok-20260828-23）
- Step 2：dm_voice.c 决策引擎 422 行（12 场景/闭嘴链/加权随机/预算）（ok-20260828-24）
- Step 3：接线（on_seated/reminder/AI 时间戳/音乐状态/init）（ok-20260828-25）
- Step 4：欢迎回来卡 + 弹窗文案银月化（ok-20260828-26）
- Step 5：AI prompt 银月化 + 12 wav 合成 + 18 wav 落地（ok-20260828-27）

**待上板**：烧录整包 → 观察一日剧本（说 6 次/闭嘴 10 次）→ 调 budget/cooldown 阈值 → 补增量 14 wav → 音色女声化评估。

*DevLog by AtomCode (deepseek-v4-flash)*
