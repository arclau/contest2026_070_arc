# VOICE_DIRECTOR_SPEC.md — Voice Director 实现规格（Phase 3/4）

> 隶属 `VOICEDESIGN.md` **Phase 3** 交付物 · 2026-08-28
> 依据：`VOICE_INTERACTION_DESIGN.md`（§四 12 场景 / §五 决策规则 / §八.5 一日剧本）+ `VOICE_ASSET_PLAN.md`（§六 最小闭环 18）
> 本文件是 **Phase 6 实施的直接输入**：数据结构 / 函数签名 / 伪代码 / cfg 参数全表 / 持久化 / 集成点。
> 设计决策见上述文档；本文件只定"怎么实现"。

## 一、模块结构（luncher_dm 新增 dm_voice.c/h）

| 文件 | 职责 | 依赖 |
|------|------|------|
| `dm_voice.h` | voice_ctx_t / voice_decision_t / 对外接口声明 | **无 LVGL 依赖**（决策层可单测） |
| `dm_voice.c` | 场景分类 + 闭嘴检查 + 类别选择 + 加权随机 + 历史回写 | dm_health_cfg（读参数）、dm_health.h（事件）、deskmate_ui.h（dm_tone_play） |

**铁律（VOICEDESIGN.md §四）**：不修改 dm_tone_play / dm_tone_worker / dm_audio_fg / PTT 打断；dm_voice 只做决策层。

## 二、对外接口（函数签名定稿）

```c
/* 事件入口 */
void voice_director_on_seated(void);          /* 坐下事件（dm_health seated 判定成功）→ 决策 + 执行 */
void voice_director_on_reminder(int kind, int level); /* 提醒弹窗（0=水 1=坐）→ 保持现链路 + 音乐抢占判断 */
void voice_director_on_ai_interaction(void);  /* AI 对话开始时间戳（dm_ai 调用） */
void voice_director_on_music_changed(bool playing);  /* 音乐状态变化（music 模块调用） */

/* 查询 / 测试 */
voice_decision_t voice_director_decide(const voice_ctx_t *ctx); /* 纯函数，可单测 */
void voice_director_note_played(voice_category_t cat, const char *wav); /* 播放后回写历史/预算 */

/* 生命周期 */
void voice_director_init(void);   /* 加载 state、注册 cfg 读取 */
```

类型定稿：

```c
typedef enum { VOICE_NONE, VOICE_UI_ONLY, VOICE_PARTICLE, VOICE_SHORT, VOICE_FULL } voice_action_t;
typedef enum { CAT_GREETING, CAT_HEALTH_REMINDER, CAT_TEASING, CAT_CARING,
               CAT_SYSTEM_RESPONSE, CAT_LAST } voice_category_t;

typedef struct {
    voice_action_t   action;    /* 输出动作 */
    voice_category_t category;  /* 输出分类 */
    const char      *wav;       /* 选中 wav 名（不含路径）；NULL=静默/UI */
} voice_decision_t;
```

## 三、voice_ctx_t 定稿（含 budget/计数）

```c
typedef struct {
    bool  seated;
    int   sit_s;            /* 连续坐姿秒 */
    int   today_sit_cnt;    /* 今日坐下次数（新增统计） */
    int   water_cnt;        /* 今日喝水 */
    int   stand_cnt;        /* 今日起立 */
    int   away_s;           /* 本次离开秒数（离开起计时，回来时读取） */
    int   since_voice_s;    /* 距最近一次语音（含提醒） */
    int   since_ai_s;       /* 距最近 AI 对话 */
    int   decline_cnt;      /* 连续拒绝提醒次数（"继续坐/先不喝"） */
    int   budget[CAT_LAST]; /* 各分类今日已用计数 */
    int   recent_ids[8];    /* 最近播放 wav 轻量 id（环形，防重复） */
    bool  music_playing;
    bool  ai_busy;
    int   hour;             /* 当前小时 0~23 */
} voice_ctx_t;
```

> ctx 数据来源：dm_health（sit_s/today_sit_cnt/water/stand/decline）、dm_health.state（持久化计数）、
> dm_ai（since_ai_s/ai_busy）、music 模块（music_playing）、time()（hour）。收集逻辑在 dm_voice.c 内部完成。

