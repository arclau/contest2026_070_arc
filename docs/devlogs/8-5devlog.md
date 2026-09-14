# DevLog 2026-08-05 — 子 APP UI 统一修整 + Music 播放器移植重写 + Settings 重设计 + 崩溃修复

> 工程：openvela (R528) — nsh 配置
> 工作目录：`/data/dm`
> 工具链：SDK 自带 prebuilts/gcc/linux-x86_64/arm-none-eabi (GCC 13.4.0)
> 打包流程：`lichee` 下 `source envsetup.sh && lunch_nuttx 2 && pack`，镜像输出至 `lichee/out/r528s3/gemini-s1_nand/`
> **AI 交接：新会话请先读 `AGENTS.md`，本文件是当日归档，仅按需查阅。**

---

## 一、任务背景

用户反馈：各子 APP（Music/Games/Books/Files/AI/Settings）元素杂乱、比例不协调、无统一设计语言、布局扁平；锁屏偶发残留 dock 图标；Settings 无法滚动、字体小、点开崩溃。本轮目标是：统一子页设计语言、按 Music 风格修整其余子页、修复多处 BUG、重设计 Settings。

## 二、子 APP UI 统一修整（第一轮）

**目标**：全部子页统一为「白色磨砂玻璃卡 + 彩色圆角 badge + 垂直居中 + FONT_BODY/FONT_CAPTION 字号体系」。

| 文件 | 改动 |
|------|------|
| `deskmate_ui.c` — show_subpage | 返回按钮三版演进：DM(44) 黑底圆钮（丑/大）→ 纯箭头 label（太小不合触摸规范）→ **DM(40) 白色磨砂玻璃圆钮**（COL_CARD opa 80，最终定稿，左上角 DM(16),DM(4)） |
| `create_games_subpage` | 纯色卡（红/绿/蓝/紫底）→ **style_card 白色玻璃卡**；彩色仅保留在 DM(56) badge；网格垂直居中 |
| `create_books_subpage` | hero 红底 → 白色玻璃卡；**修复白卡上白字不可见 BUG**（红底时代残留的白色文字改深色）；封面加同色光晕 |
| `create_music_subpage` | 占位 → 移植 LVGL music demo（见下节） |
| `create_files_subpage` | storage 进度条加粗蓝紫渐变；文件行图标改彩色 badge |
| `create_ai_subpage` | quick action 卡横排→竖排（消"扁"）；图标改彩色 badge + 光晕 |

**验证**：编译打包通过（nsh.fex==vela.bin）；模拟器同步可预览。

## 三、Music 子 APP：LVGL demo 移植 → HTML 布局重写

### 3.1 从 LVGL music demo 移植（`/data/lv_port_linux/src/music_player/`）

- 内联进 `deskmate_ui.c`（静态函数+数据表），sync 单文件同步两端一致
- **去掉开场动画**：demo 的 `auto_step_cb` 自动演示（自动切歌/滚动/FPS 弹层）与 intro 滚动全部未引入；进度 timer 创建即暂停，不自动播放
- 尺寸全部 `DM(x)` ×2.4 标定 1920x1200；字体 MiSans 替换 montserrat
- 10 首真实曲目数据（Waiting for true love 等），播放/暂停/切歌/进度条模拟
- 生命周期安全：`close_subpage` 删除音乐 timer，防悬空访问

### 3.2 按 `desktop-mate-ui.html` 重写布局（用户要求）

HTML 布局特点：整页只有一个居中播放器（封面→信息→进度条→控制键），**歌单不在主页面**，藏在右上角三点按钮的全屏浮层里。

