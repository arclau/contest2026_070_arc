# DevLog 2026-09-04（part1）— 文件复制进度条（P145）+ 音乐联动（P146）+ 状态栏室内标注（P147）+ WiFi 图标三态（P148~P150）

> 承接 9-1part1（P141~P144 语音/锁屏）。本段覆盖 ok-20260904-1~15，含**文件浏览器复制/粘贴
> 交互补齐、音乐曲目联动解耦、锁屏/home 布局多轮调整、WiFi 状态栏图标从"永不显示"到三态**。

## 一、P145 文件浏览器复制/粘贴 + 后台进度条（固化 ok-20260904-1）

### 需求（用户）
文件复制粘贴逻辑原本就有，补交互缺口：大文件复制不卡 UI。

### 实现
- 空白长按弹粘贴功能栏：`files_blank_long_cb`；重名弹窗自动改名：`files_paste_check_conflicts` + `files_unique_dst`
- **后台线程复制进度条**：`files_copy_worker`（32KB 缓冲逐块拷贝、递归统计总字节）+ lv_arc 环形进度 + 轮询收尾 + 防重入
- MP3 点播改走新接口 `music_play_path(path)`（见 §二），不再污染曲库
- 手动 code review 修 4 处隐患（见 §六 review 记录）

### 产物与固化
编译 ✅ / 打包 ✅（重跑 pack 根因：out/.../image/nsh.fex 是旧副本，pack 取数源 board/configs/nsh.fex 已更新）/ check_res ✅ / 三产物一致（vela.bin md5==nsh.fex）→ **ok-20260904-1**（vendor 仓，apps/nuttx 无改动）

## 二、P146 音乐曲目联动解耦（固化 ok-20260904-2~4）

### 需求（用户）
①AURORA 误显示（播曲库歌却显示外来文件名）；②外来文件试听播完即停；③播放键行为。

### 根因与修复
1. **AURORA 误显示**（ok-20260904-2）：`dm_now`（当前播放）与 `dm_tracks`（曲库列表）耦合——文件浏览器点播污染了曲库 → 解耦：新增 `dm_now_path/dm_now_title` 作为标题唯一源；文件浏览器点播走 `music_play_path()` 新接口，不写 `dm_tracks`；state 持久化改存路径（兼容旧数字索引格式）
2. **外来文件播完即停**（ok-20260904-3）：XPlayer 播完 → `g_xp_state=3` → 500ms `music_timer_cb` → `music_audio_poll` COMPLETE 分支（deskmate_ui.c:2384）→ 在曲库则 `music_album_next(true)`，**外来文件则 `music_pause()`**；试听结束复位 `music_inited=false` + 恢复 `music_track_id` 曲库记忆位；`music_play_path` 不写 state
3. **播放键行为**（ok-20260904-4）：ui_music.c `music_play_pause_cb`：`music_playing`→pause；`music_inited`→resume；否则→`music_play(music_track_id)`——**试听播完必须清零 `music_inited`**，否则会续播外来文件；state 不覆盖 + album_next 基准修复

### 关键常量
`DM_MAX_TRACKS` 64 / `DM_MUSIC_DIR` /sdcard/music / `DM_MUSIC_DIR_NORFLASH` /data/music / `DM_MUSIC_STATE_FILE` /data/dm_music.state

## 三、P147 状态栏「室内」+ 天气卡「室外」（固化 ok-20260904-5）

- 需求（用户）：区分传感器与天气 API 数据源
- 实现：状态栏温湿度（shtc3 室内传感器，`luncher_dm.c update_sensor_cb`，subject 存 10 倍值，一个 `sbuf` 同步 status/subpage/standby 三处）加「室内」前缀；天气卡（open-meteo，`ui_home.c weather_update_cb`）加「室外」前缀
- 优于「实时」前缀：实时无法区分室内外

## 四、P148 布局调整（固化 ok-20260904-6，16 处改动）