## 四、cfg 参数全表（收敛自设计文档，全部走 dm_health_cfg）

> 组：S=场景 / Q=静默 / B=budget / C=cooldown / W=时段权重。key 即 `dm_health_cfg_get_int/str` 的键名。

### 4.1 场景判定参数（S）

| key | 默认 | 说明 | 引用 |
|-----|------|------|------|
| away_short_max_s | 300 | 短暂离开上限（≤此=场景 4） | §四 |
| away_long_min_s | 1800 | 长时间离开阈值（≥此=场景 5） | §四 |
| sit_long_for_revisit_s | 3600 | 久坐后回来判定（场景 6） | §四 |
| sit_cnt_quiet_th | 6 | 今日第 N 次坐下开始趋静（场景 7） | §四 |
| decline_revisit_s | 900 | 拒绝提醒后回来窗口（场景 12） | §四 |

### 4.2 静默检查参数（Q）

| key | 默认 | 说明 | 引用 |
|-----|------|------|------|
| recent_ai_interaction_cooldown | 300 | AI 对话后静默窗口（**默认参数，非硬规则**） | §5.1#1 |
| voice_cooldown_s | 600 | 两次语音最小间隔 | §5.1#3 |
| music_sit_silent | 1 | 音乐中坐下静默（0=允许粒子） | §5.1.1 |
| night_short_only | 1 | 深夜 23~5 只短句/粒子，不完整句 | §5.1#7 |
| greet_max_per_period | 2 | 时段问候限播（沿用 P102） | §5.1#6 |

### 4.3 voice budget（B，替代固定"每日 12 条"）

| key | 默认 | 说明 |
|-----|------|------|
| budget_greeting | 3 | 欢迎/问候/日 |
| budget_health_reminder | 0 | 0=按触发不设上限；**L3 强提醒不可被任何 budget 拦截** |
| budget_teasing | 4 | 吐槽配额（耗尽→caring/silent 权重↑） |
| budget_caring | 4 | 关心句/日 |
| budget_system_response | 5 | 粒子/轻回应/日 |

### 4.4 wav cooldown（C，单文件级）

| key | 默认 | 说明 |
|-----|------|------|
| cooldown_particle_s | 300 | 粒子 5min |
| cooldown_short_s | 1800 | 短句 30min |
| cooldown_reminder_s | 900 | 提醒 15min |

### 4.5 时段情绪权重（W，非提醒类）

| 时段 | hour | teasing | caring | silent | 说明 |
|------|------|---------|--------|--------|------|
| 早晨 | 5~10 | 0.5 | 1.0 | 0.8 | 轻松 |
| 上午 | 10~11 | 1.0 | 0.8 | 0.8 | 正常 |
| 中午 | 11~14 | 0.6 | **1.5** | 0.8 | 关心吃饭 |
| 下午 | 14~18 | **1.5** | 0.6 | 0.8 | 调侃活跃 |
| 晚上 | 18~23 | 1.0 | 1.2 | 1.0 | 关心收敛 |
| 深夜 | 23~5 | 0.2 | **2.0** | **2.0** | 温柔少话（night_short_only） |

> 权重是相对系数（×基础权重）；实现时 cfg 存 6 组 × 3 值，key 如 `w_morning_teasing=0.5`。上板后按体感调。

## 五、决策流程伪代码（函数级，Phase 6 直接照抄结构）

### 5.1 事件入口 voice_director_on_seated()

```c
void voice_director_on_seated(void)
{
    voice_ctx_t ctx = collect_ctx();            /* 收集 §三 数据源 */
    voice_decision_t d = voice_director_decide(&ctx);

    switch (d.action)
    {
    case VOICE_NONE:     /* 静默：UI 欢迎回来卡由 ui_home 统一处理 */ break;
    case VOICE_UI_ONLY:  ui_show_welcome_back(); break;
    case VOICE_PARTICLE:
    case VOICE_SHORT:
    case VOICE_FULL:     dm_tone_play(d.wav);
                         voice_director_note_played(d.category, d.wav);
                         ui_show_welcome_back();  /* 语音 + UI 可并行 */ break;
    }
}
```

