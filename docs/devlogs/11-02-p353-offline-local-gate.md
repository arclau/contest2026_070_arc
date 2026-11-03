# P353 断网时本地能力被联网门禁误拦

> 用户："至少断网，不走AI直接做本地调用API还是可以的吧？"
> —— 对。上一轮我把「断网语音不可用」讲成了「断网什么都干不了」，表述误导了。

## 事实：断网时能做的事远多于能触发的事

`g_dm_local_cmds[]`（`deskmate_ui.c:2206`）里有 8 类执行器，全部纯本地：

```c
{ "app:",        0, dm_cmd_app },          /* 开页面      */
{ "music:",      0, dm_cmd_music },        /* 本地曲库    */
{ "volume:",     1, dm_cmd_volume },       /* 硬件音量    */
{ "brightness:", 1, dm_cmd_brightness },   /* PWM 背光    */
{ "pet:",        0, dm_cmd_pet },          /* 宠物动作    */
{ "scene:",      0, dm_cmd_scene },        /* 场景组合    */
{ "health:",     0, dm_cmd_health },       /* 健康打卡    */
{ "alarm:",      1, dm_cmd_alarm },        /* 闹钟        */
```

连确认音都有本地实现：`dm_tone_play()`（`deskmate_ui.c:3298`）播
`/resource/tones/<name>.wav`，预合成、带声卡仲裁，完全不碰网络。

**唯一的硬依赖是 ASR**——因为命令匹配器吃的是 ASR 出来的文本。

## Bug：纯本地的动作被一道一刀切的门禁挡掉

`ai_chip_click_cb`（`deskmate_ui.c:236`）是快问 chips 的点击回调，顺序是：

```c
if (!dm_net_wifi_connected()) { ...return; }   /* ← 先判网络 */
...
if (strcmp(q, "播放音乐") == 0)                /* ← 本地曲库，注释写着"离线可用" */
    { music_play(music_track_id); ... return; }
```

断网时第一道门禁就 return 了，**下面那个明确写着"离线可用"的本地分支永远到不了。**

## 修改

引入 `local_only` 标记，只对纯本地的问句跳过网络门禁，其余（天气/笑话，要云端）
仍照旧拦截：

```c
/* P353：纯本地的快问必须放在联网门禁前面。
 * 「播放音乐」走本地曲库 music_play()，下面的注释自己写着"离线可用"，
 * 但断网时被门禁挡掉 → 明明能做的事白按。 */
int local_only = (strcmp(q, "播放音乐") == 0);

if (!local_only && !dm_net_wifi_connected()) { ...return; }
if (dm_ai_busy()) { ...return; }
...
if (local_only) { music_play(...); ... return; }
```

`dm_ai_busy()` 的检查位置没动（忙时仍然先拦），只放宽了网络这一道。

## 验证

- 断网（或连 AP 无上行）打开 AI 页点「播放音乐」→ 应播放本地曲库，
  对话区显示「正在播放本地音乐」或「曲库是空的…」，**不再提示"未连接 Wi-Fi"**。
- 点「今天天气」「讲个笑话」→ 仍提示「未连接 Wi-Fi，请先联网」（这两条要云端）。
- 镜像 `792babc6` 待刷。

## 后续（没做，留给优化）

真正的"断网也能用"还差三块，详见
`project_docs/devlogs/11-02-handoff-for-optimization.md` §4：

1. **屏幕按钮直连 `ai_local_cmd_dispatch()`** —— 一行调用，零网络，投入最小。
2. **断网确认音改本地 wav** —— `voice_local_cmd_ack()` 现在走云端 TTS，断网时
   动作执行了但没声音。
3. **本地关键词唤醒（KWS）** —— 真正解决"断网也能说"，但要模型。
