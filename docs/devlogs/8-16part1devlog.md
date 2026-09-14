# DevLog 2026-08-16 — 距离校准/离开暂停/本地提示音/通知时序/语音链路修复/UI 配色/HOME 双小圈

> **会话交接**：本文件是 8-16 首篇（part1）。承接 8-15 P100（prox M→CM 统一，代码 ok-20260815-42）。本 part 记录：prox 距离校准、离开判定逻辑、本地提示音全链路、AI 中文/位置预置、llm_router 兜底、通知时序 A+B、TTS 播报链路修复（EBUSY/截断/声卡占用）、副歌破音软限幅、锁屏与 HOME UI 配色改造。结束代码 **ok-20260816-22**（22 个固化点）。

## 一、prox 距离校准 P101

**背景**：上板实测 prox 分档值偏小——"显示 10cm 实际约 20cm，超过实际 20cm 无法探知"（LTR553 探测上限物理限制）。

**根因/方案**：
- 驱动分档原始值→cm 映射按实测 ×2 校准（thread + fetch 两处同步）：
  `ps_data>1000→0cm（贴脸）` / `>500→2cm` / `>100→10cm` / `>10→20cm（探测上限）` / `≤10→100cm（无物体）`
- fetch 处 else 档原 20cm 遗漏（P101 只改 thread）→ 一并 100cm

**验证**：编译→打包→资源检查→固化 **ok-20260816-1/2**；分档语义：有物体 0/2/10/20cm、无物体 100cm（>30 离开档）。

## 二、离开判定逻辑 P102

**现象**：离开感应距离后双圈仍在倒数（"感觉不到我坐下来"曾因 20≤30 恒在座；修复后暴露离开侧问题）。

**根因**：离开判定要求 prox>30 连续 20s，等待期间 `g_seated` 仍 true、倒计时照常递减——离开后圈圈继续倒数 20s。

**方案**（dm_health.c）：
- 新增 `g_leaving` 标志：离开感应区（prox>30）**立即暂停倒计时**（remain 不再递减），防抖满才 `health_session_end()` 清零
- 防抖 20s→5s（`PROX_ABSENT_DEBOUNCE_S`），cfg 模板同步
- 回来（prox≤30）恢复倒数；跨天仍清零

**验证**：编译→打包→资源检查→固化 **ok-20260816-3**。

## 三、欢迎语每时段限播

**现象**：12:30 饭点坐下→离座→再入座反复触发"饭点都过了还坐着"。

**方案**（ui_home.c `health_sit_greeting`）：新增时段 id（morning/noon/afternoon/evening/night）+ 计数，**每时段限播 `greet_max_per_period=2` 次**（cfg 可配），跨时段自动重置；超限静默坐下。开机首次 welcome 不受限。

**验证**：固化 **ok-20260816-6**（上板日志 `greeting: 15:00 silent (period 2, cnt=2 >= limit)` 验证通过）。

## 四、本地提示音全链路 P103

**背景**：健康提醒（弹窗/欢迎语）原走云端 TTS（dm_ai_voice_speak），依赖网络；用户要求用本地 wav。

**方案**：
1. **PC 本地合成环境**：旧 devlog 的 `/tmp/tts_proto` 已清空，重新部署到 `/data/vela/tts_proto/`——官方 `protocols_.py`（用户协议 zip 解压）+ Python websockets 16.1 + `make_tone_wav.py` 脚本（单条/`--all` 批量）。
   - ⚠️ 踩坑：import 名应为 `protocols_`（文件名带下划线）；**403 = 1.0 resource（volc.service_type.10029）+ `*_mars_bigtts` 音色不匹配** → 按 8-14 实锤改用 **`seed-tts-2.0` + `zh_male_sophie_uranus_bigtts`**（2.0 音色）后链路通。