| 结构 | 实现 |
|------|------|
| 大封面 | DM(120) 蓝紫渐变 + 光晕，居中；负 margin 上移使底边持平 |
| 歌曲信息 | 歌名 FONT_TITLE(110)、艺术家 FONT_BODY |
| 进度条 | DM(6) 粗条 + 当前/总时长双显（FONT_BODY） |
| 控制键 | prev/play/next 白色磨砂玻璃圆钮，play 带蓝色光晕 |
| 三点按钮 | 右上角 DM(40) 与返回按钮同大，磨砂玻璃，三个手绘圆点（板端无 LV_SYMBOL_ELLIPSIS） |
| 歌单浮层 | 全屏 40% 遮罩 + 居中白卡（Playlist 标题 + 关闭 ✕ + 滚动列表），点曲目播放+高亮+自动关闭 |

**关键 BUG 修复**：playlist overlay 之前挂 `lv_scr_act()`（主屏），子页关闭后残留全屏遮罩挡主屏且重复叠加 → 改挂 `subpage_overlay`，随子页删除。

## 四、BUG 修复清单

### 4.1 锁屏残留 dock 7 图标（竞态）

| 现象 | 根因 | 修复 |
|------|------|------|
| 锁屏界面偶发显示 dock 图标 | 子页打开期间自动锁屏（子页停留 30s），唤醒时 `wake_from_standby` 无条件恢复 dock；`close_subpage` 也无条件恢复 | ① `wake_from_standby` 仅当无子页打开时恢复 dock；② `close_subpage` 检查 standby 未显示才恢复；③ 锁屏时 dock 仅加 HIDDEN（不再 move_background） |

### 4.2 dock 跑到状态栏上方（上一轮修复引入的回归）

| 现象 | 根因 | 修复 |
|------|------|------|
| 锁屏唤醒后 dock 出现在状态栏上方 | 上一轮在 `show_standby` 加了 `lv_obj_move_background(g_dock)`，但主屏 scr 是 **flex column 布局，子对象顺序即布局位置**——dock 被从底部挪到最前（状态栏上方） | 删除 move_background，只保留 HIDDEN（standby_overlay 全屏不透明 + move_foreground 足够盖住） |

### 4.3 Files/Books 子页崩溃（munmap_chunk: invalid pointer）

| 现象 | 根因 | 修复 |
|------|------|------|
| 点开 Files 后立即 `munmap_chunk`（日志权威：`Open subpage: Files` 后崩） | **LVGL 9.1 `lv_bar` 的 `LV_PART_INDICATOR` 渐变 + `LV_RADIUS_CIRCLE` 组合走 layer/mask 分配路径导致堆损坏** | Files + Settings 的 storage 进度条改为纯色 COL_BLUE + 普通圆角 DM(2)，避开 layer 路径 |

### 4.4 Settings 点开崩溃（回归，同源）

| 现象 | 根因 | 修复 |
|------|------|------|
| Settings 一点就崩 | 按用户要求"亮度条抄 Music 进度条"时，把**渐变 + LV_RADIUS_CIRCLE 组合**（4.3 的同款雷）抄进 settings slider；Music slider 一直没崩只因用户从未打开过 Music | settings slider + Music slider 两处都改纯色 indicator + DM(3) 数值圆角；代码注释标注该 LVGL 9.1 陷阱防再犯 |

### 4.5 Settings 无法滚动

| 现象 | 根因 | 修复 |
|------|------|------|
| Settings 六组列表无法下滑 | `show_subpage` 里 `lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE)` 把内容区滚动能力误清（注释还写着 scrollable） | 移除该行，内容区保持 SCROLLABLE |

## 五、全 APP 字体放大（最小字号 = FONT_BODY）

用户要求：**整个 APP 字体最小不能比音乐播放器里的倒数时间小**（FONT_BODY 56px）。

- Settings：section 标题、3 处行图标（WIFI 符号等）、值文字（75% 亮度数值）、箭头 → 全部 FONT_BODY
- 锁屏：日期、提示文字 → FONT_BODY
- 其余子页次级文字 17 处 `search_replace`：FONT_CAPTION(34) → FONT_BODY(56)，文件内 FONT_CAPTION 仅剩宏定义行