### 5.2 纯决策函数 voice_director_decide()

```c
voice_decision_t voice_director_decide(const voice_ctx_t *ctx)
{
    int scene = classify_scene(ctx);                          /* ① 场景分类 §4.1 */
    if (!pass_silence_checks(ctx, scene))                     /* ② 闭嘴检查 §4.2/4.3 */
        return (voice_decision_t){ VOICE_NONE, 0, NULL };

    voice_category_t cat = pick_category(scene, ctx);         /* ③ 类别选择 §4.3/4.5 */
    const char *wav = weighted_select(cat, ctx);              /* ④ 加权随机 §资产表 */
    if (!wav)
        return (voice_decision_t){ VOICE_UI_ONLY, cat, NULL };
    return (voice_decision_t){ action_of(cat), cat, wav };
}
```

### 5.3 classify_scene()（12 场景判定骨架）

```c
static int classify_scene(const voice_ctx_t *c)
{
    if (c->since_ai_s <= cfg_i("recent_ai_interaction_cooldown", 300)) return 10;
    if (c->music_playing && cfg_i("music_sit_silent", 1))               return 11;
    if (!c->seated)                                                     return 0;  /* 非坐下事件 */
    if (!boot_greet_done(c))                                            return 1;  /* 开机首次 */
    if (c->today_sit_cnt == 1)                                          return 2;  /* 当天首次 */
    if (c->away_s <= cfg_i("away_short_max_s", 300))                    return 4;  /* 短暂离开 */
    if (c->away_s >= cfg_i("away_long_min_s", 1800))                    return 5;  /* 长离开 */
    if (declined_recently(c, cfg_i("decline_revisit_s", 900)))          return 12; /* 拒后回来 */
    if (sat_long_before_away(c, cfg_i("sit_long_for_revisit_s", 3600))) return 6;  /* 久坐后回来 */
    if (c->today_sit_cnt >= cfg_i("sit_cnt_quiet_th", 6))               return 7;  /* 多次趋静 */
    if (c->hour >= 23 || c->hour < 5)                                   return 9;  /* 深夜 */
    if (c->hour >= 18)                                                  return 8;  /* 晚上 */
    return 3;                                                           /* 普通回来 */
}
```

### 5.4 pass_silence_checks()（§5.1 顺序链）

```c
static bool pass_silence_checks(const voice_ctx_t *c, int scene)
{
    if (c->since_voice_s <= cfg_i("voice_cooldown_s", 600)) return false;      /* #3 */
    if (scene == 10 || scene == 11) return false;                              /* #1/#2 场景层已拦 */
    if (scene <= 2 && budget_full(c, CAT_GREETING)) return false;              /* #4 */
    if (budget_full(c, CAT_TEASING)) return false;                             /* #5 吐槽配额 */
    if (period_cnt_full(c)) return false;                                      /* #6 时段限播 */
    if (c->hour >= 23 || c->hour < 5)                                          /* #7 深夜 */
        night_short_only = true;   /* 类别选择只允许短句/粒子 */
    return true;
}
```

### 5.5 weighted_select()（防重复核心，§5.2 设计）

```c
static const char *weighted_select(voice_category_t cat, const voice_ctx_t *c)
{
    /* 候选池 = 该 category 的 wav 表（资产表 §六 最小闭环 18） */
    for (each candidate w) {
        if (in_recent_ids(w, c->recent_ids)) continue;      /* 剔除最近 8 次 */
        if (w_cooldown_pending(w)) continue;                /* 剔除单文件冷却 §4.4 */
        weight = base_weight(w) * period_weight(cat, c->hour); /* §4.5 权重 */
        if (budget_low(cat, c)) weight /= 2;                /* 配额将尽减半 */
    }
    return weighted_random(候选集);                          /* 加权随机 */
}
```

## 六、Presence FSM 细化 + 持久化升级

### 6.1 dm_health.c 现状（保留不动）

seated 判定（防抖 5s）→ g_leaving（离开暂停计时）→ absent 30s 清零（P125 用户拍板）。

### 6.2 新增统计（Phase 6 Step 1 在 dm_health.c 增加）

