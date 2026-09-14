# DevLog 总览 — R528 (openvela) 项目节点索引

> 工程：openvela (R528) — nsh 配置｜工作目录：`/data/dm`
> 旧驱动备份：`/data/vela-backup/{screen-driver,touch-driver}`
> **本文件是目录索引：每行 = 一节点（P/主题/固化tag/子文档链接）。排障详情唯一源在子 devlog，禁止在本文件找排障过程。**
> **AI 交接：先读 `AGENTS.md`（环境/焦点/纪律）→ 本文件（索引）→ 按需点链接读 `project_docs/devlogs/<M-D>partNdevlog.md`。**
> 旧节点（8-17 及以前）已归档至 `project_docs/archive/devlog_archive_p73_p100.md` + `devlog_archive_p1_p8.md`；P114~P122续（8-23/26/27）归档至 `devlog_archive_p114_p122.md`，按详情列回溯子文档。

## 当前节点：P217（见下方节点表首行)

## 节点表

> 详情列均相对 `project_docs/devlogs/`（如 `9-4part1` = `9-4part1devlog.md`）。✅=已闭环 ⏳=待上板/续 ⚠️=挂起

| P | 主题（根因/结论加粗） | 固化 | 详情 |
|----|----------------------|------|------|
| 9-11 P217 | ✅ **交作业收尾+发布V1.0.0（YNM-3000）**：日志合规重提(212会话)/运行时Skill×2/报告按模板重写/版本型号/去假天气/README重写；编译打包自证+归档 | ok-20260911-21 | **9-11part2** |
| 9-11 P216 | ✅ **崩溃挂死6项**：music UAF补空/MONO改#if/wifi built守卫/ai EOF+127帧/uart poll验revents/alarm按行解析 | ok-20260911-20 | **9-11part1** |
| 9-11 P215 | ⏳ **公共PR去P140/P141**（尾音真因在App侧）+ 子APP遍历**发现多个高危待修** → 暂缓同步fork | — | **9-11part1** |
| 9-11 P214 | ✅ **提示音尾音根因=tone_play_one漏sw_params**：短WAV欠载重播→补sw_params；音乐/AI无此因，上板已过 | ok-20260911-19 | **9-11part1** |
| 9-11 P213 | ✅ **主机打包防呆**：check_res三段断言+新鲜度；pack末尾钩子自动跑失败即中断 | ok-20260911-18 | **9-11part1** |
| 9-11 P212 | ✅ **911=res分区未完整烧录**：0x27=固件校验fail（旧res会通过→排除旧包/代码回归）；重编整包烧res | ok-20260911-17 | **9-11part1** |
| 9-10 P211 | ✅ **日期时间独立子页**：hero活刷/自动总闸真开关+上次同步/滚轮星期预览+2月联动+2000跨度；待烧ok-44验L清单 | ok-20260910-44 | **9-10part5** |
| 9-10 P210 | ✅ **闹钟返工+日历升级**：snooze音色继承/同分全收/分钟门控/有序插入；周一起始/太阳黄经节气107点全中/纪念日•+长按增删/年快跳；待烧ok-43验K清单 | ok-20260910-43 | **9-10part4** |
| 9-10 P209 | ✅ **WiFi包自证+日历返工**：usrdata含wifi-home实锤/lunch2对版/翻月文字药丸/标题独立大行/星期全称；待烧ok-41确认+WiFi板上定界 | ok-20260910-41 | **9-10part3** |
| 9-10 P208 | ✅ **验证收绿+日历闹钟**：prompt简洁/lvPR对稿/快问本地化/全量rev R2-R6/日历+农历/闹钟+语音定+手配时间；待烧ok-38上板验+脱敏push+视频 | ok-20260910-38 | **9-10part2** |
| 9-10 P207 | ⏳ **审计修复包冻结转验证**：快问/城市/蓝牙WiFi/锁屏/音乐/UART/文件/Games砍/报告去虚/MiMo回退/AIrev/宠物子页/TTS+timezone双实锤；清单verify_checklist；待打勾+脱敏push+视频 | ok-20260910-24 | **9-10part1** |
| 9-9 P206 | ⏳ **交作业专项启动**：SDK清理+deskmate配置(ok-8)+专属仓070映射方案定稿；隐私策略定案(agent_secrets.h.example+.gitignore+基线ok-20260909-10，见checklist)；待Accept→fork→搬文件→README→日志→报告→视频 | — | **9-09part3** |
| 9-9 P205 | ✅ **极简+三开关**：三连点直开喂食；删互动条/健康联动5事件/狗；Settings加显示总开关+饥饿提醒+清档重养；存档v2 | ok-20260909-7 | **9-09part2** |
| 9-9 P204 | ✅ **点按语义**：短按只触摸不弹条；长按抚摸+出菜单（P205菜单删除，长按只抚摸） | ok-20260909-6 | **9-09part2** |
| 9-9 P203 | ✅ **命名卡死根因+喂食简化**：overlay泄漏无关闭路径→确定/取消+guard；名字≤10字；种类/成长/状态行；喂食剩鱼/零食 | ok-20260909-5 | **9-09part2** |
| 9-9 P202 | ✅ **pet三层重构**：门面230行；core引擎+view视图+ops回调；行为配帧窗；load改cJSON；API零改动 | ok-20260909-4 | **9-09part2** |
| 9-9 P201 | ✅ **新图直连+原生12帧**：192旧C→144新C；FRAMES 16→12；清两archive旧成员；呼吸/弹跳降频；固件17.6MB | ok-20260909-3 | **9-09part2** |
| 9-9 P200 | ⚠️ **cat12.png 精灵替换（板上异常待验证，代码侧P201已处理）**：cat12.png(1536×1024) 12×12→128×128 PNG→C数组→192帧(12态×16帧)；dm_pet.h 常量8→16；动画速率BUG修复(g_tick/2→g_tick)；distclean全量重编+打包+固化ok-20260909-2；**板上验证：猫待机变来变去(3形态)+点击大小跳变**——C文件像素比对✅、ar归档无重复✅、旧素材目录已清✅，根因未定（疑dm_pet.c硬编码256假设/cat12.png精灵本身形态不一致） | ⚠️ ok-20260909-2 | **9-09part1** |
| 9-8 P199 | ✅ **用户三问题一次性修复**：①启动慢=u-boot boot_normal 无长度整读 50MB 分区 → env.cfg 加 `2000000` (32MB) 仅读固件区；②猫放大=PET_SIZE 300px(1.17x)未按原 256×256 → 改 DM(107)≈256px native 1:1 显示；③猫走到音乐控件消失=代码层无法复现(z-order 顶层/区域不重叠/帧全非空)，PET_SIZE 缩小缓解视觉误判 | ok-20260908-3 | **9-08part3** |
| 9-8 P198 | ✅ **开机随机卡LOGO根因=NuttX显示链路 DSI gen 写无超时死等**：BOE 面板 init ~30 次走 `de_dsi.c` `dsi_gen_wr()` 的 `while(inst_busy);`（inst_st 偶发不清零→无限 spin=卡 LOGO，重启自愈）；同文件 `dsi_dcs_wr` 有界 50 次/5ms+强清为对照铁证；修=仿其加 100us×50 超时+强清+DISP_PRINT 诊断串（NuttX 下 hal_log/printk 为空宏会吞串，改用 printf）；u-boot `boot_normal` 无长度参数=整读 50MB 分区（固件仅 33.34MB，读 16.7MB 陈旧 NAND 区）为次要隐患，未改 | ok-20260908-2 | **9-08part2** |
| 9-8 P197 | ✅ **精灵网格对齐+SLEEPY 态删除**：旧 cut_sprite 硬编码(110,35)错位一帧混邻帧→重写自动 gutter 网格检测（12行×8列实测 band）；cat2 无 sleepy 素材→删 PET_STATE_SLEEPY（枚举11态=帧表11行）/sleepy 池+8C/帧表行；scale+翻转并为单一 image scale 体系（负 scale_x=翻转，免双系统花屏）；can_walk 闸门冻结 EAT/SLEEP/PLAY/TOUCH/GROOM/CELEBRATE 位移；死代码 pet_pick_new_target 清除；精灵 72% 占画布 | ok-20260908-1 | **9-08part1** |
| 9-8 P196 | ✅ **Cat2.png 精灵全量替换**：cat2.png(1254×1254) 12×8→256×256→C数组→96帧全覆盖；固件35.8MB | ok-20260908-1 | **9-08part1** |
| 9-8 P195 | ✅ **GROOM 状态+12状态动画体系**：新增 PET_STATE_GROOM 枚举+行为池+帧表+Makefile；输出 PET_SPRITE_SPEC.md 图片需求文档 | ok-20260908-1 | 9-08part1 |
| 9-8 P194 | ✅ **Settings 起名崩溃+狗类型切换**：textarea+keyboard 创建在滚动容器上→改全屏 overlay；删狗类型切换行+函数+变量 | ok-20260908-1 | 9-08part1 |
| 9-8 P193 | ✅ **宠物左右走动+互动栏防遮挡**：Y固定X踱步+蹲坐交替；互动栏边缘钳位 | ok-20260908-1 | 9-08part1 |
| 9-8 P192 | ✅ **崩溃修复+固定右下角**：砍自由走动(sqrt/walk帧)避免 DFAR 0xfff2f2ff；清旧.o缓存 | ok-20260908-1 | 9-08part1 |
| 9-8 P191 | ✅ **宠物子系统全面Review+7项Bug**：①4帧/8帧冲突→删除本地typedef ②花屏→呼吸改image_set_scale ③走路缩小→计算缩放 ④g_name_lbl死代码清理 ⑤切类型不刷新 ⑥pet_frames硬编码 ⑦气泡偏移 | ok-20260908-1 | 9-08part1 |
| 9-7 P190 | ✅ **黑猫根因=ar 归档污染**：同名 C 重编追加 `_N.o` 永不替换，walk8+touch_04-07 旧 `_1.o`(占位灰黑) vs 新 `_2.o`(胖橘) 并存，链接器取首定义→固件黑猫；删旧成员重链修复；**重编暴露 dm_pet.h 缺 PET_TYPE_DOG（Settings 早已引用）→补枚举** | ok-20260907-14 | **9-7part2** |
| 9-7 P189 | ✅ **胖橘 96 帧整链**：cat.png(1254) 12×8→256×256×96→300×300 C 数组→代码扩 8 帧（walk 真帧替代宏别名）→bootloader 分区 24→50MB→固件 42.7MB；核验坑：`.c` 文本正则假阳性，须 PIL 编码全量 find | ok-20260907-13 | **9-7part2** |
| 9-7 P188 | ✅ **模拟器移植**：板端UI源码+8个stub层→编译通过→lvglsim启动成功（**LV_MEM_SIZE 128MB 生效**，music_play_btn_create 已有 null 防御） | ok-20260907-1 | **9-7part1** |
| 9-6 P187 | ✅ **PIL像素画生成器**：128×128 chibi风格+表情系统+粒子效果+贝塞尔尾巴+分区扩容(8→12MB)+缩放24→128 | ok-20260906-23 | **9-6part2** |
| 9-6 P186 | ✅ **精灵质量评估**：24×24几何缩放糊成一团→搜索AI生图方案→写44条prompt+转换脚本→最终决策PIL自生成 | — | 9-6part2 |
| 9-6 P185 | ✅ **宠物代码6项修复**：MUSIC_ON→HAPPY+SAD自动回落+EAT/PLAY定时回IDLE+行为池精简+死字段清理+warning | ok-20260906-22 | 9-6part2 |
| 9-6 P184 | ✅ **宠物全链路**：动画帧重做（8状态±8~18px差异）+wander随机走动+弹跳不漂移+喂食弹窗（短按→鱼/肉/菜/零食）+帧率提升3tick | ok-20260906-12 | 9-6part2 |
| 9-6 P183 | ✅ **像素风精灵+缩放**：gen_pet_sprites.py OUT_SIZE=24 + lv_image_set_scale 放大 DM(128)；pet_frame_table 补全11状态+去static+include | ok-20260906-11 | 9-6part2 |
| 9-6 P182 | ✅ **宠物基础修复**：Montserrat→FONT_BODY（口口根因）+sprite真实数据+FLOATING脱flex+food emoji→LV_SYMBOL | ok-20260906-7 | 9-6part2 |
| 9-6 P181 | ✅ **voice_sim 语音编排测试命令**：独立 NSH app 调 voice_director_test_scene（绕 classify_scene 全局状态），覆盖 12 场景×时段×冷却边界；P141 尾音✅（片段音消除），麦克风关闭声=驱动层 codec ADC 关断 POP（不可应用层修） | ok-20260906-1 | — |
| 9-5 P180 | ✅ **全量BUG修复14项**：H1栈溢出(books)/H2 null终止(files)/H3命令注入→files_rmrf/M1 SSID上界/M2 JSON转义/M3 fd TOCTOU/M4 ring并发暂停/BT volatile/L1 strtol溢出/L2 time_t截断/L3 ld2410b_deinit/L4 als_deinit/L5 tone失败补fg_release | ok-20260905-39 | **9-5part5** |
| 9-5 P176~179 | ✅ Settings实项整改：删11假项；存储真实化+夜览手动即时(开压暗/关恢复) | ok-20260905-36 | 9-5part4 |
| 9-5 P175 | ✅ **Voice Director 听感修复**：回来按时段选池+back冷却180s独立+清空票+场景7趋静 | ok-20260905-32 | 9-5part4 |
| 9-5 P174 | ✅ **TTS 尾音修复（dm_sound_destroy 加30ms 静音+drain）**：根因=XPlayer sink 关闭直接 close 无 fade-out（dm_tone_play_one 有但 TTS 路径没有）→ 补30ms 零填充+drain 再 close | ok-20260905-30 | **9-5part3** |
| 9-5 P173 | ✅ **V0.0.7 上板验证**：P144乱码✅ P142开场音效✅ P143去主人✅；P141尾音✅（P174修复后片段音消除，麦克风关闭声=驱动POP）；P133天气注入待ai_agent配置；P135✅（voice_sim命令）；蓝牙H4 110挂起不可用 | ok-20260905-29 | 9-5part3 |
| 9-5 P172 | ✅ **UART 终端贴合输入栏 + 发送追加\r\n**：终端高度减 DM(58) 0间距贴合 input_bar；发送按钮发文本+换行 | ok-20260905-27 | **9-5part2** |
| 9-5 P171 | ✅ **键盘弹出自动上移 textarea 父容器**：focused 保存 orig_y→set_y 到键盘上方，defocused 恢复；dm_kb_state_t 加 ta_parent_orig_y | ok-20260905-25 | 9-5part2 |
| 9-5 P170 | ✅ **dm_kb_* 审查+WiFi 迁移+防悬挂指针**：WiFi 两处裸键盘→dm_kb_create；dm_kb_destroy 加 lv_obj_is_valid 防 overlay 先删后崩 | ok-20260905-24 | 9-5part2 |
| 9-5 P169 | ✅ **共享键盘工具 dm_kb_* 重构**：统一键盘创建/显隐/销毁 API，uart_dbg 删 3 回调改 dm_kb_create 一行；deskmate_ui.c 不能 include 自己的头文件（dm_track 冲突）→ typedef 内部重复 | ok-20260905-23 | **9-5part1** |
| 9-5 P168 | ✅ **键盘进入自动弹出修复**：根因=keyboard 默认可见→加 HIDDEN + textarea FOCUSED/DEFOCUSED 事件自动显隐 + READY/CANCEL 事件 | ok-20260905-22 | 9-5part1 |
| 9-5 P167 | ✅ **键盘字体统一**：WiFi+UART 统一 Montserrat 30px（可用字号 12/14/16/24/30/36/48，30 是 24 上一档） | ok-20260905-19 | 9-5part1 |
| 9-5 P166 | ✅ **UART 工具栏布局优化**：pad_column DM(10→6) / btn pad_hor DM(14→10) / 内部 DM(8→4) / port 短写 UART1 | ok-20260905-18 | 9-5part1 |
| 9-5 P165 | ✅ **UART 功能增强**：暂停按钮+去掉 4×256B 读限制+HEX 地址连续+统计接口 | ok-20260905-17 | 9-5part1 |
| 9-5 P164 | ✅ **UART 键盘弹出修复**：根因=FLOATING 打断 LVGL 键盘显隐→去 FLOATING 改 align BOTTOM_MID | ok-20260905-21 | 9-5part1 |
| 9-04 P163 | ✅ **暗光弹窗尺寸修复**：**根因=lv_obj_set_width 在 flex flow 前调用被覆盖→w=0 h=0→不可见**→改 lv_obj_set_size(DM(500),DM(300)) 在 flex 后；弹窗已能弹出，视觉效果待用户确认 | ok-20260904-41 | **9-4part3** |
| 9-04 P162 | ✅ **暗光弹窗全链路打通**：①P157 确认门(≥200lux)移除 ②LD2410B present()拦截移除 ③触发日志确认 cb=set ④去冷却期→无限弹 ⑤弹窗10s消失 | ok-20260904-39 | 9-4part3 |
| 9-04 P161 | ✅ **暗光参数定稿**：阈值50→5lux / 防抖5s→60s / 冷却10min→0（无限）；弹窗样式对齐健康弹窗（btn_row 容器+居中） | ok-20260904-36 | 9-4part3 |
| 9-04 P160 | ✅ **驱动 lux 校正 + 极暗估算**：①加积分时间×增益校正因子（50ms→×2.0，与 Android 同芯片驱动一致） ②activate() 同步 struct 字段（原 init=100ms vs activate=50ms 不同步） ③ratio≥0.85 极暗环境最小估算（CH0<200→0.1×CH0×correction） ④sninfo 全删防刷屏 + defconfig 关 SENSORS_INFO | ok-20260904-31 | 9-4part3 |
| 9-04 P159 | ✅ **光感显示修复**：①锁屏 `亮度 N` → `光感 N lux`（加单位+改名） ②label 宽度150→200px ③传感器灰尘清除后正常读数（44lux） | ok-20260904-28 | 9-4part3 |
| 9-04 P158 | ⏳ **亮度显示问题交接（下会话 AI 续查）**：症状=锁屏「亮度」恒 0；已确认链路全通（弹窗铁证）、寄存器配置正确、有效位=bit7 已修、配置非根因；**唯一未决=CH0 为何读出 0**；下会话第一步=上板 ok-20260904-27 抓 `grep LTR553` 看 sninfo(CH0/CH1) 三分支定案 | ok-20260904-27 | **9-4part2** |
## 里程碑（仅版本/大闭环，一行一条 + 链接）