## 六、Settings 重设计（用户决策驱动）

用户通过问卷确认四项决策：保持 6 组 / 只接能实现的 / 分组卡+图标光晕强化 / iOS 浅色分组列表。

### 6.1 视觉

| 项 | 实现 |
|----|------|
| iOS 大标题 | "Settings" FONT_TITLE + 底部留白 |
| 分组卡 | 圆角 `RAD_CARD`(55px)、阴影 16px/opa 20%、组间距加大（section pad_top 20） |
| icon badge | DM(36)、彩色光晕阴影（同色 opa 30）、图标 FONT_ICON(76) |

### 6.2 真实功能（调查结论）

| 设置项 | 板端接口 | 实现 |
|--------|---------|------|
| Brightness 滑块 | `dm_led_set_brightness(0-100)`（LEDC，board-only） | ✅ 拖动真实调 LED 亮度 + % 实时刷新（`CONFIG_LED_RGB_WS2812` 条件编译，模拟器跳过） |
| Auto-Lock | `IDLE_TIMEOUT` 宏 → `g_idle_timeout` 变量 | ✅ 点击循环 30s/1min/5min/Never，写入变量并重置倒计时，真实生效 |
| Text Size | freetype 固定字号创建，label 创建时绑定字体 | ⚠️ 保持演示态（切换需重建整个 UI，成本高，代码注释说明） |

## 七、iPad 圆角体系（最后定稿）

283PPI 面板，1pt ≈ 3.93px；搜索结果：iPadOS insetGrouped 列表卡默认 10pt、HIG 卡片 16pt、按钮 12pt、小元素 8pt。

```c
#define RAD_CARD    55   /* 大卡片 ≈14pt（用户指定 55px） */
#define RAD_BADGE   20   /* badge/封面/天气日卡/dock 按钮 ≈5pt */
#define RAD_SMALL   12   /* 小 badge/迷你封面/歌单行 ≈3pt */
```

统一应用 13 处：`CARD_RADIUS` 48→55（style_card 全局，含 home 首页天气/AI 卡）、Settings 分组卡、3 处 Settings badge、Games badge 14→20、AI badge 9→20、Files badge 6→12、Books 封面 10→20、迷你封面 8→12、歌单行 8→12、天气日卡 28→20、dock 按钮 14→20；圆形按钮保持 LV_RADIUS_CIRCLE；dock 胶囊背景保留。

## 八、工具链 BUG：sync_deskmate.sh 误报构建成功

| 现象 | 根因 | 修复 |
|------|------|------|
| 用户反馈"模拟器没更新" | `deskmate_ui.c` 新增 `#include "lv_demo_panel_rgb_control.h"`（板端目录才有），模拟器编译 fatal error；但 sync 脚本只检查二进制是否存在（旧二进制还在）→ 误报 `lvglsim OK` 并用旧二进制重启 | include 与 `dm_led_set_brightness()` 调用用 `CONFIG_LED_RGB_WS2812` 宏包裹（板端 Makefile 已定义、模拟器没有）；修复后模拟器构建 0 错误、二进制更新、两端 md5 一致 |

**经验教训**：sync_deskmate.sh 应校验二进制时间戳或构建日志，不能只检查文件存在。

## 九、验证

```bash
# 每轮改动均执行
cd /data/dm && ./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh -j$(nproc)   # 0 error 0 warning
cd /data/dm/vendor/allwinnertech/lichee && source envsetup.sh && lunch_nuttx 2 && pack
ls -la .../image/nsh.fex /data/dm/nuttx/vela.bin   # 字节一致
bash /data/vela/sync_deskmate.sh -r                # 板端→模拟器 + 重建重启
```

- 最终产物：`nsh.fex == vela.bin`（5311936 bytes）
- 模拟器：构建 0 错误，web 预览可交互；崩溃复现路径（Settings 打开）验证不再崩

