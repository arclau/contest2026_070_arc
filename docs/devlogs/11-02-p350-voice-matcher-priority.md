# P350 语音本地命令表：优先级错位 + 死代码

> 起因：P349 之后做一次"同型 bug 巡检"（用户："这套体系还是有很多类似 BUG，化身 debugger 巡检一波"）。
> 方法：写脚本把 `voice_channel.c` 里 9 张词表全部解析出来，复制 `voice_local_cmd_parse()`
> 的归一化 + 否定过滤，然后把每条关键词和 23 条真实口语逐条灌进优先级链，看谁被谁吃掉。

## 1. `match_volume` 抢走了 `match_comfort`

链上顺序是 `match_comfort → match_volume → match_brightness`（修复前 comfort 在后）。
`dev_adjust_parse()` 的判定是"设备词 + 方向词"，而 comfort 表里大量词条整句都含"声音"：

| 说 | 期望 | 实际（修复前） | 原因 |
|---|---|---|---|
| 声音开到最大 | `volume:100` | `volume:+5` | comfort 的"声音开到最大"没轮到，被 volume 的"+5 相对步进"先拿走 |
| 声音小 | `volume:-10` | `volume:-5` | 同上 |
| 声音大 | `volume:+10` | `volume:+5` | 同上 |

comfort 的词更长更具体，按"具体词在前"的既有约定（P333/P334 注释里写过），
`match_comfort` 必须排在 `match_volume` / `match_brightness` 之前。

```c
/* P350：comfort 必须排在 volume/brightness 前面。两者都靠"音量/声音 + 方向词"
 * 推断，而 comfort 表里的"声音小""声音大""声音开到最大"整句都含"声音"，
 * 排在后面会被 dev_adjust_parse 抢走：说"声音开到最大"只 +5（期望 100），
 * 说"声音小"只 -5（期望 -10）。comfort 的词更具体，先给它。 */
match_comfort, match_volume, match_brightness,
```

## 2. `match_pet` 的"分钟→秒"解析是死代码

`match_pet()` 里本来就有一段把 `pet:walk` 换成 `pet:walk:<秒>` 的解析，
但它挂在 `for (i…) strstr(text, t[i].word)` 命中之后。口语"走五分钟"里
既没有"走路"也没有"散步"，词表一条都匹配不到 → 解析永远跑不到。

补一条前置分支（只认"走"开头且带"分钟"，避免"走开""走啦"被当成散步）：

```c
if (strncmp(text, "走", 3) == 0 && strstr(text, "分钟"))
    { ... cn_num(buf, &secs); secs *= 60;
      snprintf(cmd_out, cap, secs>0 ? "pet:walk:%d" : "pet:walk", secs); return 1; }
```

## 3. 顺带确认没问题的

| 项 | 结果 |
|---|---|
| `g_pet_asset_files[]` / `g_pet_png_expect[]` / `g_pet_asset_count` / `PET_PNG_EXPECT_COUNT` / 磁盘 PNG | 82 / 82 / 82 / 82 / 82，一致 |
| `pet_png_crc.h` ↔ 磁盘 ↔ `res.fex` | 82 条全对：size 不符 0、crc 不符 0、res.fex 缺内容 0 |
| `dm_pet_state_t` 11 个状态 | 全部可达，且 `pet_select_behavior()` 全部有映射 |
| 9 个"config.json 有、CFG_* 没读"的键 | 实际经 `AGENT_CFG_KEY_*` 宏读（`llm_router.c`/`ui_ai_cfg.c`），不是死键 |

只有 `AGENT_CFG_KEY_SEARCH_KEY`、`AGENT_CFG_KEY_VOLC_CLUSTER` 两个宏是定义了没人用；
CLI 侧 `cmd_set_search_key` 走的是 `tool_web_search_set_serp_key()`，功能没断。

## 4. 死代码（不影响行为，但注释在骗人）

| 位置 | 情况 |
|---|---|
| `voice_channel.c` `s_wake_triggered` | 2751 置 1、2804 清 0，**没有任何读取**；唤醒后是直接 `agent_task_create(asr_and_dispatch)` 派发的，这个标志是旧设计的残留 |
| `pet_view.c` `g_last_tap_ms` | 声明后从未使用 |
| `pet_view.c` `g_tap_count` | 9 处赋值、0 处比较；但 814 行注释还写着"双击或三连点直开喂食"——**功能已经没了，注释还在** |

## 验证

- 静态：`output/p350/matcher_shadow.py` 修复后重跑，"声音开到最大"→`volume:100`、"声音小"→`volume:-10`、"走五分钟"→`pet:walk:300`。
- 板端：待刷 `91d1db40` 后口述验证。
