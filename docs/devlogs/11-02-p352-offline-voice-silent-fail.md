# P352 断网时语音静默失败

> 用户："断网的情况下语音指令好像用不了啊。你看 logs"

## 先给结论：语音 100% 依赖云

| 环节 | 实现 | 断网可用 |
|---|---|---|
| ASR | `xiaomi_asr` → `api.xiaomimimo.com:443`；另有 `volc_asr` | ✗ |
| LLM | MiMo（SSE） | ✗ |
| TTS | 云 | ✗ |
| "本地命令短路" `g_local_cmd_matchers[]` | 只是**跳过 LLM**，输入仍是 ASR 文本 | ✗ |

关键点：`voice_local_cmd_parse()` 在 `voice_channel.c:1328` 和 `1741` 两处，入参都是
`voice_asr_recognize()` / `voice_asr_stream_finish()` 的输出。**本地短路短路的是 LLM，
不是 ASR**——所以「坐下/睡觉/散步」这类宠物指令断网一样用不了。

串口统计（`p351-0452` 会话）：

```
5 次 TURN begin
5 次 pre-open ASR failed
4 次 [xiaomi_asr] request failed: -1
4 次 ASR failed or empty: -5
1 次 no PCM data for batch ASR
4 次 net_connect api.xiaomimimo.com:443 ret=0x52
0 次成功识别
```

## 真 bug：失败是静默的

原来 ASR 失败只做两件事：

```c
ws_server_broadcast_voice_evt("pet_idle", "");
syslog(LOG_WARNING, "[%s] ASR failed or empty: %d\n", TAG, ret);
```

`pet_idle` 让猫回到 idle，**没有任何用户可见提示**。主屏/锁屏按完语音键，
用户看到的就是"猫闪一下就没了"。

更隐蔽的是：`deskmate_ui.c:365` 本来有联网拦截（2026-08-12 加的），但
`dm_net_wifi_connected()` 只判 `wlan0` 有没有 IPv4（`dm_net.c:807`）：

```c
if (netlib_get_ipv4addr(WLAN0_NAME, &ip) != 0) return 0;
return ip.s_addr != INADDR_ANY;
```

**连着 AP 但路由器没上行 / DNS 挂了 → 返回真 → 拦截放行 → 白录一遍再失败。**
这正是本次的现场。

## 修改

**agent 侧**（`voice_channel.c`）加统一失败上报：

```c
static void voice_report_asr_failure(int ret)
{
    const char *msg = ret < 0 ? "网络连不上…" : "没听清…";

    /* 自己打一行，验证时不依赖 ws_server 的广播日志（实测那行从未出现过）。 */
    syslog(LOG_INFO, "[%s] ASR failure -> bubble: %s\n", TAG, msg);
    ws_server_broadcast_voice_evt("pet_idle", "");
    ws_server_broadcast_voice_evt("error", msg);
}
```

三个失败点接上：`ASR failed or empty`、`stream ASR failed`、`no PCM data`。

**UI 侧**（`dm_ai.c` / `dm_ai.h` / `deskmate_ui.c`）：新增 `kind="error"` 通道
（不复用 `llm`，语义别混），由**系统级** timer `dm_ai_cmd_poll_cb`（300ms）消费，
调 `pet_view_show_bubble()` 弹宠物气泡——这样主屏、锁屏、AI 子页都能看见。

## 验证

- 断网（或连 AP 无上行）按语音键 → 宠物气泡出现「网络连不上…」，
  串口出现 `[voice] ASR failure -> bubble: 网络连不上…`。
- 正常联网但没说话 → 「没听清…」。
- 镜像 `15d60475` 待刷。

## 一个待解的日志疑点

`ws_server_broadcast_voice_evt()` 里有一句
`[ws] voice_evt(%s) broadcast to %d client(s)`，但**全部历史串口日志里一次都没出现过**。
同时 `pet_listen`/`pet_think` 确实送达了 UI（证据：`pet_set_action ... reason=voice-phase`
共 8 次），说明通道本身没问题，只是这行日志在当前固件里没有输出。
所以 P352 的验证日志放在 `voice_report_asr_failure()` 自己身上，不依赖它。

## 遗留 / 可选

- **真离线语音**：要断网也能控宠物，得在本地做关键词唤醒（KWS）或离线小模型 ASR，
  属于新功能，不是这次修 bug 的范围。
- `dm_net_wifi_connected()` 只判 IP 不判连通性，这个语义问题还在——所有依赖它的
  拦截（天气、语音、设置页状态）都会在"连 AP 无上行"时误判为在线。要彻底解决
  得加一个后台可达性探测 + 缓存标志，影响面比这次大，先记为已知问题。