## 十、全 APP 圆角统一 = 40px（追加，用户指定范围）

**背景**：用户要求"每个子 APP 里的卡片、插件的角都改成 Settings 卡片里的圆角度"，并确认范围 = **除圆形按钮/进度条外全部 40px**。Settings 卡片圆角即 `RAD_CARD`（40px）。

**改动明细（`apps/luncher_dm/deskmate_ui.c`，模拟器同步副本一致）**：

| 文件 | 改动 |
|------|------|
| `deskmate_ui.c` | 13 处 `lv_obj_set_style_radius(x, RAD_*, 0)` 统一改为 `RAD_CARD`：Settings/子页图标徽章 ×6（原 20）、天气卡每日小格 ×5（原 20）、专辑封面（原 36）、书籍封面（原 20）、迷你封面（原 12）、歌单行（原 12）、Files 文件徽章（原 12）、dock 底（原 28）+ dock 图标按钮（原 20） |
| `deskmate_ui.c` | 删除冗余宏 `RAD_BADGE(20)/RAD_SMALL(12)/RAD_COVER(36)/RAD_DOCK(28)`，仅留 `RAD_CARD 40`，更新注释说明例外项 |

**保留原值（有意为之）**：圆形按钮/触点圆点 `LV_RADIUS_CIRCLE`、进度条/slider `DM(2)/DM(3)`、分隔线/全屏遮罩 `0`。

**收益**：后续调圆角只需改 `RAD_CARD` 一处，全 APP 生效。

**验证**：
```bash
cd /data/dm && ./build.sh .../configs/nsh -j$(nproc)          # 0 error 0 warning
cd /data/dm/vendor/allwinnertech/lichee && source envsetup.sh && lunch_nuttx 2 && pack
md5sum out/r528s3/gemini-s1_nand/image/nsh.fex nuttx/vela.bin  # cfc1d0be… 一致（5311936 bytes）
bash /data/vela/sync_deskmate.sh -r                            # 模拟器同步 + 重建重启，web 预览已刷新
```

## 十一、子页 iOS 大标题 + 顶部对齐（追加，用户确认设计方向）

**背景**：用户反馈 Games/Books/Files/AI 不像 Settings——返回键下方没有当前子 APP 的大标题。用户明确要求**只同步设计风格**（大标题 + 圆角卡片），不改各 APP 功能形态（书仍是书架、文件仍是文件管理器），并提醒"不能照抄 Settings 的分组列表"。

**改动明细（`apps/luncher_dm/deskmate_ui.c`）**：

| 文件 | 改动 |
|------|------|
| `deskmate_ui.c` | 新增 `subpage_big_title()` helper（FONT_TITLE + COL_TEXT + pad_bottom 12，与 Settings big_title 同款写法） |
| `deskmate_ui.c` | Games/Books/Files/AI 四个子页：flex 主轴对齐由 `LV_FLEX_ALIGN_CENTER` 改 `LV_FLEX_ALIGN_START`（顶部对齐），函数开头调用 `subpage_big_title(parent, "Games"/"Books"/"Files"/"AI")` |
| `deskmate_ui.c` | Games 额外将 2×2 卡片网格包进 `grid` 容器（ROW_WRAP 移到 grid 上），大标题位于网格上方；Books/Files/AI 内容结构不变 |

**保留**：各子页原有内容形态（游戏卡网格 / 书架 hero+网格 / Storage 卡+文件列表 / AI hero+动作网格）；Music 播放器形态不动（居中播放器无需标题）。

**验证**：
```bash
cd /data/dm && ./build.sh .../configs/nsh -j$(nproc)          # 0 error 0 warning
cd /data/dm/vendor/allwinnertech/lichee && source envsetup.sh && lunch_nuttx 2 && pack
md5sum out/r528s3/gemini-s1_nand/image/nsh.fex nuttx/vela.bin  # 5a332030… 一致（5311936 bytes）
bash /data/vela/sync_deskmate.sh -r                            # 模拟器同步 + 重建重启，web 预览已刷新
```