- 需求（用户）：时间/日期入左上角状态栏，移除中间大时钟
- 实现：`create_status_bar_ex` 左侧两行（时间 FONT_TITLE 48px + 周几日期）；home/锁屏/子页共用；hero 时钟点击对时功能迁移到状态栏时钟容器（P41 不丢）；删除 `clock_big_label`/`standby_clock_label`/`standby_date_label`，新增 `subpage_date_lbl`/`standby_bar_date_lbl`；home/锁屏控件上移、dock 不动
- 编译踩坑：music state 函数引用的 `dm_tracks/dm_now` 定义需前移到宏之后；`standby_bar_date_lbl` 缺 extern；`clock_click_cb` 被误删（均已修复）

## 五、P149 WiFi 状态栏图标三态（固化 ok-20260904-7~10，跨多轮）

### 需求（用户）
①右上角 WiFi 图标**从未出现过**；②未连接/扫描失败也要有图标（灰或错误态）。

### 排查链（为什么"搞了很久"）
1. 图标链路本身完整：`create_status_bar_ex` 建图标 → 每秒 `dm_net_status_refresh` → `dm_status_icon_refresh`
2. **根因 1（ok-20260904-7）**：状态源 `dm_net_wifi_is_up()` → `wifi_is_up()` 读驱动内部关联标志，开机 `start_wifi.sh` 直连驱动路径下**恒 0**（P66 同款坑，P46/P47 教训）→ 图标永远 HIDDEN。改用 `dm_net_wifi_connected()`（wlan0 有 IPv4，P66 上板验证）——与 AI 语音按钮联网判定同源
3. **三态化（ok-20260904-9）**：`dm_status_icon_refresh(lbl, st)`：st=0 隐藏 / st=1 灰色 / st=2 蓝色——未连接（含扫描中/失败）灰色常驻，消除"图标凭空消失"
4. **根因 2（ok-20260904-12）**：未连接误判蓝色——日志 wlan0 残留 IP 10.*.*.* 但 `wapi_get_essid failed`/ssid=NULL → IP 判定误报已连接 → 改用**与 WiFi 子页「已连接 %s / 未连接网络」文本同源的 SSID 判定**（`dm_net_wifi_cur_ssid` = wifi_get_setting）
5. **符号字体验证（ok-20260904-10）**：fa-solid-900.ttf/fa-brands-400.ttf 源目录存在、strings 确认在 res.fex 内（曾误判"漏打包"，是解析脚本 bug）；check_res.sh 增加两字体字节校验（P92 纪律加固）；驱动路径 `CONFIG_WIFI_FW_PATH=/resource/etc/wifi/FW_NIC_ACUT.bin` 与 romfs 路径一致

### 上板结论（用户反馈）
整包烧录后 **WIFI OK**；`download_firmware FAIL status=0x27` = res 分区未烧全老毛病（AGENTS.md 坑#3），须整包烧录 + `ls /resource/etc/wifi/` 验证

## 六、P150 锁屏/home 布局 + 状态栏细节多轮调整（固化 ok-20260904-9~15）

