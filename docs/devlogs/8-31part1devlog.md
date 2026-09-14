# DevLog 2026-08-31（part1）— P129~P135 上板验证 + P137 AI 语音尾音修复

## 一、P129~P135 上板验证（镜像 ok-20260828-28，整包烧录）

| 项 | 结果 |
|----|------|
| P129 锁屏健康卡（喝水/起立汉化 + 字号 24px/宽 260） | ✅ 通过 |
| P130 状态栏/锁屏日期中文（周日 8月28日，无 Sun/Aug 英文） | ✅ 通过 |
| P128 锁屏人体距离显示（有人 X.Xm / 离开隐藏，无乱码 CLIP） | ✅ 通过 |
| P131/P132 PTT 录音 EBUSY 根治（连续按 + 打断 TTS 正常） | ✅ 通过（日志：ASR 正常收文本，无 prepare EBUSY） |
| P133 AI 本地上下文（健康/天气注入） | ⚠️ 部分：**「我坐了多久」回复正确**；**「今天天气怎么样」AI 答"天气 API 没接入"——天气本地注入未生效，待修** |
| P134 AI 称呼「主人」 | ✅ 通过 |
| P135 语音编排听感（坐下欢迎/时段/银月语感） | ⏳ 待确认（用户"未感受到"，需一日剧本观察） |

日志佐证：`[time] sync via HTTP Date OK`、`ASR: 今天天气。`、`volc_asr Server sent WS close (code=1000, reason=finish last sequence)` —— TTS/ASR 链路正常。

### P133 天气注入待修线索
- AI 答"天气 API 没接入"而非引用本地数据 → system prompt 注入分支未命中或 `/data/dm_weather.state` 未生成/路径不对
- 板端排查：`ls -la /data/dm_weather.state` + 看天气 worker 是否落盘

## 二、P137 AI 语音尾音修复（固化 ok-20260831-1）

### 现象
AI 语音（本地 wav 提示音 + TTS 流式回复）**播放完毕后出现一个残缺尾音**（例：「主人你好啊」的「啊」说完后多一声）；音乐播放完毕无此问题。

### 排查过程
1. **wav 文件证伪**：PC 端脚本检查全部 18 个 wav 尾部——结尾静音 0~9ms、尾 50ms 峰值 53~142（满幅 32767）、无爆音；用户电脑单独播放全部正常 → **文件干净，问题在板端播放链路**。
2. **链路对比**：
   - 本地 wav（欢迎语/提醒/粒子）：`dm_tone_worker`（luncher_dm）→ `snd_vela_pcm_writei` 循环 → `drain` → `close`
   - AI 回复（TTS 流式）：`audio_playback_close`（ai_agent）→ `drain`（正常播完）→ `close`
   - 音乐：长播/循环，**几乎从不自然播完走 close** → 解释了"只有 AI 语音有尾音"
3. **根因定位**：aw-tiny-alsa-lib `snd_vela_pcm_close()`（pcm.c:358）内部**无条件先 `snd_vela_pcm_drop()`**（→ `ksnd_pcm_drop` → `snd_pcm_stop(SETUP)` 硬切）。drain 等 DMA 播完后 DAC 输出电平未必归零，close 的 drop 在非零电平硬切 → 爆音/残缺尾音。

### 修复（板端 fade-out，不动驱动——A3 纪律）
播放结束、`drain` 前补写 **30ms 线性衰减到 0** 的 fade-out 帧，让 close 硬切发生在静音电平上：

1. **`luncher_dm/deskmate_ui.c` dm_tone_worker**：写循环内记录最后样本电平 `last`，写完 wav 后小块循环（int16_t fade[512]，勿大栈数组——P87）生成 fade-out 写入，再 drain/close。
2. **`packages/ai_agent/src/voice/audio_playback.c`**：
   - struct 加 `last_sample` 字段，`audio_playback_write` 记录本次块最后样本；
   - `audio_playback_close` 正常播完（stopped=0）分支，drain 前小块循环 fade-out（初版 `fade[1024]` + `fade_frames<=1024` 在 48kHz 会静默跳过，改小块循环修复）。

