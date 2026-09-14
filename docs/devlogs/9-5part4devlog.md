# 9-5 Part4 Devlog — Voice Director 听感修复（回来按时段说话 + 回来冷却独立 + 清空票）

> 日期：2026-09-05｜P175
> 触发：用户反馈语音提醒「死板」——离开再回来没响应、不同时间段说话没差别。
> 设计依据：`VOICEDESIGN.md` Phase 4 `VOICE_INTERACTION_DESIGN.md`（§四 12 场景 / §五 决策规则 / §六 台词）。

## P175 ✅ Voice Director 听感修复

**前置确认**：提醒系统链路本身是通的（dm_health 状态机 → 事件 → health_popup_show → L1/L2/L3 弹窗+语音），用户「没提醒」查下来不是框架缺失，而是 dm_voice.c 决策层的**资源调度与冷却策略**导致听感死板。

### 根因（3 + 2）

1. **不同时段说话没差别**：`greet_morning/noon/afternoon/evening/night` 5 个时段问候 wav 早已合成在 `res/tones/`，但 dm_voice.c 决策池**从没引用**——任何时段回来都是从 tease/care/tiny 里随机，说话永远一样。
2. **离开再回来没响应**：`pass_silence_checks` 用 `voice_cooldown_s=600`（10 分钟）一刀切。只要坐下播过一次语音（如开机欢迎），10 分钟内任何回到座位全被闭嘴检查拦截——用户大概率落在这个 10 分钟窗口内。
3. **决策池一半是「空票」**：池里 `tease_02~04` / `care_02~04` / `tiny_02~05` 引用从未合成的 wav，随机选中后 `dm_tone_play` 打开失败静默跳过 → 有效候选只剩 tease_01 / care_01 / tiny_01 三个，翻来覆去重复。
4. （连带）当天多次坐（场景 7,today_sit_cnt≥6）原设计要趋静，但代码没有显式静默分支，反而正常整句播报。
5. （连带）`hour<0`（未对时，时间戳仍为 1970）时 pick_category 落到 `night` 权重（tease 0.2 / care 2.0）→ 白天会误播「早点休息」类深夜句。

### 修复（只改决策层 dm_voice.c，音频播放链路零改动）

| # | 改动 | 文件 | 说明 |
|---|------|------|------|
| 1 | **CAT_TEASING 并入 back_01/back_02**（通用回来短句） | dm_voice.c g_pool | 回来调侃有变化，复用现成 wav |
| 2 | **CAT_CARING 并入 5 个时段问候**（greet_morning/noon/afternoon/evening/night + care_01） | dm_voice.c g_pool | **不同时段回来说不同的话**，由 pick_category 时段权重驱动：早晨→晨间句、中午→饭点句、下午 tease 权重最高（调侃活跃）、晚上/深夜 care 权重最高（温柔） |
| 3 | **剔除空票**：删 tease_02~04/care_02~04/tiny_02~05（资源从未合成） | dm_voice.c g_pool | 决策只覆盖真实存在 wav，杜绝白播 |
| 4 | **回来冷却独立**：新增 cfg `back_cooldown_s=180`；场景 3/4/5/6/8/9/12 用短冷却，问候(1/2)与多次坐趋静(7)保持全局 `voice_cooldown_s=600` | dm_voice.c pass_silence_checks + dm_health_cfg.c 模板 | 离开 ≥3 分钟再回来就有回应，不再被 10 分钟封死 |
| 5 | **场景 7 趋静**：pick_category 命中 scene==7 强制 CAT_SYSTEM_RESPONSE（只粒子/静默） | dm_voice.c pick_category | 当天多次坐下后话痨度收敛 |
| 6 | **hour<0 兜底**：未对时用 1:1:1 无时段倾向权重，避免误播深夜句 | dm_voice.c pick_category | 未对时（无网络）白天不说晚安 |

**候选池变更后**：`weighted_select` 缓冲 `cand[6]→cand[8]`（CAT_CARING 现 6 项）；`g_pool_cnt = {2,0,3,6,1}`。

### 行为预期（上板验证点）