| 轮次 | 反馈 | 修复 |
|------|------|------|
| ok-20260904-9 | 锁屏健康卡浮空/双圈死顶状态栏；home 卡片太顶留白失衡；日期字太小（FONT_CAPTION 14px 看不清） | sp_top 保底 DM(24)（双圈不顶状态栏）、sp_bot DM32→12（健康卡贴底）、home sp_top DM12→56、日期 FONT_CAPTION→FONT_LABEL |
| ok-20260904-10 | 健康卡仍留空 | **删除 sp_bot**（pad_row 只作用于子元素间，删后健康卡=最后子元素、底部零空隙）|
| ok-20260904-11 | 内容组贴健康卡（sp_top grow1:sp_mid grow3 失衡） | grow 改 3:2——内容组居中偏上，两端 ≥20px 呼吸间距 |
| ok-20260904-14 | WiFi 图标贴电池太密集；时间/日期"顶部左中" | 图标组 pad_column 16→DM(12)；bar pad EDGE_PAD(96)→DM(12)（96 是卡片级边距，把时钟推到左中）|
| ok-20260904-15 | 时间/日期仍"顶部左中" | **LVGL flex 布局覆盖 lv_obj_set_align**（lv_flex.c place_content：content 尺寸对象对齐都被挤压为 START）→ LV_ALIGN_TOP_LEFT 无效；改 clk_cont 高度=TOP_BAR_H 填满 bar（垂直不再居中）+ 内部 column 贴顶贴左 |
| ok-20260904-17 | 时间/日期"顶部左中"根治（P150 闭环） | **真根因链：flex-grow(1) 把 clk_cont 沿主轴(横向)强制拉伸到「左缘~屏幕中点」（place_content 对 grow 子项 area_set_main_size 硬设宽度）+ column 交叉轴 CENTER 把两行横向居中于该大容器**；上轮"贴顶贴左"注释与代码不符（交叉轴仍 CENTER，漏改）→ 交叉轴 CENTER 改 START 贴容器左缘，一次修改覆盖 Home/锁屏/子页三处调用点 |
| ok-20260904-18 | **P151 状态栏时间/日期视觉调整**（用户：时间往右挪一点/日期组合居中/日期加深跟时间一样/时间还能调大吗） | ①时间 48→**56px 新增专用档 FONT_CLOCK**（独立档位，不动全局 title 的 26 处引用；283PPI≈5.0mm 达正文可读标准）+ translate_x 右移 DM(8)≈19px；②日期 FONT_LABEL(30)→FONT_ICON(36) + COL_SEC→COL_TEXT 与时间同深；③**两行水平中心对齐**：`lv_obj_update_layout` 实测两行宽后 translate 补偿 (tw-dw)/2（日期比时间宽 ≈190 vs 150px）|
| ok-20260904-19 | **P152 日期上移贴住时钟**（用户：日期能往上贴住上面的时钟吗，调用 deskmate-ui skill） | **视觉间距根因=label 行高（freetype ≈1.4×字号）内嵌上下空白**，flex gap=0 下日期仍与时间 glyph 有间距 → update_layout 实测 date顶-time底，`translate_y = -vgap` 反向抵消，日期 glyph 顶部贴住时间 glyph 底部（两行合成一个时钟块）；skill 模拟器工作流与 AGENTS.md 坑#1 冲突，仍走板端直改 |
| ok-20260904-22 | **P153 锁屏亮度显示 + 暗光开灯提醒**（用户：亮度传感器数值放人体右侧 + 光暗弹窗提醒开灯） | **数据链路：LTR553 驱动已发布 uORB sensor_light（现采未订阅，CONFIG_SENSORS_LTR553=y）→ 新增 dm_als.c/h**：orb_subscribe + LVGL 1s 轮询 + 暗光状态机（<50lux 持续 5s 防抖触发，10min 冷却）；锁屏健康卡人体右侧新增「亮度 N」（FONT_BODY/COL_SEC，文本去重防乱码 P144 同款）；**暗光弹窗独立于 health_popup**（不掺和排队）：有人才弹（LD2410B 无人不打扰）+「知道了」按钮 + 5s 自动消失；编译踩坑：sensor_light 结构在 `<sensor/light.h>` + `<uORB/uORB.h>`（非 nuttx/uorb.h） |

### ✅ P150 闭环（下会话首项已处理）
**状态栏时间/日期左上角定位已修复**（ok-20260904-17）：根因是 flex-grow 拉伸 clk_cont 宽度后交叉轴 CENTER 的横向居中，改 START 后时间/日期贴容器左缘（x=DM12）。**待上板确认视觉效果**；若仍有偏移，下一步可弃 flex-grow 改绝对坐标（lv_obj_set_pos）或左右定宽。