**修复（用户反馈后）**：首次实现 Books/Files/AI 用了 `lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER)` → 交叉轴（水平）= CENTER，导致大标题横向居中；与 Settings（不设 flex_align）不一致。**删掉该行**（Games 本就未设），标题恢复左对齐；已同步模拟器 + 编译打包复验（md5 `4ab3d72d…` 一致）。**教训：子页布局对齐一律参照 Settings 写法，勿额外设置 flex_align 交叉轴。**

## 十二、遗留事项 / 下一步

1. **模拟器预览确认**：用户端浏览器需强刷（Ctrl+F5）才看得到新 UI（noVNC 缓存）；**子页大标题 + 顶部对齐 + 全 APP 40px 圆角**观感待用户在 web 预览确认（需微调只改 `RAD_CARD` / 标题样式）
2. **Text Size 保持演示态**：freetype 固定字号，切换需重建 UI；如要真实实现需评估重建方案
3. 上板验证：WiFi 联网取真实天气、dock PNG 渲染、内存/刷新率表现（延续）
4. 背景图 `/resource/imgs/luncher_mini_bg_new.png` 320x240 旧图待换 1920x1200（延续）
5. AI 卡仍为占位；传感器/LED/触摸测试入口未挂 Dock（延续）
6. **工具链改进建议**：`sync_deskmate.sh` 构建失败检测只认二进制存在，易误报成功——建议加时间戳/日志校验

## 十三、模拟器 Web 预览拉起 + 上板差异修复（符号口口 / 字体过大 / dock 失调）

> 本会话：① 拉起模拟器 web 预览链路；② 用户上板后发现与模拟器有差距（dock 变大显示不全、播放键等符号全部口口），比对两端源码定位根因并修复。

### 一、模拟器拉起

- 链路已齐（Xvfb :99 / x11vnc:5900 / websockify:9009 / http:8080），缺 lvglsim（旧进程已退出）→ 补拉 `/data/lv_port_linux/build/bin/lvglsim -b SDL -W 1920 -H 1200`
- 截图 + 直方图验证渲染正常（主色 #F2F2F7 浅色主题 / #2196F3 强调色，非黑屏）
- 访问入口：`http://192.168.*.*:8080/vnc.html?host=192.168.*.*&port=9009&autoconnect=1&resize=scale`

### 二、现象 → 排查 → 根因

**现象（上板）**：字体过大；dock 变大、显示不全、比例失调；播放键等符号全部口口。

**排查过程**：

1. **比对两端源码**：`deskmate_ui.c/.h`、`deskmate_icons.c/.h` diff = 0 差异（完全一致），`dm_weather.*` 仅差离线假数据 → **排除"UI 源码不一致"**（用户猜测"整个 APP 源码有问题"不成立，差异在渲染环境）
2. **验证字体文件在固件**：`res.fex`（09:01 与 09:02 固件同批）内 `fa-solid-900.ttf`（TTF 魔数@432128、名字表 'Font Awesome 6 Free Solid 6.7.2'）、`MiSans-Normal.ttf` 均在 → 非文件缺失
3. **符号口口根因**：UI 内全部 `LV_SYMBOL_*`（播放键、返回箭头、settings 行图标等）用 `FONT_ICON/BODY/TITLE` 渲染；板端这些 = **MiSans freetype 字体**，MiSans 不含 LVGL 符号码点（U+F000 私有区），且 freetype 字体 fallback 默认 NULL → 缺字形画口口。模拟器符号正常 = montserrat 内置字体自带全部 LV_SYMBOL 字形
4. **字体过大/dock 失调根因**：板端 MiSans 字号 110/76/56/66/34 是模拟器 montserrat 48/36/24/30/14 的 **2.3 倍**（800x480→1920x1200 过度标定）；dock 标签 66px 行高≈92px → dock 总高 154+2+92+16≈264px > DOCK_H=240 → 溢出裁切；模拟器 214px 正常
5. **fa-solid 覆盖验证**（fontTools 分析 cmap）：FA6 6.7.2 覆盖 LVGL 符号表 57/61，缺 `LV_SYMBOL_BLUETOOTH`(U+293，settings 页正用到)、USB、NEW_LINE、BULLET → **弃用 fa-solid fallback 方案**，改 montserrat 内置

