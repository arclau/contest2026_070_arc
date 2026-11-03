# DevLog — R528 DesktopMate 节点索引（当前：V1 主线）
> 先读 AGENTS，再读本表，按任务点开详情。**交接/接手先看首行「进度」快照**，那里有当前固件、待验证清单和已知问题。V1（P1～P243）归档于 `project_docs/archive/v1/`。 本表记录事件，不替代 ROADMAP；“实现/构建/运行”分开判断。历史 tag 不自动代表当前可用。
> **进度快照（2026-11-02）**：V1 主线；源码已冻结（P364），固件 `ccb08b24`（53,149,696 B）。
> 语音链路：`[voice-evt]` 每事件单发、listen/think/speak 起身、播完 `voice-sleep-back` 实测 4 次、P362 衔接差 101ms。
> 已知问题：AI 回复说英语（本地短路 1ms、正文未落日志，根因未知）、NAND 坏页 PNG、偶发 xrun。
| P | 结论与验证状态 | 固化 | 详情 |
|---|---|---|---|
| P370 | 专属仓交接快照：V1源码镜像与159份日志在本地未提交；记录映射/历史缺口；此后devlog仅本地维护 | 文档快照 | [详情][p370] |
| P369 | 专属仓 README 改为 V1 冻结基线与二次开发现状；修正构建入口，标出板配置映射和日志历史缺口 | 文档已验 | [详情][p369] |
| P368 | 公开前日志筛查：268份中移出109份，保留159份/33,720事件；旧工具载荷与敏感/归责消息已隐去 | 本地已验 | [详情][p368] |
| P367 | 用户决定先保留全部日志：268份/68,350事件；曾移出的会话已恢复，后续再筛选 | 本地已验 | [详情][p367] |
| P365 | Codex 补采39份，CodeBuddy 入仓41份；本次会话已采集，隐私与全仓日志校验通过 | 本地已验 | [详情][p365] |
| P364 | 源码冻结，镜像 `ccb08b24`；专属仓已同步 V1 APP、AI Agent、defconfig、devlog/docs，未提交/推送 | 镜像已验 | [详情][p364] |
| P363 | **语音回复时宠物不起身**：订阅曾静默失效（ESTABLISHED 但无动作）；加 ping 保活+`[voice-evt]` 诊断；首版自愈用 `fd<0` 误判→双订阅，事件全双发、动作同 tick 选两次、missing frame 4→15，改 worker 心跳+线程数守卫；hold=2 只压"去睡觉"那声回应（`VOICE_PHASE(0)` 解除）；睡着被叫醒→播完 `voice-sleep-back`；板测：每事件单发、每相位只选一次动作、sleep-back 4 次 | 已验 | [详情][p363] |
| P362 | **语音播报句间空档**（用户"一卡一卡的"）：串口实测每轮切两段送 TTS，第二段重付 TLS 首包 ~1s，而第一段已播完 → 中间硬空一秒；第一句改 `speak_async` 不阻塞 LLM 流，收尾段改**边收边放**（攒够 1s 就 trylock 接上播放，接不上继续攒），`session_finish` 等最后一段播完再广播待命；同轮修 UI 空转：状态文字只在变化时重设、对话区历史收到 1600 字节；板测衔接差 101ms，已并入 `ccb08b24` | 已验 | [详情][p362] |
| P361 | **天气地点改北京**：`DM_CITY_DEFAULT_IDX` 0→3（表里本有北京 39.90/116.40，不动序号），`dm_city_cur_idx()` 把已落盘的旧默认 0 一次性迁到北京并打标记；AI 侧读 `/data/dm_weather.state` 的城市字段自动跟随，另改 `USER.md` 默认地点并泛化 `migrate_replace` 处理老文件（顺带修掉不等长替换留尾巴的坑）；已并入冻结固件 `ccb08b24` | 已验 | [详情][p361] |
| P360 | **天气图标重绘**：64×64 灰云 → 96×96 分色（晴琥珀/云蓝灰渐变/雨宝石蓝/雪冰蓝/雷暴紫+金闪电），圆与线做覆盖率抗锯齿、闪电 4×4 超采样，多云画成太阳被云遮住；顺带修掉雪分支不可达（雨区间 51..82 吃掉 71..86）；纯代码绘制不读 NAND；镜像`a2599d82`待刷 | 待验 | [详情][p360] |
| P359 | **壁纸常驻 + 播报前导音**：`blue-hour.png` 开机解一次存 `lv_image_dsc_t`（8.8MB），绘制不再读 NAND/解码；`dm_voice` 粒子/关心类裸人声改为先响 `bg_reward_std`；板上首版壁纸失败（res 读坏+WARN 被编译掉）→ PNG 编入 `.rodata` 不再读 NAND；镜像`473235ff`待刷 | 待验 | [详情][p359] |
| P358 | **AI页撤四个本地宠物按钮**；「讲个笑话」无回复＝agent先推"正在思考中"被UI当答案后断连（`No WS client for chat_id=ui`），且`speak:1`数字被`cJSON_IsTrue`判假→从不播报；同轮默认名银月→**维拉**（银月被听成音乐）含存档/SOUL迁移；镜像`a9e23b02`待刷 | 待验 | [详情][p358] |
| P357 | 停桌面后panic、重启恢复；cmp误报获完整64KB校验确认，G2D对照无效 | 板测 | [详情][p357] |
| P356 | ROMFS重读仅用cmp判差异，因已知误报撤回读坏结论 | 证据撤回 | [详情][p356] |
| P355 | 板端核验：对话首音单轮3.69s；宠物取帧失败1442条/15分，读路径根因未定 | 测量 | [详情][p355] |
| P354 | 本地 API/确认离线化，四个宠物按钮直连；宿主与V1打包过，SIM构建受阻，镜像14ef2705待刷 | 待验 | [详情][p354] |
| 优化交接 | **给外部优化者的系统说明**：两个进程（UI ↔ agent，127.0.0.1:28789）边界、一次语音指令的完整链路（标出哪段在云端）、**断网能力矩阵**（命令匹配 + 8 类执行器 + 本地提示音全部离线可用，唯一硬依赖是 ASR）、三个离线落地点（屏幕按钮直连本地 API / 本地 wav 确认音 / 本地 KWS）、待优化清单与权衡、硬约束 | 交接 | [文档][handoff] |
| P353 | **断网时本地能力被联网门禁误拦**：`ai_chip_click_cb` 先判网络再判断句，「播放音乐」走本地曲库 `music_play()`（注释自称"离线可用"）却在门禁处 return；改 `local_only` 跳过门禁。澄清：断网能做的事远多于能触发的事，缺的是入口不是能力；镜像`792babc6`待刷 | 待验 | [详情][p353] |
| 进度 | **11-02 会话快照**：P346→P352 的节点状态、当前固件 `15d60475`（含 P352，待刷）、待验证清单（P351 设备已就绪最该先测）、本轮技术结论（`drop` 非丢帧 / 语音命令被自己那句回话打掉 / 本地命令不本地）、已知问题与工具坑 | 交接 | [快照][snap] |
| P352 | **断网语音静默失败**：ASR/LLM/TTS 全在云端，"本地命令短路"只短路 LLM 不短路 ASR（`voice_local_cmd_parse` 吃的是 ASR 文本），所以断网连"坐下/睡觉"都用不了；且失败只发 `pet_idle`，零提示。另 `dm_net_wifi_connected()` 只判 wlan0 有无 IPv4，连 AP 无上行时拦截失效→白录一遍。加 `voice_evt kind=error`，系统级 timer 弹宠物气泡「网络连不上…」/「没听清…」；镜像`15d60475`待刷 | 待验 | [详情][p352] |
| P351 | **睡觉 hold 被语音播报打断**（P349 不充分）：`pet_core_command(2)` 设好 `hold=2` 后 0.42s，TTS 起播的 `PET_EVT_VOICE_PHASE(3)` 经 `pet_request_action()` 的过渡分支把 `sleep_enter` 换成 `wake`，`animation_done` 再把 `g_state` 写回 IDLE，而 hold 只在 `state==SLEEP` 分支里被认 → 彻底失效。四处修：`pet_next_state` 开头 hold 自愈、VOICE_PHASE 睡觉期间早退、`nap-done` 加 hold 检查、摸它时清 hold；镜像`91d1db40`待刷 | 待验 | [详情][p351] |
| P350 | **本地命令词表优先级**（巡检）：`match_comfort` 排在 `match_volume` 后面，被 `dev_adjust_parse` 抢走——"声音开到最大"只 +5（期望 100）、"声音小"只 -5（期望 -10）；`match_pet` 的"分钟→秒"解析因词表匹配不到"走五分钟"而成死代码，补"走…分钟"前置分支；另核素材 82/82、CRC 表 82/82 三方一致，11 个状态全可达；镜像`91d1db40`待刷 | 待验 | [详情][p350] |
| P349 | **叫它睡觉它不睡**：`pet_core_command(2)` 只设了 `g_hold_mode`，而 `pet_next_state()` 根本不看它——白天+精力够就把 SLEEP 判回 IDLE，`sleep_enter` 播完下一个 time-tick 就爬起来。加 `hold_mode_now()` + SLEEP 分支早退；hold 补时长（坐10min/睡30min）；顺带把重复失败素材的 4 次重试降到 1 次（每次取帧 50ms→10ms）。镜像`f9c9c6d8`待刷 | 待验 | [详情][p349] |
| P348 | **1.3s 级卡顿真凶＝壁纸 PNG 每帧重解码**：`blue-hour.png` 1920×1200 解出 8.8MB > `LV_CACHE_DEF_SIZE` 5MB，装不进缓存 → 每帧 open/read/inflate 约 90ms×15 次/轮；关掉 `DM_GLASS_USE_WALLPAPER` 回落到渐变；**板测：`decode_png` 650/分→0，`round p50` 1361ms→17~20ms**；宠物侧同时确认已好转（mismatch 1881→50~68/分）；另更正 `drop` 是采样溢出计数、非丢帧；镜像`b39ca99f`已刷 | 板测 | [详情][p348] |
| P346 | **返回键偶发卡顿**：宠物坏帧在 33ms 定时器里每拍重试（30Hz×4×78KB NAND＋逐位 CRC32），坏素材活跃期间 UI 线程被占满；改坏帧退避（30s→120s）＋CRC32 表驱动（结果逐位一致）＋日志限流，`close_subpage` 加分段埋点；镜像`2768678e`待刷 | 待验 | [详情][p346] |
| P345 | **快问按钮三修**：超时文案被写进 LLM 缓存、命中后还会再 put 刷新 TTL →「讲个笑话」永久秒回超时；时间 fast-path 改本地格式化（原样念 `UNIX epoch`）；chips 请求带 `speak:1` 补 TTS，点按钮既上屏也出声；镜像`6cc49ee0`待刷 | 待验 | [详情][p345] |
| P344 | **人设统一"银月"**：提示词/SOUL/宠物默认名/聊天气泡全改银月（原先 AI Agent、旺财、第三人称"猫"互相打架）；`pet:*` 先应"好的"再动，新增舔爪；镜像`9c657efd`待刷 | 待验 | [详情][p344] |
| P343 | **猫"说英语"**：回复被模型包成 `<tool_call>…<parameter=text>`，TTS 念标签成英文并触发 HTTP 400；改为取正文再播、UI/历史同源清洗；usrdata 的 config.json 同时预置 Key 与 WiFi；镜像`702dafa4`待刷 | 待验 | [详情][p343] |
| P342 | **首音前移**：LLM 走 SSE、只早切第一句（每多切一句多付约0.9s TTS首包）；`app:*` 本地命令补"好的"；Key 改预置在 usrdata 的 config.json（不进固件），镜像`0ec3a7cf`待刷 | 待验 | [详情][p342] |
| P341 | **计时打准**：旧 `LLM latency` 因 `s_asr_done_ts` 只在流式ASR赋值而失真；加 TURN 打点+xrun/短写计数；板测首音4.3s、播放侧零欠载 | 板测 | [详情][p341] |
| P340 | **语音方向评估**：建议豆包ASR/TTS与LLM独立选择；已有双后端，实时对话需新适配；未迁移 | 评估 | [详情][p340] |
| P339 | **语音未闭环**：实机欠载、旧Ark 400；计时失真；拟评估实时语音，明日续接 | 交接 | [详情][p339] |
| P338 | **语音延迟**：MiMo已回复但首段TTS约10s，系XML工具轮次；禁tools版待刷 | 待验 | [详情][p338] |
| P337 | **扫码Key到不了LLM**：`agent_loop` 每轮 `llm_router_apply(0)` 覆盖回构建期 Ark 默认模型 → 400 ClosedEndpoint；pair_server 现同步写 router slot 0，400 日志加 host/model；镜像`cf017943`待刷 | 待验 | [详情][p337] |
| P336 | **ASR 请求体截断**：`prefix_len` 把格式串里的 `%s` 两字节也算进去，body 停在 `base64,` → MiMo 400 Invalid JSON；改 `strlen(prefix)-2`，板端 `ASR: 今天多少` 通过 | 板测 | [详情][p336] |
| P335 | **串口交接**：实际921600恢复1.5M后乱码消失；新日志无明确资源错误，用户反馈res异常待冷启动核验 | 交接 | [详情][p335] |
| P334 | **收尾修复**：语音62例、养成计时5项、宠物9组通过；V1包`25ac5db6`待刷，SIM编译受阻 | 待验 | [详情][p334] |
| P333 | **语音动作三组**：闹钟增删查(`alarm:del/delall/off/list/snooze`)、场景(`scene:sleep/out/home` 串音乐+亮度+猫)、健康打卡(`health:water/stand/snooze`)；查询类按用户意见砍掉；镜像`86ab3d03`待刷 | 待验 | [详情][p333] |
| P332 | **语音开页面 + 口语化控制**：`app:*` 开 11 个页面/回桌面；`match_comfort` 认"太吵/刺眼/听不清"等自然说法（±10）；音乐增别名并修"停下"被音乐抢走的冲突；镜像`14f0d8ce`待刷 | 待验 | [详情][p332] |
| P331 | **语音控宠物**：`pet:walk[:秒]/sit/sleep/eat/stop` 本地短路（不过 LLM）；散步满屏 2D 走+坐 4~8s，到点复用既有回位分支；镜像`ef9a4788`待刷 | 待验 | [详情][p331] |
| P330 | **V1 语音整体切小米**：MiMo ASR/TTS 后端 + 设置页「AI 语音」扫码配 Key（局域网配网页 `:9000`，模型 ID 也可配），LLM+ASR+TTS 一个 Key 打通；镜像`31ca542a`待刷，真实调用待验 | 待验 | [详情][p330] |
| P329 | **V1 MiMo 准备**：预设改 V2.6 Flash，私密 Key 入口已接；板包`282c7507`待刷，真实调用待验 | 待验 | [详情][p329] |
| P328 | **V1 天气慢**：修正耗时计数与结果检查延迟；板包`cc3da52c`待刷，服务待选 | 待验 | [详情][p328] |
| P327 | **开机提速（保守档）**：新脚本烧录后自动启动联网成功，脚本31.6→14.3s、NSH +48.3→27.9s；冷启动待复测 | 板测中 | [详情][p327] |
| P326 | **开机提速交接**：旧包显示循环约17.5s、脚本结束48.3s；Wi-Fi/LED/引导读量待核，未改代码 | 交接 | [详情][p326] |
| P325 | **启动日志修复**：NSH64字节截断WiFi长注释，碎片被当命令；已缩行并出包`12d02c0a`，待刷 | 待验 | [详情][p325] |
| P324 | **蓝牙暂缓**：H5仍超时；UART1还给PD21/PD22调试口，停BT初始化/服务；新包`f255e96d`待刷 | 待验 | [详情][p324] |
| P323 | **逗猫自然退出**：移除结束按钮，松手3秒退出；宿主5组与V1整包通过，待刷 | 待验 | [详情](project_docs/devlogs/10-31-p323-pet-play-idle-exit.md) |
| P322 | **冷启动挂载通过**：实取`/resource` romfs、82张宠物PNG；约105秒无PNG校验错，长期稳定性待验 | 板测 | [详情][p319] |
| P319～321 | **宠物手感三改**：胶囊464×119→267×86；单击只开关菜单、摸猫归长按；闲置30~60分打盹8~13分；搬动后5分自己走回；`39d463f6`待刷 | 待验 | [详情][p319] |
| P318 | **宠物轻量胶囊交互**：干掉笨重弹窗，改猫头顶悬浮胶囊[喂鱼/逗猫]，防抖8→20px，镜像`404dc275`待刷 | 待验 | [详情][p318] |
| P317 | **宠物止血落地**：槽位5→25常驻常用帧切断高频NAND读，解码失败复用上一帧，镜像`896d8005`待刷 | 待验 | [详情][p317] |
| P314～316 | **宠物解码**：证读路径间歇返回坏字节；1KB分块更差（否定路径假设），改一次读全+4次校验重试，`a6b0c68f`待刷 | 待验 | [详情][p314] |
| P313 | **板测FAIL**：布局修正生效，5分钟293次PNG失败；串口仍采集，诊断包未实施 | 交接 | [详情][p313] |
| P307 | **宠物验收不通过**：提醒/计时缺陷、无猫叫、成长未闭环；宿主复现 | 审计 | [详情][p307] |
| P312 | **宠物素材解码损坏**：宠物+壁纸+WiFi固件同时读坏；已排除资源/文件名/堆大小等7项；H1=G2D-DMA冲突待验 | 未解决 | [详情][p312] |
| P305 | **宠物互动+板包**：走位/回位/喂食条/逗猫/双击喂食；官方流程重打包`46a6eb57`，资源/入口/启动/整包校验PASS，上板待验 | 待验 | [详情][p305] |
| P304 | **宠物三项返工**：呼吸帧量测达标、浮层钳位、800ms双击喂食；LVGL宿主通过、goldfish编译通过；未运行SIM | 待验 | [详情][p304] |
| P303 | **宠物接入 PET APP**：18动作真帧+逐帧时长进播放器；宿主7组PASS、goldfish 2541/2541过，SIM资源80张哈希一致；板端/视觉待验 | 待验 | [详情][p303] |
| P302 | **宠物制帧**：149活动帧位/79图；新增walk十二帧过渡候选，导出核验过，步态/固件待验 | 待验 | [详情][p302] |
| P301 | **远程日志**：adbd TCP 5555 + /dev/log；板端实测 connect/logcat/shell/dmesg/pull/重连通过，logcat 先回灌历史 | 代码 | [详情][p301] |
| P300 | **V1 蓝牙 E1 + 音频**：UART1 恢复 PG6/7/8/9，放开 BT 初始化与 bluetoothd；Settings 蓝牙页补真实状态/自动发现/可被发现回读；A2DP sink 软件解码出声 + AVRCP 控歌，编译打包通过，板端待验 | 待验 | [详情](project_docs/devlogs/10-29-p300-bt-uart1-pinmux.md) |
| P299 | **V1 按钮阴影**：板测否决（返回键/目录图标丑），两处样式已回滚并出回滚包 | 回滚 | [详情](project_docs/devlogs/10-28-p299-button-shadow.md) |
| P298 | **V1 壁纸**：thin 板测仍艳；再叠 30% 白霜，新板包待验 | 资源 | [详情](project_docs/devlogs/10-28-p298-v1-thin-frost-wallpaper.md) |
| P296 | **V1 官方流程重编**：镜像 `a80605fe`，板端开机进桌面 | 代码 | [详情](project_docs/devlogs/10-28-p296-v1-official-build.md) |
| P294～P295 | **V1 LVGL 刷新**：同值跳过 setter；B0 定 fbdev+DIRECT 与埋点位置，B1 埋点进 SIM 与板包；运行与板端数据待测 | 代码 | [刷新][p294] [量测][p295] |
| P291 | **V1 玻璃材质与壁纸**：蓝青大圆弧渐变已进板包和 SIM 资源；板端观感待验 | 代码 | [详情][p291] |
| P290 | **V1 接入 goldfish**：SIM 分支+资源根+1920×1200；截图/点击进设置；板端已打包未上板 | 代码 | [详情][p290] |
| P289 | **转 V1 主线**：V2 降副产品冻结（显示可用/触摸断触）；V1 UI 架构改造启动，范围待定 | 决策 | [详情][p289] |
| P288 | **TIANMA 1080x2160**：hspw 3→4 修 dsi_hsa；底稿=tianma6in.dtsi；20FPS 仍闪；E1 改 BURST 包 `2be4c9af` 待刷 | 实验 | — |
| P287 | **V2 收尾**：触摸停止投入（R5 PB5 上拉致花屏已回退）；V1 独立出包过全项校验；两线一键入口修通 | 决策 | [详情][p287] |
| P286-aux | **复核+参考板**：STOP 错误被吞、恢复分类失真；INT 仅 10k 上拉 1.8V 无转换；RK3588S 400k 无转换 | 只读 | [复核][p286review] [对照][p286ref] |
| P286 | **停止**：R4s 未刷；R5（PB5 上拉 3.3 V）花屏已回退；触摸停在可点按/拖动断触 | 停止 | [详情][p286] [流程][p286-fix] |
| P285 | **根因未定**：E2同症降低init表优先级；LVGL/G2D与输出链路均待分层。E3包已核验，待TCON色条/纯色板测；宿主无串口 | 实验 | [详情][p285] |
| P281～P284 | **V2 显示实验**：竖屏驱动已移除；burst 暗影、31 条 Himax init，未上板复测 | 实验 | — |
| P275～P280 | **720×1440 驱动移植**：屏已点亮，颜色偏蓝/偏暗/残影未解（按驱动移植未完成）；细节 MD 已清理。设置内联详情同期 | 未固化 | [设置][p275] [盘查][p275-inv] |
| P274 | **UART 重排 + VNC 恢复**：左工具栏/右日志 1440×720；goldfish 像素核验；R528 已打包；观感待验 | 代码 | [详情][p274] |
| P273 | **HOME UART 磁贴不跳转**：`launch_app_after_fade` 缺 "uart_dbg" 分支，补分支后 goldfish 可进子页 | 代码 | [详情][p273] |
| P270～P272 | **V2 UI 整改**：锁屏布局、材质和样式审计；goldfish 验，板端待验 | 代码 | [锁屏][p270] [材质][p271] [审计][p272] |
| P269～P244 | 已归档（早期 V2 节点） | — | [归档索引](project_docs/archive/v2/devlog-index.md) |
[p334]: project_docs/devlogs/11-01-p334-voice-guard-pet-time.md
[p338]: project_docs/devlogs/11-01-p338-voice-latency-status.md
[p335]: project_docs/devlogs/11-01-p335-serial-handoff.md
[p336]: project_docs/devlogs/11-01-p336-asr-body-truncation.md
[p337]: project_docs/devlogs/11-01-p337-llm-router-not-synced.md
[p319]: project_docs/devlogs/10-31-p319-pet-tap-nap-freepos.md
[p324]: project_docs/devlogs/10-31-p324-uart1-restore.md
[p325]: project_docs/devlogs/10-31-p325-boot-log-fixes.md
[p326]: project_docs/devlogs/10-31-p326-boot-speed-handoff.md
[p327]: project_docs/devlogs/10-31-p327-wifi-boot-trim.md
[p328]: project_docs/devlogs/10-31-p328-weather-latency.md
[p329]: project_docs/devlogs/11-01-p329-mimo-flash-prep.md
[p330]: project_docs/devlogs/11-01-p330-xiaomi-voice-settings.md
[p331]: project_docs/devlogs/11-01-p331-pet-voice-commands.md
[p332]: project_docs/devlogs/11-01-p332-voice-open-pages.md
[p333]: project_docs/devlogs/11-01-p333-alarm-scene-health.md
[p318]: project_docs/devlogs/10-31-p318-pet-capsule-interaction.md
[p317]: project_docs/devlogs/10-31-p317-pet-cache-and-fallback.md
[p312]: project_docs/devlogs/10-31-p312-pet-asset-corruption.md
[p314]: project_docs/devlogs/10-31-p314-pet-png-input-snapshot.md
[p313]: project_docs/devlogs/10-31-p313-v1-pet-memory-layout.md
[p301]: project_docs/devlogs/10-29-p301-adb-tcp-log.md
[p302]: project_docs/devlogs/10-29-p302-v1-pet-redesign.md
[p303]: project_docs/devlogs/10-30-p303-v1-pet-app-integration.md
[p304]: project_docs/devlogs/10-30-p304-v1-pet-rework.md
[p286]: project_docs/devlogs/10-25-p286-touch-driver-port.md
[p291]: project_docs/devlogs/10-27-p291-v1-glass-material.md
[p295]: project_docs/devlogs/10-28-p295-v1-lvgl-perf-entry.md
[p294]: project_docs/devlogs/10-28-p294-v1-lvgl-refresh.md
[p286review]: project_docs/devlogs/10-27-p286-touch-review.md
[p286-fix]: project_docs/specs/misc/hx83102-touch-fix-process.md
[p290]: project_docs/devlogs/10-27-p290-v1-sim-hookup.md
[p289]: project_docs/devlogs/10-27-p289-v1-mainline-ui-rework.md
[p287]: project_docs/devlogs/10-27-p287-v2-wrapup.md
[p286ref]: project_docs/devlogs/10-26-p286-reference-boards.md
[p285]: project_docs/devlogs/10-22-p285-display-host-audit.md
[p274]: project_docs/devlogs/10-18-uartdbg-relayout-verify.md
[p273]: project_docs/devlogs/10-17-uart-nav-fix.md
[p272]: project_docs/devlogs/10-17-ui-style-audit.md
[p271]: project_docs/devlogs/10-17-glass-material-and-wash.md
[p270]: project_docs/devlogs/10-17-standby-landscape.md
[p275-inv]: project_docs/devlogs/10-19-p275-source-inventory.md
[p275]: project_docs/devlogs/10-18-subapp-design-language-handoff.md
[p305]: project_docs/devlogs/10-30-p305-v1-pet-interaction.md
[p307]: project_docs/devlogs/10-31-p307-v1-pet-acceptance-audit.md