| 状态 | 维护点 |
|------|--------|
| g_away_s | seated→absent 起计时；回来 seated 判定成功时值入 ctx 并清零 |
| g_sit_cnt_today | seated 判定成功时 +1 |
| g_decline_cnt | 弹窗"继续坐/先不喝"时 +1；"我起来了/我喝了"时清零 |
| g_ai_ts | dm_ai 交互事件调 voice_director_on_ai_interaction() |
| g_last_voice_ts | voice_director_note_played() 内更新（任何语音后） |

### 6.3 dm_health.state 格式升级（向后兼容）

```text
旧格式: <day> <water> <stand> <seat_s>
新格式: <day> <water> <stand> <seat_s> <sit_cnt> <tease_cnt> <decline_cnt> <voice_cnt>
```

- **读取**：先按旧 4 字段 fscanf，再尝试读新增 4 字段；EOF（旧文件）→ 新增=0，**自动兼容**
- **写入**：health_state_save() 追加 4 字段
- **跨天**：沿用 g_state_day 逻辑，day 变化清零计数
- **budget 明细**：tease_cnt=budget[CAT_TEASING]、voice_cnt=今日语音总数；greeting/caring/system 明细存内存（重启清零可接受，跨天自动重置）

## 七、集成点清单（Phase 6 接线图）

| 集成点 | 现有代码 | 改动 |
|--------|----------|------|
| 坐下事件 | dm_health seated 判定 → DM_HEALTH_EVT_SEATED → ui_home.c standby_health_cb → `health_sit_greeting()`（现直接 dm_tone_play greet_*） | 替换 health_sit_greeting 内部为 `voice_director_on_seated()`；时段/限播逻辑移交 Director |
| 提醒弹窗 | DM_HEALTH_EVT_WATER_POP/SIT_POP → health_popup_show() → dm_tone_play(popup_*) | **保持直通**；加"音乐中 L2+ 抢占"判断（dm_audio_fg_acquire 已具备）；L3 不可拦截 |
| AI 交互 | dm_ai.c 对话开始/结束 | 调 `voice_director_on_ai_interaction()` 记时间戳 |
| 音乐状态 | music 模块播放/暂停/停止 | 调 `voice_director_on_music_changed()` |
| UI 欢迎回来卡 | ui_home.c standby 锁屏健康卡区域 | 新增卡片：坐下时显示"欢迎回来 · 今日坐姿 xx 分钟 · 喝水 x 次"，3~5s 消失 |
| cfg | dm_health_cfg.c 默认模板 | 追加 §四 全表 key（S/Q/B/C/W 五组） |
| 持久化 | dm_health.c health_state_save/load | 按 §6.3 升级格式（向后兼容） |

## 八、实施小步（Phase 6，每步编译打包固化）

| Step | 内容 | 涉及文件 | 验证 |
|------|------|----------|------|
| 1 | 数据结构 + cfg 参数 + state 格式升级（新增 g_away_s/g_sit_cnt_today/g_decline_cnt/g_ai_ts 统计） | dm_voice.h、dm_health.c、dm_health_cfg.c | 编译 + 串口确认 state 新字段落盘 |
| 2 | dm_voice.c 决策引擎（classify/检查/选类/加权随机，纯函数） | dm_voice.c/h | 编译（可加 uartdbg 式自测命令） |
| 3 | 接线：standby_health_cb 换 health_sit_greeting→on_seated；提醒加抢占判断；AI/音乐时间戳 | ui_home.c、dm_ai.c、music 模块 | 编译 + 上板行为观察 |
| 4 | UI 欢迎回来卡 + 弹窗文案银月化（设计 §6.6 表） | ui_home.c | 编译 + 上板视觉确认 |
| 5 | AI system prompt 银月化（角色段）+ 资源落地（重录 6 + 新增 6 进 res/tones） | context_builder.c（ai_agent）、res | check_res.sh + 上板一日剧本走查 |

> 每步结束：编译 → 打包 → check_res.sh → git_snapshot 固化。Step 5 的 wav 依赖用户 tts_proto 合成回填，可单独排期；Step 1~4 用现有 wav 即可跑通 Director 机制（最小闭环第 0 版）。

*DevLog by AtomCode (deepseek-v4-flash)*