### 三、修复（文件 | 改动）

| 文件 | 改动 |
|------|------|
| `configs/nsh/defconfig` | 启用 `CONFIG_LV_FONT_MONTSERRAT_14/24/30/36=y`（12/16/48 已有） |
| `luncher_dm.c` | `init_fonts()`：六档字号 110/76/56/66/34 → **48/36/24/30/14**（对齐模拟器观感）；每个 MiSans 字体 `->fallback = &lv_font_montserrat_<同字号>`（LV_SYMBOL 字形走内置 montserrat，与模拟器渲染路径一致）；`dm_font_symbol`(fa-solid 140px) 保留为备用（UI 未引用 FONT_SYMBOL） |
| `deskmate_main.c`（模拟器） | 更新过时注释（板端字号已对齐，不再 "stand in 110/76/56/66/34"） |

**说明**：未改 `deskmate_ui.c/.h` → 无需跑 `sync_deskmate.sh`；模拟器行为本就用 48/36/24/30/14（montserrat 自带符号），**新板端固件效果 = 当前模拟器预览**，可直接在 web 预览核对。

### 四、验证

```bash
cd /data/dm && ./build.sh .../configs/nsh -j$(nproc)      # 0 error（仅既有 gt9271 警告）
cd /data/dm/vendor/allwinnertech/lichee && source envsetup.sh && lunch_nuttx 2 && pack
md5sum out/r528s3/gemini-s1_nand/image/nsh.fex nuttx/vela.bin  # cffc5299… 一致（5537220 B）
# 镜像: out/r528s3/gemini-s1_nand/rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img (27871232 B)
```

### 五、遗留 / 下一步

1. **上板验证新固件（重点）**：dock 不再溢出、符号（播放键/返回箭头/settings 图标）不再口口、整体字号与模拟器一致
2. `dm_font_symbol`(fa-solid 140px) 无人引用，后续可删或留给 dock 字形需求
3. 蓝牙符号走 montserrat 内置字形（36px），观感与 fa-solid 实心图标略异——如不满意可换 fa-brands-400.ttf 做蓝牙专用 fallback
4. 其余延续：背景图 1920x1200、AI 卡换功能、Text Size 演示态、sync 脚本误报改进

## 十四、全 APP 渲染环境审计 + dark 主题对齐（P5）

> 用户要求"整个 APP 查一遍"类同"模拟器正常、上板异常"的差异点（例：状态栏 WiFi/蓝牙/电池），主动审计而非等刷机后才发现。

### 审计结论（逐项盘点）

| 依赖点 | 结论 |
|--------|------|
| 状态栏 wifi/bt/bat（LV_SYMBOL_WIFI/BLUETOOTH/BATTERY_FULL + FONT_ICON） | ✅ P4 已修（montserrat fallback 覆盖） |
| 全 APP 35+ 个 LV_SYMBOL（PLAY×4/KEYBOARD×4/FILE×4/EYE_OPEN×4…） | ✅ 全部走 5 个 MiSans 字体 + fallback |
| dock 图标（`deskmate_icons.c`） | ✅ 内嵌 **raw ARGB8888 数组**（非 PNG，无解码依赖；B,G,R,A 字节序与 LVGL9 little-endian 匹配） |
| 天气 canvas（`w_icon_buf`） | ✅ P3 已修（ARGB8888 + uint32_t） |
| label 字体 | ✅ 70/70 显式设字体，无默认字体依赖 |
| LV_COLOR_DEPTH | ✅ 板 32 / 模拟 32 一致 |
| LV_DPI_DEF | ✅ 板 130（Kconfig 默认）/ 模拟 130 一致 |
| PNG 解码 | ✅ 板 LIBPNG / 模拟 LODEPNG 都有（UI 无 PNG 使用） |
| 图像 header 缓存 | ✅ 板 128 / 模拟 0，无害（模拟 0 也正常渲染 dock） |
| **LV_THEME_DEFAULT_DARK** | ❌ **板端=y（暗色）vs 模拟器=0（浅色）→ 差异** |