### 产物与固化
- 编译 ✅ / 打包 ✅ / nsh.fex md5 == vela.bin md5（491b15c8）✅ / check_res ✅（18 提示音 + 固件/字体全在）
- git_snapshot.sh 三仓固化 → **ok-20260831-1**；**packages/ai_agent 独立仓不在脚本 REPOS 内，手动 add/commit/tag ok-20260831-1**（fade-out 修改落在该仓 audio_playback.c）

### 待上板
烧录 ok-20260831-1 → AI 语音播完应无残缺尾音；顺带验证 P133 天气注入（板端查 /data/dm_weather.state）。

## 三、P138 时段 bug 修复（固化 ok-20260831-2）

### 现象
用户 16 点上厕所回来坐下，AI 播「早啊主人，今天也要元气满满」——16 点被当早上。日志：`health_session_start: seated` → `welcome_card_show`。

### 根因
**dm_voice.c `collect_ctx()` 用 `localtime_r()` 直接取 `tm_hour`，而系统 time 是 UTC**（`settimeofday` 设 UTC，dm_weather.c:424）。UI 层全部手动补偿东八区：`gmtime + tm_hour += 8 + mktime`（ui_home.c:48-52 clock_update_cb、luncher_dm.c:485-490 同款）。dm_voice 漏了补偿 → 本地 16 点 = UTC 08:15 → `tm_hour=8` → 命中 morning 时段（5~10 点）→ 播早安 wav。

### 修复
`collect_ctx()` 改为与 UI 层一致：`gmtime(&now)` → `tm_hour += 8` → `mktime()` 规范化跨天；`gmtime` 失败时 `tm_hour=-1`（时段退化）。

### 产物与固化
- 编译 ✅ / 打包 ✅ / nsh.fex md5 == vela.bin md5（4358c41d）✅ / check_res ✅（18 提示音全在）
- git_snapshot 三仓 → **ok-20260831-2**（仅 vendor/allwinnertech 仓 dm_voice.c 1 文件）

### 待上板
烧录 ok-20260831-2 → 16 点回来应播下午时段欢迎语（tag=afternoon，w_afternoon_teasing=1.5/caring=0.6），不再播早安。

## 四、P139 站立自动检测（固化 ok-20260831-3，Phase A 简单版）

### 背景（用户批评健康助理"稀碎"）
用户指出：人站起来系统无感知（起立计数只能靠久坐弹窗点「我起来了」）；喝水只能手动点且无常驻累计显示。经能力边界分析：
- LD2410B 已解析运动状态（MOVING/STATIC/BOTH）与距离，但 dm_prox_get_cm() 压成三态（0=在座/99=离开/-1=不可用）丢弃语义
- 站立：可自动推断（离座又回来）；喝水：物理测不到（雷达看不到举杯）

### 方案（用户拍板 Phase A 简单版）
**离座又回来 = 自动记一次起立**，不再只靠弹窗按钮。

### 实施（dm_health.c）
1. `STAND_MIN_S 3` 宏：离开感应区 ≥3s 判定起立（防抖，防传感器抖动误报）
2. `g_stand_recorded` 状态标志：本次离开只记一次起立；回来（prox==0）重置，下次离开可再记
3. `health_tick_cb` prox!=0（离开）分支：`g_away_s >= STAND_MIN_S && !g_stand_recorded` → `stand_count_today++` + 落盘 + 日志 `[health] auto stand, count=N`
   - 边界：g_away_s 在 session 内单调增长（回来不清零），故必须用标志而非 `== STAND_MIN_S` 判断，否则第二次离座失效
4. UI：锁屏健康卡 h3（`standby_health_rings_refresh` 读 `dm_health_stand_count()`）经每秒 RING 自动刷新，无需改 UI