- 开机首次坐下 → greet_first（bg_reward_heart 前置）；当天首次 → greet_hello
- 短离开(≤5min)回来、离开 3 分钟后 → 有回应（back_01/back_02 或时段句/粒子）
- 普通/长离开回来 → 按时段说话（早晨/中午/下午/晚上/深夜台词不同）
- 久坐后回来 → 调侃 tease_01；拒提醒后回来 → 调侃
- 当天第 6 次坐下后 → 只粒子/闭嘴（UI 正常）
- 未对时 → 说话但不分时段，白天不说"早点休息"

### 固化

- tag：`ok-20260905-32`（三仓一致）
- 产物：vela.bin == nsh.fex == 8155944B；check_res.sh 通过（res.fex 路径级验证，23 提示音）
- 板上 cfg 如需调，可在 `/data/dm_health.cfg` 追加 `back_cooldown_s`（代码默认 180）

### 待续

- 上板验证听感（AGENTS §三 待上板 ③P135 编排听感项顺带）
- 台词真多样性依赖后期合成 tease_02~04/care_02~04/tiny_02~05（asset plan 已列），当前用现有资源先跑通听感

---

## P176~P179（2026-09-05）Settings 实项整改：删 11 假项 + 存储真值 + 夜览手动暗光

### 背景
用户排查 Settings：26 项里仅 6 项有真实回调，20 项是假开关/假数据/占位行。要求删掉无意义的，实现低风险可落地的。

### 保留的真功能（6 项）
| 项 | 入口 |
|----|------|
| Wi-Fi 开关→dm_net_wifi_set + 管理子页 | settings_wifi_cb / settings_wifi_row_open_cb |
| 蓝牙开关→dm_net_bt_set + 管理子页 | settings_bt_cb(deskmate_ui.c:1228) / settings_bt_row_open_cb |
| 亮度滑块→屏幕 PWM ch4 + WS2812 | settings_brightness_cb → apply_brightness_hal |
| 自动锁定 30s/1m/5m/永不 | settings_autolock_cb |
| 音量滑块→g_music_volume | settings_volume_cb |
| UART 调试工具→子页 | settings_uart_dbg_open_cb |

### 删除的 11 项假项
深色模式、文字大小、通知、按键音、静音模式、语言、键盘、辅助功能、电池（87% 无数据源）、省电模式、软件更新。
原「存储 12.4/32GB + 39% 条」也是写死假值。

### 新增/改动
| 变更 | 说明 |
|------|------|
| apply_brightness_hal 抽取 | 亮度滑块/夜览共用 PWM+LED 写，不写 g_screen_brightness |
| 亮度滑块初值 75→g_screen_brightness | AI 语音(P115)改过亮度后不再跳回 75 |
| 存储真实化 | statfs(/sdcard) →「内存卡存储」「已用 x / 共 y GB」+ 进度条真实比例；无卡「未插入」 |
| 进度条可见性 | 背景 0x000000@10%→0xE5E5EA 不透明 + indicator COL_GREEN + 6px（原白卡片上≈白条） |
| 夜览改手动即时 | 开→立即压暗 10%（night_shift_brightness 可配）；关→无条件恢复 g_screen_brightness；开机按 cfg 恢复一次；落盘 night_shift / night_shift_brightness |

### 夜览「关不回去」踩坑
- ok-20260905-33：settings_night_shift_cb 无条件按「当前时段」应用 → 夜间关闭仍压暗档，开关已关屏仍暗
- ok-34 修复：`apply(g_ns_enabled ? in : 0)`，关闭恢复用户亮度
- ok-35 彻底改手动即时：去掉时段判定，开=压暗/关=恢复，交互 100% 明确
- 硬件限制：BOE MIPI 面板无暖色温寄存器 → 夜览只做背光亮度，无暖色

### 存储「白条」修复
未插卡/未挂载时 bar=0 → 只剩背景轨；背景 0x000000@LV_OPA_10 白色卡片上≈隐形 → 改 0xE5E5EA 不透明（ok-36）。

### 固化
- tag：ok-20260905-36（含 33~36 迭代整链；35/36 存储白条+夜览最终形态）
- 产物：vela.bin==nsh.fex（md5 4bfbf025）；check_res.sh 通过（23 提示音）
- Settings 此后无假开关；P179 起为最终交互

### 待上板验证
- 存储：插 TF 卡显示真实「已用 x / 共 y GB」+ 绿色进度条；无卡「未插入」+ 浅灰空轨
- 夜览：开立即变暗、关立即恢复、重启保持上次状态（cfg 持久化）