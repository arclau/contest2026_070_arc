# DevLog 2026-09-01（part1）— AI 语音尾音收尾（P141）+ 背景音乐开场（P142）+ 欢迎语去称呼（P143）+ 锁屏乱码修复（P144）

> 承接 8-31part1（P140 驱动层 POP 根治）。本段覆盖 ok-20260901-1~4（4 个稳点），
> 全部为**上板反馈驱动的二次修复 + 新功能**。用户反馈 = 验证，上板前先烧录听效果。

## 一、P141 AI 语音仍有尾音 → PA 关断顺序修复（固化 ok-20260901-1）

### 现象（用户上板验证 P140 后）
用户烧 ok-20260831-4（含 P140 驱动先静音再关 DAC）后：**语音播报（尤其开机欢迎）播放 wav 仍有尾音**；音乐播放器播放同 wav 无此问题。

### 排查链（P140 为何没根治）
1. **链路对比**：语音（dm_tone_worker）播完必 drain→close→dapm off；音乐（XPlayer→dm_sound）连播/循环几乎不 close → **只有语音路径触发 dapm off**。
2. **P140 只修了一半**：`sun8iw20-codec.c` 两处 OFF 分支（lineout_route/hp_route）已改为「DACLMUTE=0 静音 → 5ms → DACLEN=0 关 DAC」；但**PA（功放 GPIO，GPIOD17=pa_level 拉低）仍排在静音之前**（1003-1006 vs 1010）——功放掉电瞬间 DAC 还在输出 → 喇叭 POP 依旧。
3. 默认 route = `PB_AUDIO_ROUTE_LO_HP_SPK`（sun8iw20-codec.c:96）→ dapm off 走 `lineout_route(spk=1)` → 先关 PA 再静音，问题必然命中。

### 修复（驱动层，A3 纪律：只调序不改逻辑）
两处 OFF 分支统一改为：**DACLMUTE=0 静音 → hal_msleep(5) → PA 关断（gpio 拉低）→ DACLEN/DACREN=0 → EN_DAC=0**：
- `sunxi_codec_playback_lineout_route` else 分支：`if (spk)` PA 关断块从函数开头移到静音+5ms 之后
- `sunxi_codec_playback_hp_route` else（非 A 版本）分支：同样补静音后关 PA；CHIP_VER_A 分支保持原行为不动

### 产物与固化
- 编译 ✅ / 打包 ✅ / nsh.fex md5 == vela.bin md5（9a301c87）✅ / check_res ✅
- git_snapshot 三仓 → **ok-20260901-1**（sun8iw20-codec.c 1 文件）

### 待上板
烧录 ok-20260901-1 → 开机欢迎/喝水站立提醒/AI 语音播完应无尾音（与 P140 一起验证）。

## 二、P142 语音提示加开场背景音效（固化 ok-20260901-2）

### 需求（用户）
语音提示"总是很突然，突然有人说话很可怕"——**每个语音提示前先播一段背景音乐（开场音效）**。用户提供 4 个背景音乐 wav 在 /data/dm。

### 资源（转英文名进 res）
| 原文件名 | 英文名 | 用途 |
|----------|--------|------|
| 背景音乐_奖励_心之容器.wav | `bg_reward_heart` | 开机欢迎 |
| 背景音乐_奖励_标准.wav | `bg_reward_std` | 喝水/久坐提醒 |
| 背景音乐_奖励_豪华.wav | `bg_reward_lux` | 喝满 6 次水夸奖 |
| 背景音乐_奖励_高评价.wav | `bg_reward_high` | 备用（未接线） |
全部 32000Hz/16bit/stereo（与提示音 24000Hz/mono 不同 → 两个文件必须各自独立 open/hw_params）。

### 代码改动（luncher_dm 仓）
1. **`dm_tone_play_seq(intro, voice)` 新入口**：dm_tone_worker 重构为顺序播放两个 wav；抽出 `tone_play_one()`（单文件 open→写→fade→drain→close），worker 解析 "intro|voice" 字符串依次调用（格式不同各自 open，P87 小块缓冲纪律保持）。
2. **接入三场景**：
   - 开机欢迎（dm_voice.c `voice_director_on_seated`，CAT_GREETING）→ `dm_tone_play_seq("bg_reward_heart", d.wav)`
   - 喝水/久坐弹窗（ui_home.c `health_popup_show`）→ `dm_tone_play_seq("bg_reward_std", popup_*)`
   - 喝满 6 次水（dm_health.c `dm_health_water_drank` 计数==6 → 新增事件 `DM_HEALTH_EVT_WATER_6` → ui_home.c `standby_health_cb` 接住）→ `dm_tone_play_seq("bg_reward_lux", "care_01")`（夸奖语音暂用 care_01 占位，可换 tts_proto 合成专用）
3. **tone_test 测试命令**（照 uartdbg_test 先例，独立 NSH app `vendor/allwinnertech/apps/tone_test/`，同镜像直接调 luncher_dm 的 dm_tone_play_seq）：
   - `tone_test boot|water|sit|lux` 单场景；`tone_test all` 4 组依次播（间隔 6s）