### dark 主题影响面（为何要修）

- **switch**（deskmate_ui.c:580）完全未设色 → 走主题默认，上板偏暗
- settings slider knob 未设色（仅 music_slider 设白）→ 主题色
- 滚动条（Settings 页 SCROLLABLE）→ 主题色
- 屏幕背景：scr 未显式设色、顶层 main_area 透明 → 露主题背景色
- 模拟器浅色主题 = 用户认可的观感 → 板端必须对齐

### 修复（文件 | 改动）

| 文件 | 改动 |
|------|------|
| `configs/nsh/defconfig` | 删除 `CONFIG_LV_THEME_DEFAULT_DARK=y`（LVGL 9 Kconfig 默认 n = 浅色主题，对齐模拟器） |

未改任何 C 源码 → 无需 sync 模拟器。

### 验证

```bash
cd /data/dm && ./build.sh .../configs/nsh -j$(nproc)      # 0 error
cd /data/dm/vendor/allwinnertech/lichee && source envsetup.sh && lunch_nuttx 2 && pack
md5sum out/r528s3/gemini-s1_nand/image/nsh.fex nuttx/vela.bin  # 249ac25a… 一致
# 镜像: out/r528s3/gemini-s1_nand/rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img (27871232 B)
```

### 遗留 / 下一步

1. 上板验证 249ac25a：switch/滑块 knob/滚动条/背景为浅色、与模拟器一致
2. `LV_USE_DRAW_G2D`（板端 GPU 加速 vs 模拟器软件渲染）为潜在差异，上板 UI 渲染正常故未动；若后续出现局部图元异常优先查 G2D
3. 蓝牙符号观感（P4 遗留，montserrat vs fa-solid）待确认

---

## 十五、Git 版本管理体系建立 + 音乐播放器修复（P6-P7 追加）

### 背景

经历了"builtin registry 清空导致系统起不来"后，决定建立 git 版本管理体系，并把"每次编译完自动固化"写死为硬性规则。

### 1. Git 版本管理体系（2026-08-05）

**现状**：工程为 openvela repo 多仓结构（顶层无 git），改动分散在三个独立仓：`vendor/allwinnertech` / `nuttx` / `apps`，各仓 detached HEAD。

**新增工具**：`/data/vela/git_snapshot.sh`（三仓一键固化/恢复）

```bash
bash /data/vela/git_snapshot.sh                 # 固化三仓 + 自动 tag（ok-日期-序号）
bash /data/vela/git_snapshot.sh -t <tag>        # 固化 + 指定 tag
bash /data/vela/git_snapshot.sh -r <tag> [-y]   # 恢复三仓到稳点
bash /data/vela/git_snapshot.sh -l              # 看 tag 列表
```

**固化点**：`ok-0805-sdcard-fix`（vendor=04ee7cb3 / nuttx=f6d360be37e / apps=2699cf961），对应固件 md5 `f3e10224`。

**规则（已写入 AGENTS.md 第八章）**：AI 每次编译打包成功后必须自动运行 `git_snapshot.sh`，不等用户提醒；会话结束前检查三仓工作区，有未提交改动视为任务未完成。

### 2. 歌名显示不全根因：FAT_LFN 未启用

