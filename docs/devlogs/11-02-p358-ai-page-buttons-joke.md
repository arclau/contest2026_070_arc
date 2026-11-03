# P358 AI 页撤本地宠物按钮 + 快问"不出声/无回复"两个协议缺陷

目标：按用户要求撤掉 AI 语音页多出的四个本地宠物按钮；修「讲个笑话」点了没反应。
平台：V1 `ynm3000` / `luncher_dm`；V2 不改。

## O：现象与证据

- 板端跑的是 P354 镜像（`bg_reward_std.wav` 在 07:38:21 出现，是 P354 新增的本地确认音），
  所以 AI 页确实多了「坐下/睡觉/散步/回来」四个按钮（`create_ai_subpage` 里 P354 新增的一行）。
- 「讲个笑话」无回复，串口唯一一条：
  `07:30:54 [ws] No WS client for chat_id=ui`。时间线：07:30:40 打开 AI 页 →
  07:30:47 `IDLE -> CURIOUS (ai-chat)`（本轮对话开始）→ 07:30:52 agent 回复时
  客户端已不存在 → 回复被丢。

## H 与结论

H1：UI 侧只等一帧响应，收到"正在思考中"就当成答案并断开连接。
H2：快问带 `"speak":1` 但 agent 只认布尔 true，所以文字通道从不播报。

两条都在代码里坐实，不是推测：

1. `agent_loop.c:send_working_status()` 对 websocket 通道会先推一条
   `正在思考中...`；`dm_ai.c:ai_worker()` 收到第一帧 `type=response` 且 content
   非空就 `g_ai_ok=1; break;` → 关 fd。真正的回复随后 `ws_server_send` 找不到
   chat_id=ui（客户端已 remove）→ 丢弃。UI 上只看到「正在思考中…」。
2. `ws_server.c` 用 `cJSON_IsTrue(speak_item)` 解析 `speak`，而 UI 发的是数字 1
   （`dm_ai.c` 的 `\"speak\":1`）→ 恒为 0，`speak_async()` 从不执行。

## 改动

- `deskmate_ui.c`：删除四个本地宠物按钮（含 `ai_local_click_cb`）与其对齐循环；
  `chat_cont` 高度/位置恢复为 DM(180) / -DM(85)，即 P354 之前的两行布局。
  `dm_local_cmd_dispatch()` 保留（语音命令、cmd 轮询、快问仍用）。
- `agent_loop.c`：`send_working_status()` 增加 websocket 通道早退。UI 自己已经在
  状态栏显示「思考中…」，这条状态对 UI 没有信息增量，只会毒化唯一等待帧。
- `ws_server.c`：`speak` 同时接受布尔 true 与非零数字。

## 验证与产物

| 项 | 结果 |
|---|---|
| V1 编译/打包 | PASS，121 资源，入口/启动/整包校验通过（`output/p358-build.log`） |
| 重编确认 | `deskmate_ui.c` / `ws_server.c` / `agent_loop.c` 三个 .o 均为 07:41 重建 |
| 镜像 | `output/ynm3000/ynm3000.img`，MD5 `a9e23b0203f8b280a932ddcd985e68cc`（含改名全量落地） |
| 板端 | 未刷；AI 页按钮数量、快问出声与回复待验 |

## 待验清单

1. AI 页只剩一行四个快问（现在几点 / 今天天气 / 播放音乐 / 讲个笑话），无宠物按钮。
2. 点「讲个笑话」：气泡出现笑话正文（不是"正在思考中"），且听到 TTS。
3. 串口**不再**出现 `No WS client for chat_id=ui`。
4. 点「现在几点」「播放音乐」仍是本地秒回，不受影响。

## 追加（同轮）：默认名 银月 → 维拉

用户实测说「银月」被 ASR 听成「音乐」，要求换关键词。

- 20 处用户可见/提示词引用全部改名：UI 对话气泡、宠物默认名、`ui_pet`/`ui_settings`、
  `context_builder.c` 人设头、`memory_store.c` 默认 SOUL/MEMORY、打包的 `SOUL.md`。
- 全量统一：UI 气泡/宠物名/设置页/提示词/默认 SOUL 与 MEMORY/打包 SOUL.md/cfg 模板注释/VOICEDESIGN 人设段 全部改名；
  仅保留两处旧名常量（改名迁移比对用）与历史 devlog 记录（不重写历史）。
- 迁移：`pet_core.c` 读档时若存档名仍是「银月」则改成「维拉」（用户自起的名字不动）；
  `memory_store.c` 新增 `migrate_legacy_name()`，对已存在的 `SOUL.md`/`MEMORY.md`
  就地替换旧名——`ensure_file` 不覆盖已存在文件，不迁移会出现"提示词叫维拉、
  人设文件叫银月"的自我矛盾。
- 「维拉」与 vela 的读音：vela ≈ /ˈviːlə/，中文近似"维拉"（wéi lā），首尾都对齐；
  比「韦娜」(wéi nà) 更贴，且与"音乐"不再撞音。

## 备注

P354 的离线本地 API 能力没动，只是撤掉了 AI 页的四个入口。若还要离线入口，
应另择位置（用户未确认），不要在 AI 页恢复。