### ⏳ P151/P152（上板确认项）
**状态栏时间/日期视觉调整已固化**（ok-20260904-18/19）：时间 56px 新档+右移 DM(8)、日期 36px 加深 COL_TEXT、两行水平中心对齐（update_layout 实测宽度 + translate 补偿）、日期 translate_y 上移贴住时钟（两行合成一个时钟块）。**待上板确认**；字号仍嫌大/小可调档位（48/56/64），右移量改 DM(8)，贴紧度过大可在 -vgap 基础上加回 DM(2)~DM(4) 呼吸间距。

### ⏳ P153（上板验证项）
**锁屏亮度显示 + 暗光开灯提醒已固化**（ok-20260904-22）：亮度数值（dm_als_get_lux）显示在锁屏健康卡人体右侧；暗光弹窗阈值/防抖/冷却均可在 dm_als.h 宏调整（DM_ALS_DARK_LUX=50 / DEBOUNCE 5000 / COOLDOWN 600000）。**待上板验证**：锁屏是否显示「亮度 N」、遮挡光感时是否 5s 后弹窗、按「知道了」后 10min 内不重复。

### ✅ P153b 全链路审查修复（ok-20260904-23，用户要求提前处理 BUG）
审查发现并修复 4 处：①**init_sensors 顺序缺陷**——温湿度订阅失败提前 return 会连带跳过 dm_als_init（shtc3 挂时光感/暗光提醒整体失效）→ dm_als_init 独立于温湿度先执行；②dm_als_init 的 lv_timer_create 失败时 orb_unsubscribe 释放 fd（原 fd 泄漏 + 亮度永远 "--"）；③standby_lux_lbl 创建先于首次 standby_health_rings_refresh（原锁屏首帧无亮度行，1s 后才补）；④暗光状态机注释修正（实际=连续暗光只弹一次；恢复后再暗按冷却限频）。排查通过项：timer 自身回调内删除安全、lv_init 先于 init_sensors、弹窗与 standby_click 无冲突、单线程无竞态。

### ✅ P154 上板"亮度 0"驱动修复（ok-20260904-24）
上板验证发现锁屏「亮度 0」而非真实环境光。根因链：ltr553_register 已被板级调用（r528_boot.c:814，i2c_bus2）✅、框架订阅自动 activate（sensor.c:753 nsubscribers 0→1 调 activate(true)）✅、驱动已 push——但 **ltr553_thread ALS 段两处缺陷**：①I2C 读取失败（ret<0）后仍无条件 push `priv->lux`（activate 时重置为 0.0f）→ 读失败即误报 0；②STATUS(0x8C) ALS 数据有效位从未检查，转换未完成就读 CH0/CH1 强算。修复（ltr553.c，谨慎最小改动）：读取失败 `continue` 跳过本次采样；加有效位检查才计算并 push。**⚠️ P154 有效位误用 bit1，被 P155 纠正（见下）**。

### ✅ P155 "亮度 0"真根因（ok-20260904-25，datasheet + Android 同芯片驱动交叉验证）
用户嫌 P154 不到位，用 nuttx-driver-development skill 重新审计：拉取 **LTR-553ALS-01 datasheet** + Android 同芯片驱动（android_kernel_lenovo_msm8937 ltr553.c）交叉验证，结论：
- **STATUS(0x8C) ALS 数据有效位 = bit7(0x80)、低有效**（0x00=新数据有效可读；0x80=无效/更新中须丢弃等待）。**P154 误用 bit1——那是 PS 中断位**（datasheet: Interrupt_Status = Data & 0x0A）→ 有效数据被 `continue` 误判丢弃，根本没读到；
- 正确流程：**先查状态 bit7==0 有效 → 才读 CH0/CH1**（读取数据寄存器会清除就绪标志，无效时读会丢"数据就绪"边沿）；
- **寄存器配置本身正确非根因**：ALS_CONTR(0x80)=0x01（Gain bits[4:2]=000=1X + SW_reset bit1=0 + ALS_mode bit0=1 active，datasheet 官方示例 Gain X1 就是 0x01）；ALS_MEAS_RATE(0x85)=0x08（积分时间 <<3 与 Android 驱动 `(integration_time<<3)|rate` 一致）；
- **诊断缺口**：CONFIG_DEBUG_SENSORS_ERROR/WARN/INFO 全未开 → 驱动 snerr/sninfo 全被编译掉，"日志无输出"不能证明 I2C 正常（P154 误判依据）→ defconfig 开 CONFIG_DEBUG_SENSORS_ERROR=y + WARN=y，下次上板 I2C/激活错误可见。
**待上板复测**：锁屏显示真实 lux；若仍异常，新开的 SENSORS_ERROR/WARN 日志可区分 I2C 失败 vs 未激活 vs 硬件。