- 🎉 **文件复制进度条 + 音乐联动 + 状态栏三态 WiFi（9-04）**：P145 复制/粘贴/进度条；P146 曲库解耦（dm_now_path）+ 试听即停；P148 时间/日期入状态栏（hero 时钟移除）；P149 WiFi 图标从"永不显示"到三态（SSID 判定根治）；**⏳ P150 时间/日期左上角定位待续（flex 覆盖 align，clk_cont 填满+贴顶左方案待上板）** → 9-4part1
- 🎉 **UART1 回环自证全通 + 无传感器静默（8-27 part2）**：内部回环 LOOP OK（控制器+驱动全通）+ 波特率链路调查（代码正确）+ 无传感器不播欢迎语/不计时；**🔴 1500000 档 FAIL 遗留待专挖** → 8-27part2
- 🎉 **UART 调试工具 + UART0/TF 卡冲突真相（8-27）**：Settings 串口 log 子 APP + 子页不锁屏；**P118 误判修正——gemini-s1 的 PF2/PF4 是 TF 卡（SDC0），全链路禁用 UART0、换口 UART1（PD21/22 mux4）** + uartdbg_test 自测命令；UART1 硬件待上板 → 8-27part1
- 🎉 **V0.0.5（2026-08-23）**：UI 全量汉化收尾（蓝牙/灯光/触摸窗口英文清零）+ Settings 关于区精简（4 项）+ 音频仲裁层（停音乐→释放→播→恢复，根治 EBUSY）+ 死机修复 + XPlayer 生命周期审计；镜像 `/data/vela/releases/V0.0.5/`（md5 6cca3c03 = ok-20260823-15，三仓 v0.0.5 tag）→ 8-23part3/4
- 🎉 **本地提示音全链路（8-16）**：PC 端 tts_proto 合成 12 个 wav（火山 2.0 音色）进 res 分区，板端 dm_tone_play 直读播放替换云端 TTS；顺带 AI 强制中文/llm_router 兜底/TTS 播报链路修复（EBUSY+截断+声卡占用）/锁屏反色/HOME 双小圈 → 8-16part1
- 🎉 **V0.0.3（2026-08-15）**：AI 语音对话闭环上板（双向流式 TTS + ASR batch：Recognized 你好 → LLM → TTS → 喇叭播报）；镜像 `/data/vela/releases/V0.0.3/`（md5 91770356 = ok-20260815-10）→ 8-15part1
- 🎉 **V0.0.2（2026-08-10）**：状态栏恢复 + WiFi/蓝牙 iPad 子页重设计；镜像 md5 a035fed0 = ok-20260810-13 → 8-10
- 🎉 **UI 架构重构 Phase 1（8-9）**：deskmate_ui.c 5876→2146 行，7 页拆 `ui/` → 8-9
- 🎉 **Beta V0.0.1（8-8）**：镜像 md5 fd057da1；流程变化=版本号由用户决定 → 8-8part3
- 🎉 **切歌/连播无声闭环（8-8）**：**RDEN OFF 清零写反 + RAMP FSM 复位**（OFF 清 RDEN + W1C 中断 + RAMP_SRST bit24）→ 8-8part1
- 🎉 **音乐后台化（8-8）**：系统级常驻 + HOME/standby 控件 + 音量死机修复 → 8-8part3
- **Git 版本管理体系（8-5 P7）**：`/data/vela/git_snapshot.sh` 三仓一键固化/恢复；编译打包成功必须自动固化 → 8-5

---
*DevLog by AtomCode (deepseek-v4-flash)*