2. **合成 12 个提示音**（24000Hz/16bit/mono，与板端 TTS 同规格）：`popup_water_1/2/3`、`popup_sit_1/2/3`、`greet_first/morning/noon/afternoon/evening/night` → `lichee/board/common/data/res/tones/`（进 res.fex，板端 `/resource/tones/`）。
3. **板端播放器**（deskmate_ui.c `dm_tone_play()`）：后台线程直读 WAV→snd_vela_pcm 播放，**不经过 XPlayer**（避免与音乐状态机纠缠）；声卡被音乐占用时跳过提示音（不打断音乐）。
4. **接入点**（ui_home.c）：健康弹窗（喝水/久坐全档位）与坐下欢迎语均改为 `dm_tone_play`，不依赖云端 TTS/网络。

**验证**：编译→打包→资源检查（含 tones wav 三段一致）→固化 **ok-20260816-4**；上板日志 `[tone] done: /resource/tones/greet_first.wav` + codec 播放序列验证。

## 五、AI 回复强制中文 + 位置预置

**现象**：问"今天天气怎么样？"AI 回复英语（TTS 用中文音色念英文，听感"说一堆英语"）；web_search 无 key 时英文错误原样念出。

**根因**：system prompt 英文 Rules 占多 + 中文约束仅弱规则（`Reply in user's language`）。

**方案**（packages/ai_agent/src/core/context_builder.c）：新增「## 语言（最重要）」3 条强制规则——始终简体中文（用户明确要求除外）/提示词其他部分英文也必须回中文/工具返回内容（含英文错误）必须翻译成中文组织。

**位置预置**：用户要求预置"广东惠州龙门"避免问天气被反问位置——`templates/USER.md` + `memory_store.c` 默认 user 信息写入 `位置：广东省惠州市龙门县 / 天气关注城市：惠州龙门`（经 AGENT_USER_FILE → /data/agent/config/USER.md 注入 system prompt）。

**验证**：固化 **ok-20260816-5/10**。

## 六、llm_router "No available backend" 根治

**现象**：日志持续 `[llm_router] No available backend`，LLM 调用失败。

**根因**：llm_router 从 config 加载 backend 列表，未执行 set_llm/未配置 router 时 `s_backend_count=0` → select 返回 -1；且板端可能残留 host 为空的 `llm_backend_0` 配置导致 `s_backend_count!=0` 但 select 仍跳过。

**方案**（llm_router.c）：
1. `llm_router_init`：config 无 backend 时用 llm_proxy 默认配置（agent_secrets.h 火山方舟 key/model）**兜底注册 backend[0]**（与 llm_proxy_init 同源）
2. 加载时 **host 为空的残留配置跳过**（不计数），确保兜底必然生效

**验证**：`strings vela.bin` 确认 `Skipping backend %d: empty host` + `P103: defaulted backend 0` 已进固件；固化 **ok-20260816-10/11**。

## 七、通知时序审计 + A+B 修复

**现象**（用户交互工程师视角审计）：开机到桌面→WiFi 初始化慢→连上后同步时间天气→**锁屏后才报第一次语音播报**→坐下倒数。觉得"怪"。

**根因**（代码实锤）：
- `dm_health_set_cb(standby_health_cb)` 只在 `show_standby()`（进锁屏）注册，而 dm_health 状态机开机就跑 → **锁屏前坐下事件（欢迎语/双圈反馈）全部丢失**
- 欢迎语分时段依赖本地时间，而时间靠 WiFi 同步（未对时时时段判断不可靠）

**方案**：
- **A**：`dm_health_set_cb` 注册提前到 `ui_home_create()` 末尾（开机即注册）；回调内 standby UI 判空（standby_health_rings_refresh/health_popup_hide/show 均有 NULL 保护），锁屏前只播语音/弹窗不刷双圈
- **B**：欢迎语等时间就绪——`g_greet_time_ready`/`g_greet_pending`；`health_sit_greeting` 未对时时 defer；`weather_update_cb` 成功分支（=HTTP Date 对时完成）调 `health_greet_maybe_catchup()` 补播

**验证**：改前固化 **ok-20260816-8**（回退点），改后固化 **ok-20260816-9**。

