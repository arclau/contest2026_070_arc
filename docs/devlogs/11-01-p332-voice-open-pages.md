# P332 — 语音开页面（低成本批）

2026-11-01；V1 `ynm3000`。承接 P331 的本地命令链路，只加"导航"这一层：
`show_subpage()` / `close_subpage()` 本来就存在，缺的只是语音入口。

## 新增命令

| 说 | cmd | 动作 |
|---|---|---|
| 打开书本 / 看书 / 读书 | `app:books` | `show_subpage("Books")` |
| 打开日历 / 日历 | `app:calendar` | `Calendar` |
| 打开串口 / 调试工具 / UART | `app:uart` | `UART` |
| 打开设置 | `app:settings` | `Settings` |
| 打开文件 | `app:files` | `Files` |
| 打开音乐 | `app:music` | `Music` |
| 打开闹钟 | `app:alarm` | `Alarm` |
| 打开宠物设置 | `app:pet` | `Pet` |
| 打开对话 | `app:ai` | `AI` |
| 打开无线网络 / 蓝牙 | `app:wifi` / `app:bt` | `WiFi` / `Bluetooth` |
| 回桌面 / 退出 / 关掉 | `app:home` | `close_subpage()` |

## 实现

- agent 侧 `match_app()`（`voice_channel.c`），放在 matcher 表**最前**：
  歧义词（音乐/闹钟/设置/文件）必须带开启动词（打开/进入/看看/去）才命中，
  避免"我设了闹钟"被当命令；书本/日历/串口/回桌面歧义小，直接命中。
- UI 侧 `dm_cmd_app()`（`deskmate_ui.c`）+ 命令表加 `{ "app:", 0, dm_cmd_app }`。
  只调页面入口，不碰各页内部状态；页面不存在时走 `show_subpage` 的"敬请期待"兜底。
- 命中后播 ack（"打开日历"），沿用 `needs_ack=0`——导航类不打断音乐。

## 追加：口语化音量/亮度 + 音乐别名（同日）

用户要求"音量/亮度的自然说法"和"大声点/小声点"这类别名。

- 新增 `match_comfort()`（`voice_channel.c`），**按明确极性**映射，不走
  `dev_adjust_parse` 的方向词推断——"太亮了"含"亮"会被判成调高，反了。
  步进 ±10（口语诉求比"调一点"的 ±5 更贴切）：
  - 音量↓：太大声 / 太吵 / 吵死 / 小点声 / 小声点 / 声音小 / 轻一点 / 安静点
  - 音量↑：大声点 / 大点声 / 声音大 / 听不清 / 听不见
  - 极值：静音 / 关掉声音 / 别出声 → 0；最大声 / 声音开到最大 → 100
  - 亮度↓：太亮 / 刺眼 / 晃眼 / 暗一点 / 调暗
  - 亮度↑：太暗 / 看不见 / 亮一点 / 亮一些 / 调亮
- 音乐别名：play 增"来点音乐/放首歌/放点歌/听歌/接着放/开始放"；
  pause 增"别唱/关掉音乐/安静/不要放了/关音乐"；
  next 增"切下一首/换一个/不想听这个"；prev 增"回到上一首/再听一遍/重放"。
- **修一个冲突**：`match_music_pause` 原来含"停下"，会把 P331 的 `pet:stop`
  抢走（music 在 matcher 表里更靠前）。已把"停下"从音乐暂停里移除，
  归宠物；音乐暂停保留"停一下"。

## 验证

- 编译 + `pack_deskmate.sh ynm3000` 全项 PASS；仅剩既有告警
  （`deskmate_ui.c:1190/1271` 的 `%d` vs `int32_t`，P332 之前就在）。
- 镜像 `output/ynm3000/ynm3000.img` MD5 `14f0d8ce4d52616e2767f05c9b5f7cf5`
  （含口语化音量/亮度与音乐别名）
- **未刷板**。锁屏状态下开子页的行为未验（cmd 轮询 timer 是全局的，
  理论上锁屏也能开页），板端要看一眼是否需要"锁屏时先解锁再开页"。

## 还没做（按成本排序，供下一轮挑）

1. **零成本**：`music:` 已有 play/pause/next/prev —— 只差把"声音小一点/大一点"
   的自然说法补进 matcher（`dev_adjust_parse` 已支持"调到五十"）。
2. **低成本**：天气/时间问答（agent 有 `get_weather` / `get_current_time` 工具，
   但那是 LLM 工具调用；本地短路可以再快一档）。
3. **中成本**：亮度/音量之外的系统项——锁屏、静音、勿扰。
4. **中成本**：拍照/截图（`dm_screenshot.c` 已有 LRADC 触发全屏 BMP，加语音入口即可）。
