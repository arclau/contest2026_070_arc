# 9-10 part1 devlog — P207 全应用审计+修复包：快问/城市/蓝牙WiFi/设置/音乐/UART/文件/Games/报告/README/MiMo/A2DP交接

> 日期：2026-09-10 ｜ 节点：P207 ✅（代码冻结，转上板验证）｜ 固化：ok-20260910-13~24（vendor 三仓）+ ai_agent 4 commit
> 基线：P206 ok-20260909-10 → 本日 12 次 snapshot；会话结束转新会话继续 verify_checklist 上板验证

## 一、AI 对话快问（ok-1）
- 键盘无拼音 → 放弃打字框，改 4 点按快问（现在几点/今天天气/播放音乐/讲个笑话），走 `dm_ai_ask` 文本链路（agent fast-path，免 search key）。
- `ai_poll_cb` 文本回复只回状态不显示 → 追加对话区回显（文本通道无 TTS/llm 事件，此为唯一回显路径）。
- 对话缓冲上提共用 + 进页清空 + 滚到底；Dock“AI”改名“AI对话”。

## 二、城市天气 A 方案（ok-2）
- 无 GPS 解法：Settings→通用→城市（12 城，默认惠州，落盘 `weather_city_idx`），切完即时刷天气卡。
- 天气卡 URL 改运行时拼坐标；state 落盘加城市字段；agent fast-path 无城市问天气读本地落盘（免 key），`/weather` 无参同理，prompt 带城市。
- 快问“北京天气”→“今天天气”（走本地路径）。

## 三、全应用审计 + 修复包（ok-3，四路并行实审）
- 蓝牙：Other Devices 恒空（`g_bt_scan_done` 永不置1→补 STOPPED 回调）、连接恒“正在连接…”（加 10s 轮询回读+超时）、加断开按钮（配对行内）。
- WiFi：连上后 `save_conf(cur,NULL)` 覆空 psk 致重启连不上（删）、忙返回 0 冒充成功（改 -1）、加断开按钮（`wifi_disconnect`）。
- 主屏：永不锁反杀（`idle_timeout=0` 直接返）、删假电池图标、无传感器显“室内 --”、星期汉化、日期行去箭头。
- 音乐：无曲目静默（调 `update_track_info` 刷暂无曲目）；播放列表序号/DOT/计数/加宽（DM180→240）。
- UART：fd 泄漏（close 补）、假暂停（reader 真停）、盲发（[TX]回显+空不发）、open 失败错显、波特率落盘。
- 文件：`system(mkdir)`→mkdir(2)、`/`/resource/etc/proc/sys/dev guard + 确认框带路径。
- 音量/亮度落盘 + 开机恢复（luncher_dm init）。
- 附带：`libapps.a` P190 同类污染（9 旧 `.o` 在前致新码不进固件），`/tmp/dedup_ar.py` 去重 + 二进制验串。

## 四、Games 来回（ok-5/6/7）
- 死页下掉 → 用户想游戏机 → 查官方树无游戏 demo → 手写 2048（方向键版，300 行）→ 用户嫌不加分 → 全砍（删文件+去接线）。Dock 回 6 入口。

## 五、报告/README 去虚（评审逐条对代码）
- 报告：固件 17.5MB/整包 50MB 实测；性能表标估计待复测；删唤醒词声明（纯 PTT）；67 万行拆 8 万手写+59 万精灵；token 未统计承认；8 子页→6 应用+3 嵌套；补团队分工（单人）。
- fork README 按现设备重写（功能清单+明确不做+路线对照表）；xuqiu 加 supersede 头；毒舌调侃池下掉 tease_01。

## 六、MiMo 来回（未落地，冻结）
- 预置位更新（mimo-v2.5→pro），`is_openai_compat_host` 早含 xiaomimimo，协议零改动。
- 用户无 MiMo key + 旧 log 额度用完 → 切回方舟 deepseek（`agent_config.h` checkout 还原，secrets 未动）。
- Ark 备份 `/tmp/agent_secrets.ark-backup.h`；教训：改默认前先确认 key 在手。

## 七、AI 对话 rev（ok-11，dm_ai.c 820 行精读）
- 修 4 bug：spins 耗尽静默（走 fail）、b64 少 1 字节、unsubscribe fd 复用误伤、跨页旧回复串页。
- 故意不动：REQ 256 截断、`\u` 转义、PTT 500ms 忙等（防录音卡死设计）。

## 八、蓝牙音箱方向（半套，A2DP 不做）
- 用户拍板被人连；探坑：sink profile 自注册、SBC 虚表干净、fluoride 解码器现成——但 bluetoothd 独立进程（PCM 过进程是大块）+ R528 无 a2dpsnk（offload 死）+ 44.1k→48k 重采样。
- 结论 9.20 前不跳；交接包 `project_docs/specs/wireless/A2DP-SINK-PLAN.md`（含接手 AI 提示词）。

## 九、TTS 根因（ok-23，板上实锤）
- `ws_upgrade` 等服务端先推帧，但 TTS V3 是客户端先发 StartConnection → 空等撞 10s 超时（-EIO，TTS 永失败）。改头齐即返。
- 板上 `ai_agent --test tts/llm/full` NSH 直测可用（vela 控制台难进）。

## 十、WiFi 预置 + 上板验证启动 + 910bootlog 两实锤（ok-20）
- 910bootlog 体 `body={"error":true,"reason":"Invalid timezone"}`：`Asia%2FShanghai`
  编码斜杠被拒 → 改直写（天气自 8-10 从未成功过，一直假数据）；加 body 头 64B 打印。
- 宠物消失实锤：先有鸡先有蛋（`pet_core_init` 只在 create 内，门控读未初始化零），
  存档一丢即死锁 → 开机即 init（幂等）。
- 附带：蓝牙驱动 open 超时（filep_h5 -110，本机蓝牙可能没起）、光感 lux 恒 0（暗光误弹）、
  A2DP-Sink 注册成功、WiFi 自连 wifi-home 正常、麦录音正常（128000B）、DNS 时好时坏。
- UDISK `wapi.conf` 配 wifi-home/手机号（ok-24，push 前必还原，见提交检查清单 §三）。
- `project_docs/verify_checklist.md`（A~J）+ 清单同步 ok-22；宠物子页独立（ok-19，Settings 一行入口，顺带修起名页退出野指针）。
- 待新会话：A~J 逐项打勾（宠物init/城市/快问待板上确认；TTS 待 `--test tts` 结果；MiMo 待 key）。

## 待办（新会话）
1. 烧 ok-24，按 verify_checklist A~J 打勾。
2. 脱敏 push（主人→主人 + UDISK wapi 还原 + 重 pack）→ 用户点头。
3. 视频 + 真机照片 + 报告收尾。