### ⏳ P156 开机误弹窗 + 亮度 0 诊断升级（ok-20260904-26）
上板 ok-20260904-25 后用户反馈：**亮度依然 0，且一开机就弹"知道了"**。弹窗是关键铁证：
- **弹窗=数据链路全通**：暗光弹窗要 lux<50 持续 5s 才触发 → 驱动确实在推 lux=0、应用确实收到 → 订阅/activate/read/push/应用全链路 OK，问题收窄为 **CH0 读回就是 0**；
- **开机即弹=驱动首个有效 ALS 转换前推 0**（activate 后 lux=0.0 直至首帧转换完成）→ dm_als 加 `DM_ALS_STARTUP_MS=20000` 开机稳定期（dark_tick 早退），防开机误报；
- **弹窗 UX**：用户反馈"小小的都不知道是干啥的"→ 补 fa-sun 太阳图标（U+F185，COL_ORANGE）+ FONT_TITLE 大标题「光线有点暗」+ FONT_BODY 说明「开个灯看得更清楚，也更护眼～」+ 宽度 DM(360→420)；
- **诊断**：defconfig 补开 `CONFIG_DEBUG_SENSORS_INFO=y` → ltr553 的 `sninfo("LTR553: ALS: x.xx lux (CH0=, CH1=)")` 上板可见，一次区分 CH0=0/I2C 失败/status 异常；
- **minidisplay defconfig 对比**（用户提示）：LTR553/SHTC3/SGP30/I2C 配置与 nsh **完全相同**（minidisplay 只是 LUNCHER_MINI+LVGL demo 极简配置，无 AI/UART3/LD2410）→ 确认问题不在配置层。
**待上板**：看 sninfo 的 CH0/CH1/status 原始值定案；若 CH0 恒 0 → 检查 I2C 总线/传感器上电/遮光，若 CH0 有值但 lux=0 → 查计算分支。

### ⏳ P157 暗光弹窗"确认门"（ok-20260904-27）
用户：**"得有确切低亮度才弹，不要一开始就弹"**——把 20s 稳定期升级为数据级确认：新增 `DM_ALS_CONFIRM_LUX=200`，dm_als_dark_tick 先检查 **s_seen_bright**（曾见过 lux≥200 才算传感器真实工作）→ 未确认前暗光检测永不触发；确认后按原 50lux/5s 防抖/10min 冷却判定。恒 0 坏数据（未就绪/遮光/I2C 挂）从开机到永远都不弹。20s 稳定期保留作双保险（防确认后瞬间误报）。**待上板**：正常环境应不再开机弹窗；真暗环境（先亮后暗）仍会 5s 后弹。

## 七、review / 排查记录（本段）
- 文件浏览器手动 code review 修复 4 处隐患（缓冲/边界/竞态/资源释放）
- **经验教训**：①`strings vela.bin | grep 中文` 因编码空结果 ≠ 固件无此字符串——用 python 按 utf-8 count 验证；②解析 romfs 的脚本与 check_res.sh 逻辑不一致会误判"漏打包"——以 check_res.sh（已加固）为准；③flex 布局中 `lv_obj_set_align` 无效，定位须用"容器填满 + 内部贴边"或绝对坐标

---
*本节 9-04 子文档由 AtomCode 维护 · 2026-09-04*