[p339]: project_docs/devlogs/11-01-p339-mimo-dialog-boundary.md
[p340]: project_docs/devlogs/11-02-p340-voice-provider-assessment.md
[p341]: project_docs/devlogs/11-02-p341-voice-turn-timing.md
[p342]: project_docs/devlogs/11-02-p342-voice-first-sentence-streaming.md
[p343]: project_docs/devlogs/11-02-p343-voice-toolcall-leak.md
[p344]: project_docs/devlogs/11-02-p344-persona-unify-pet-ack.md
[p345]: project_docs/devlogs/11-02-p345-quick-ask-buttons.md
[p346]: project_docs/devlogs/11-02-p346-pet-bad-frame-backoff.md
[p348]: project_docs/devlogs/11-02-p348-wallpaper-per-frame-decode.md
[p349]: project_docs/devlogs/11-02-p349-pet-sleep-hold.md
[p350]: project_docs/devlogs/11-02-p350-voice-matcher-priority.md
[p351]: project_docs/devlogs/11-02-p351-pet-sleep-stomped-by-voice.md
[p352]: project_docs/devlogs/11-02-p352-offline-voice-silent-fail.md
[p353]: project_docs/devlogs/11-02-p353-offline-local-gate.md
[handoff]: project_docs/devlogs/11-02-handoff-for-optimization.md
[snap]: project_docs/devlogs/11-02-progress-snapshot.md