## 八、TTS 播报链路修复（EBUSY/截断/声卡占用）

**现象**：①音乐播放中 AI 回复 `snd_vela_pcm_open fail -16 (EBUSY)` ×10 重试仍失败 → `voice_channel_speak failed: -5`（说话无声）；②播报尾音"立马断掉"。

**根因**：
- 声卡 `hw:audiocodec` 单实例，luncher_dm 音乐（XPlayer/dm_sound）占用时 ai_agent TTS open 必 EBUSY；且 **XPlayer pause 只调 snd_vela_pcm_pause 不关 handle**（PAUSE_PUSH 后 CNT 停住 = 句柄未释放）
- `audio_playback_close()` 无条件 `snd_vela_pcm_drop`——TTS 最后一块 PCM 还在 8192 帧≈170ms buffer 里就被丢弃 → 尾音截断

**方案**：
1. `audio_playback.c`：open 返回 -16 时**重试 10 次×200ms 等待声卡释放**（非 EBUSY 不重试）；close 正常路径（stopped=0）改 **drain 等缓冲播完**，被打断才 drop
2. `deskmate_ui.c` `dm_ai_voice_press_cb`：PTT 按下 → `music_audio_force_stop()` **停音乐释放声卡**（语音优先，音乐让路），使 TTS 播报时声卡空闲
3. **三处播放按钮状态同步**：`music_audio_force_stop` 改为先调 `music_pause()`（同步 music_playing=false + HOME/锁屏/播放器三按钮去 CHECKED/回 PLAY 图标）再 XPlayerStop——修"AI 打断音乐后按钮残留播放中"不同步

**验证**：固化 **ok-20260816-12/19/21**。

## 九、副歌破音软限幅

**现象**：放歌到副歌部分"破音、发沙"。

**根因**（日志实锤）：`raw_peak=32767`（解码输出 16bit 满幅）+ raw_rms 持续升高（9972→11708→13269）——XPlayer 解码/44100→48000 重采样后输出顶格削波，`dm_sound_write` 只有线性衰减无限幅。

**方案**（deskmate_ui.c `dm_sound_write`）：软件音量衰减后加**软限幅（折线压缩近似 tanh）**——|v|>30000 后斜率 1/4（`v = 30000 + (v-30000)/4`），削去硬顶保留动态，纯整数无浮点开销。

**验证**：固化 **ok-20260816-16**（注意：物理功放/喇叭过载非软件可解，若仍破音只能降音量）。

## 十、锁屏/HOME UI 配色与显示改造

1. **h1 在座时长**：`HH:MM` 无秒 → **`HH:MM:SS`**（秒级跳动可见，修"坐下累计看不见"误判；`buf[16]→[24]`）+ 加"在座"标签
2. **双圈反色对撞**（用户拍板）：喝水环→**橙**、久坐环→**蓝**（原喝水蓝/久坐橙对调）；**knob 圆头点显式设色**（`LV_PART_KNOB`，原未设→LVGL 默认蓝导致久坐环"点"是蓝）；背景轨浅灰 0xE5E5EA
3. **健康卡**：边缘**蓝色光晕**（对齐播放按钮发光语言，覆盖 style_card 黑影）；内部三行配色跟随双圈（在座/Stand 蓝、Water 橙）
4. **弹窗按钮**（喝水/久坐弹窗）：左键 COL_BLUE 蓝底+蓝光晕+按压变深 0x0062CC、右键 iOS 浅灰 0xE5E5EA+细阴影+按压 0xD1D1D6、胶囊圆角
5. **锁屏 AI 语音钮**：样式对齐音乐播放按钮（白玻璃 OPA90+蓝阴影 8px+按压蓝紫渐变光晕），图标保留麦克风 LV_SYMBOL_AUDIO——UI 独立、功能共享
6. **HOME 右侧卡**：原 AI 静态卡（Ready to help 样板）→ **喝水/久坐双小圈**（`create_home_health_card`，DM(56) 缩小版，配色同锁屏反色；`home_health_rings_refresh` 挂 RING 事件每秒同刷，与锁屏双圈永远同步）