### 产物与固化
- 编译 ✅ / 打包 ✅ / nsh.fex md5 == vela.bin md5（037582ee）✅ / check_res ✅（18 提示音全在）
- git_snapshot 三仓 → **ok-20260831-3**（dm_health.c 1 文件）

### 待上板
烧录 ok-20260831-3 → 起身离开 ≥3s 再回来，锁屏健康卡「起立 N」应 +1；久坐弹窗「我起来了」按钮保留（手动兜底）。

## 五、P140 AI 语音 POP/CLICK 根治（固化 ok-20260831-4，驱动层）

### 现象（用户反馈：P137 fade-out 后仍有 pop）
用户烧 ok-20260831-3（含 P137 fade-out）后：欢迎语「早啊，主人，今天也要元气满满」**说完后仍有一点点像麦克风磕碰的声音（POP/CLICK）**，音乐播放完毕无此问题。日志佐证固件最新：`health_session_start ... dm_health.c:193`（P139 改动使行号 190→193）。

### 排查链（fade-out 为何无效）
1. **drain 机制正常**：`ksnd_pcm_drain`（snd_pcm.c:2290）DRAINING 状态等 DMA 播完 buffer（snd_pcm_update_state → avail>=buffer_size → snd_pcm_stop(SETUP) → 唤醒），**fade 帧会被播完**——数字域已无尾音。
2. **close 链路全摸清**：`snd_vela_pcm_close`（pcm.c:358）无条件 drop → hw_free → `soc_pcm_close`（snd_core.c:578）→ **`dapm_control(0)` 关 DAC 通路**（shutdown 为空实现）。
3. **根因实锤**：`sunxi_codec_playback_lineout_route`（sun8iw20-codec.c:958）onoff=0 分支关闭顺序反了：
   - 原顺序：**先 `DACLEN/DACREN=0`（关 DAC 模拟输出）→ 再 `DACLMUTE/DACRMUTE=0`（静音）**
   - DAC 在**未静音状态**下输出被硬断 → 输出电平瞬跳 → **POP/CLICK**
   - DACLMUTE 语义确认：ON 分支写 1 能出声 → 1=正常输出，0=静音 → 关闭时应先写 0（静音）
4. **P137 fade-out 只治数字域**（drain 前衰减 PCM 数据），**碰不到模拟域 DAC 关断瞬态**——所以 fade-out 后 pop 原样还在。音乐几乎不自然播完 close，所以只有 AI 语音有。

### 修复（驱动层，用户拍板方案 A）
`sun8iw20-codec.c` 两处 OFF 分支（hp_route else + lineout_route else）改为：
```c
snd_codec_update_bits(codec, SUNXI_DAC_ANA_CTL,
        (0x1<<DACLMUTE) | (0x1<<DACRMUTE),
        (0x0<<DACLMUTE) | (0x0<<DACRMUTE));   /* ① 先静音 */
hal_msleep(5);                                 /* ② 等输出归零 */
/* ③ 再关 DAC 模拟输出（DACLEN/DACREN=0）+ 数字（EN_DAC=0） */
```
- hp_route else 分支（原 944-954）：补静音前置
- lineout_route else 分支（原 995-1012）：调换静音与关 DAC 顺序
- P15/P16 改过同文件（RDEN 清零条件）有先例；本次只调序+5ms，不动其他逻辑

### 产物与固化
- 编译 ✅ / 打包 ✅ / nsh.fex md5 == vela.bin md5（4c0df879）✅ / check_res ✅（18 提示音全在）
- git_snapshot 三仓 → **ok-20260831-4**（sun8iw20-codec.c 1 文件，14+/3-）

### 待上板
烧录 ok-20260831-4 → AI 语音（欢迎语/提醒/TTS 回复）播完应**无咔哒声**；顺带验证 P139 站立检测、P138 时段。