### 产物与固化
- 编译 ✅ / 打包 ✅ / nsh.fex md5 == vela.bin md5（0f17f682）✅ / check_res ✅（**提示音 18→22**，脚本期望值同步）
- git_snapshot → **ok-20260901-2**（vendor 仓 15 文件：4 wav + tone_test app 4 文件 + dm_tone/dm_voice/dm_health/ui_home/deskmate_ui.h）

### 待上板
烧录后：`tone_test all` 依次听 4 组组合；真实场景触发喝水/久坐弹窗、连喝 6 次水验证夸奖。

## 三、P143 欢迎语去掉「主人」称呼（固化 ok-20260901-3）

### 需求（用户）
欢迎语音里 AI 念了「主人」（P134 设置的称呼）→ **去掉**，重新打包。

### 改动
1. **重合成 2 个含「主人」的欢迎语 wav**（/data/vela/tts_proto/make_tone_wav.py，火山 TTS 2.0 同音色 zh_male_sophie，24000Hz/16bit/mono 规格不变）：
   - greet_hello：「早呀主人，今天也元气满满？」→ **「早呀，今天也元气满满？」**
   - greet_first：「诶嘿，主人来啦~今天也要加油哦。」→ **「诶嘿，来啦~今天也要加油哦。」**
2. **dm_health_cfg.c 模板同步**：`greet_first=` 文案去「主人」（与 wav 一致）。

### 产物与固化
- 编译 ✅ / 打包 ✅ / nsh.fex md5 == vela.bin md5（80e33a7f）✅ / check_res ✅（22 提示音全在）
- git_snapshot → **ok-20260901-3**（2 wav 重合成 + 1 配置）

### 待上板
烧录后欢迎语 = 心之容器音乐 →「诶嘿，来啦~今天也要加油哦。」（无主人）。**注意 popup_water_3 弹窗文案仍有「主人，水都凉啦」**（cfg 模板 + ui_home.c 兜底），用户未要求改，如需一并去除下会话处理。

## 四、P144 锁屏「人体」体字乱码修复（固化 ok-20260901-4）

### 现象（用户上板反馈）
锁屏右下角「人体」距离显示：**更新距离或走开再回来时，「体」字乱码**。

### 排查链（P126 遗留，P128「去 CLIP 根治」系误判）
1. **P128 只验证了"距离显示了"，未验证乱码消除**；P129 改 24px 后复现 → 乱码从未真正解决。
2. **排除三项**：① FreeType 位图 pitch 拷贝错行——freetype-py 实测 MiSans 24px 全部 `pitch==width`（lv_freetype_image.c:193-195 用 box_w 行距无错）；② 字形本身损坏——人/体/有/无 outline 与渲染位图均正常；③ glyph/draw_data cache key 冲突——key 均含 `size`（lv_freetype_glyph.c:196-206 / lv_freetype_image.c:209-218），不同字号条目不冲突。
3. **根因实锤**：`CONFIG_LV_FREETYPE_CACHE_FT_GLYPH_CNT=256` → draw_data_cache 仅 256 条目，而全 UI 多字号共享 cache_node（同 pathname=同一 face）、中文字形远超 256 → **LRU 频繁淘汰字形位图**；且 `standby_health_rings_refresh` **每秒无条件 `lv_label_set_text`**（内容相同也重绘）→ 文本切换（距离更新/无人↔有人）时被淘汰字形（如「体」）重建竞态 → 渲染错乱。

### 修复（2 处，低风险防御）
| 文件 | 改动 |
|------|------|
| `configs/nsh/defconfig` | `LV_FREETYPE_CACHE_FT_GLYPH_CNT` 256→**1024**（字形位图缓存扩容 4 倍） |
| `ui_home.c` | `standby_prox_lbl` 刷新**文本去重**：`g_prox_lbl_text[32]` 缓存上次文本，内容未变跳过 `set_text`（不再每秒无谓重绘） |

### 产物与固化
- 编译 ✅（首轮缺 `g_prox_lbl_text` 声明报错 → 补 static 声明后通过）/ 打包 ✅ / nsh.fex md5 == vela.bin md5（d632ca89）✅ / check_res ✅（22 提示音全在）
- git_snapshot → **ok-20260901-4**（defconfig + ui_home.c 2 文件）

### 待上板
烧录后进锁屏：① 距离刷新时「人体」应始终清晰；② 走开再回来（无人↔有人切换）无乱码；③ 若仍有偶发乱码，下一步给 standby_prox_lbl 显式 `LV_LABEL_LONG_DOT` 兜底。

## 五、待上板汇总（本会话 4 个稳点）

| P | 稳点 | 上板验证要点 |
|----|------|--------------|
| P141 | ok-20260901-1 | 语音播完无尾音（PA 关断时序） |
| P142 | ok-20260901-2 | tone_test all 试听 4 组背景+语音；喝水/久坐弹窗带音乐 |
| P143 | ok-20260901-3 | 欢迎语无「主人」 |
| P144 | ok-20260901-4 | 锁屏「人体」刷新/切换无乱码 |

*DevLog by AtomCode (deepseek-v4-flash)*