**验证**：固化 **ok-20260816-13/14/15/18/20/22**（编译时修 1 处 home_* 变量声明顺序错误）。

## 改动文件汇总

| 仓 | 文件 | 改动 |
|----|------|------|
| vendor | `chips/r528/.../sensor/als/ltr553.c` | P101 分档校准（thread+fetch 两处） |
| vendor | `apps/luncher_dm/dm_health.c` | g_leaving 暂停/防抖 5s |
| vendor | `apps/luncher_dm/dm_health_cfg.c` | prox_absent_debounce_s=5、greet_max_per_period=2 |
| vendor | `apps/luncher_dm/ui/ui_home.c` | 欢迎语限播/h1 秒级/双圈反色/健康卡/AI 钮样式/弹窗按钮/HOME 双小圈/回调注册提前/对时补播 |
| vendor | `apps/luncher_dm/deskmate_ui.c` | dm_tone_play 播放器/软限幅/force_stop 同步/AI 钮回调 |
| vendor | `apps/luncher_dm/deskmate_ui.h` | dm_tone_play 声明 |
| vendor | `apps/luncher_dm/dm_health.h/cfg.h` | 无（cfg 已含） |
| packages | `ai_agent/src/core/context_builder.c` | AI 强制中文语言规则 |
| packages | `ai_agent/src/core/memory_store.c` | 默认 user 位置惠州龙门 |
| packages | `ai_agent/agent_skills/templates/USER.md` | 位置/天气关注城市 |
| packages | `ai_agent/src/llm/llm_router.c` | 默认后端兜底 + 空 host 跳过 |
| packages | `ai_agent/src/voice/audio_playback.c` | EBUSY 重试 + close drain 防截断 |
| packages | `ai_agent/src/tools/tool_media.c` | 点歌失败中文提示 |
| 资源 | `lichee/board/common/data/res/tones/` | 12 个提示音 wav（进 res.fex） |
| 工具 | `/data/vela/tts_proto/` | 本地 TTS 合成环境（protocols_.py + make_tone_wav.py） |

## 验证命令与产物

- 编译：`./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh -j$(nproc)`
- 打包：`lichee && source envsetup.sh && lunch_nuttx 2 && pack`
- 资源检查：`python3` 脚本验证 res.fex 含 MiSans/FW_NIC_ACUT/tones/*.wav 且整包镜像含完整 res.fex 块（P92 纪律）
- 固化：`bash /data/vela/git_snapshot.sh` + `packages/ai_agent` 独立仓手动 commit/tag（git_snapshot.sh 三仓不覆盖 packages）
- 产物：`/data/vela/releases/ok-20260816-1~22/`

## 遗留事项

- 烧录必须整包镜像（含 res.fex）——板端 `FT_New_Face error` + WiFi `download_fw FAIL 0x15` 双症状 = res 分区未烧写（P74/P92 老坑，PhoenixSuit 需勾选 res 分区）
- AI 回复延迟（LLM 非流式，等完整生成才 TTS）——架构级，需用户拍板是否流式化（llm_proxy SSE + agent_loop 流式 + TTS 并行）
- 音乐播放中 TTS 播报：已做"PTT 按下停音乐"语音优先；"AI 播完自动恢复音乐"需 ai_agent→luncher_dm 跨进程 WS 通知（P84 voice_evt 机制可扩展）
- 副歌破音若仍存在：物理功放/喇叭过载（非软件可解）
- 第②层：语音改参数 + 风格切换（dm_health_cfg_set_* 已预留）
- 光感 ALS 已采集未订阅——自动亮度/午休检测/接近亮屏等方向已记 memory 待实施
- 蓝牙 H4 110 挂起（勿擅改驱动）；状态栏改进（中文年月日/WiFi 图标）；Files/Books/Games 回归

*DevLog by AtomCode (deepseek-v4-flash)*