**现象**：清单歌名显示为 `BEAUTI~1.MP3`（8.3 短名）。

**根因**：`CONFIG_FAT_LCNAMES=y` 只是小写支持，**长文件名需 `CONFIG_FAT_LFN=y`**（未开启时 readdir 返回 8.3 短名）。

**修复**：defconfig 加 `CONFIG_FAT_LFN=y`。

### 3. WAV 播放崩溃根因更正：SDMMC 多块读超时（非 spinlock）

**现象**：播放 WAV 时 playthread Data Abort（PC 4143821c, DFAR=eafffffe），spinlock 修复后依旧。

**根因（用新固件 addr2line 重新解析）**：
```
nxplayer_playthread → fat_read → mmcsd_readmultiple
→ sunxi_mmc_mult_sendcmd → HAL_SDC_Request
→ do_rom_HAL_SDC_Request(hal_sdhost.c:1674)  ← SDMMC 命令超时错误分支
```
- 清单能读（单块读 readsingle/CMD17）✅，播放崩（多块读 readmultiple/CMD18+DMA）❌
- `CONFIG_MMCSD_MULTIBLOCK_LIMIT=0`（多块无限制）→ 大文件走多块 DMA 读，命令 20s 未完成 → 超时 dump 路径访问坏 sg buffer（DFAR=eafffffe）崩溃
- **spinlock 修复方向错误**（无害但未解决此崩溃）

**修复（验证性）**：defconfig 加 `CONFIG_MMCSD_MULTIBLOCK_LIMIT=1` 强制单块读（与清单读取相同路径），绕过多块 DMA bug。若上板确认不崩，坐实多块读路径问题，后续可深挖 DMA 根治。

### 4. 音量预设 75%

`music_audio_start` 中 nxplayer 创建后 `nxplayer_setvolume(750)`（0-1000 制，默认 400=40%）。

### 5. 清单 UI 风格重做

- 遮罩 40%→20%、卡片 DM(190)×240→DM(180)×230 + RAD_CARD 圆角 + 阴影
- 行高 DM(30)→DM(34)、歌名 `LV_LABEL_LONG_WRAP` 换行全显
- 修 LVGL 9.1 无 `LV_OPA_15`（→LV_OPA_20）编译错误

### 验证（文件 | 改动）

| 文件 | 改动 |
|------|------|
| `defconfig` | `CONFIG_FAT_LFN=y`、`CONFIG_MMCSD_MULTIBLOCK_LIMIT=1` |
| `deskmate_ui.c` | 音量 750 预设、清单 UI 风格重做 |
| `AGENTS.md` | 新增第八章 Git 版本管理（含自动固化硬性规则） |
| `/data/vela/git_snapshot.sh` | 新增：三仓一键固化/恢复脚本 |

```bash
cd /data/dm && ./build.sh .../configs/nsh -j$(nproc)      # 0 error
cd /data/dm/vendor/allwinnertech/lichee && source envsetup.sh && lunch_nuttx 2 && pack
md5sum out/r528s3/gemini-s1_nand/image/nsh.fex nuttx/vela.bin  # f3e10224… 一致
bash /data/vela/git_snapshot.sh -t ok-0805-sdcard-fix    # 固化完成
```

### 遗留 / 下一步

1. **上板验证 f3e10224**：WAV 播放不再死机（单块读）+ 音量 75% + 清单长文件名 + 新样式
2. **多块读 DMA 根治**（后续优化）：单块读为权宜；深挖 `sunxi_mmc_mult_sendcmd` DMA 描述符/中断竞争，修复后恢复多块读提速
3. **builtin registry 坑**：distclean 中断后 register_all 未触发 → builtin 命令全丢（luncher_dm 起不来）。下次 distclean 后重跑 `make register_all`；固化流程已防此坑

---

*DevLog by AtomCode (deepseek-v4-flash)*