[p354]: project_docs/devlogs/11-02-p354-local-api-offline.md

[p355]: project_docs/devlogs/11-02-p355-voice-pet-latency-audit.md

[p356]: project_docs/devlogs/11-02-p356-romfs-read-ab.md
[p358]: project_docs/devlogs/11-02-p358-ai-page-buttons-joke.md
[p359]: project_docs/devlogs/11-02-p359-wallpaper-resident-chime.md
[p360]: project_docs/devlogs/11-02-p360-weather-icons.md
[p361]: project_docs/devlogs/11-02-p361-city-beijing.md
[p362]: project_docs/devlogs/11-02-p362-voice-gap.md
[p363]: project_docs/devlogs/11-02-p363-pet-wake-on-speak.md
[p364]: project_docs/devlogs/11-02-p364-freeze.md
[p365]: project_docs/devlogs/11-02-p365-session-backfill-codebuddy.md
[p367]: project_docs/devlogs/11-02-p367-v1-session-curation.md
[p368]: project_docs/devlogs/11-02-p368-public-session-review.md
[p369]: project_docs/devlogs/11-02-p369-public-readme-refresh.md
[p370]: project_docs/devlogs/11-02-p370-dedicated-repo-handoff.md

[p357]: project_docs/devlogs/11-02-p357-romfs-display-aborted.md
