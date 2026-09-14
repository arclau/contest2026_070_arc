# DevLog 2026-08-10 part3 — 状态栏根治（子页/锁屏与 Home 同款）+ 文件管理器图标修复

> 本文档定位：8-10 晚间会话（-22~-26）**状态栏根治** 与 **文件管理器图标修复** 全过程。
> 上游：8-10devlog.md（-11~-13 状态栏恢复 + iPad 子页）、8-10part2devlog.md（P29 蓝牙 110 排查，
> 顺带完成 -21 子页状态栏换 create_status_bar_ex 共享构建器，但**未固化上板**）。

## 背景

用户上板 -20 后反馈三件事：
1. **子页状态栏只显示电池**，不显示 WiFi/蓝牙状态小图标（板端还是 -20 旧状态栏，-21 共享构建器没上板）
2. **子页内容与状态栏重叠**（Settings/蓝牙/WiFi 等所有子页都这样），返回键也压在状态栏上
3. **锁屏界面没有状态栏**

本 part 分五节点根治：-22 子页状态栏固化+内容下移初版 → -23 子页返回键/内容完善 → -24 锁屏状态栏修复（关键教训）→ -25 文件管理器存储/TF+工具栏图标 → -26 存储/TF 图标"上下拉伸条"修复。

---

## 节点 1：-22 子页状态栏固化 + 内容下移初版（固化 ok-20260810-22）

- **现象**：子页状态栏无 wifi/bt 图标；子页内容标题/卡片与顶部状态栏重叠
- **根因**：①-21 的 create_status_bar_ex（Home/子页同款 132px 全宽：时钟+天气+wifi/bt/battery）已提交但未编译上板，板端仍跑 -20 旧版 ②子页 content `pad_top=DM(48)≈115px < TOP_BAR_H(132px)`，内容从 y≈115 开始绘制，与 0~132 状态栏重叠
- **方案**：
  1. 固化 -21 共享构建器（create_status_bar_ex 五件套 + clock/weather/dm_net_status_refresh 三刷新链 + close_subpage 置空，代码完备无需改）
  2. `show_subpage` content `pad_top: DM(48) → TOP_BAR_H + DM(10)`
  3. `show_standby` 新增锁屏状态栏：standby_bar_clock/weather/wifi/bt_lbl 独立指针（standby_overlay 常驻不删除、无需置空），三钩子同步刷新（clock_update_cb / weather_update_cb / dm_net_status_refresh）
- **验证**：编译 0 错误 → pack → 固化 ok-20260810-22（镜像 md5 eb2638cc）
- **上板反馈**：**子页 OK**（wifi/bt/battery 图标齐全），但**锁屏"你把状态栏拉下来了"**——状态栏不在屏幕顶部

## 节点 2：-23 子页返回键 + 内容下移完善（固化 ok-20260810-23）

- **现象**：子页返回键（圆钮）压在状态栏上；内容仍与返回键重叠
- **根因**：返回键 `y=DM(4)≈10px`、高 `DM(40)≈96px` → 占 y≈10~106，全在状态栏 0~132 区域内；content pad_top（156）只避开了状态栏、没避开下移后的返回键（底边 `TOP_BAR_H+DM(44)`）
- **方案**（deskmate-ui skill 铁律 2：元素不冲突 + set_pos 后边界断言）：
  1. 返回键 `y: DM(4) → TOP_BAR_H + DM(4)`（=142px，状态栏下方）
  2. content `pad_top: TOP_BAR_H+DM(10) → TOP_BAR_H+DM(48)`（=247px，同时让出状态栏与返回键）
  3. 返回键 set_pos 后加 `assert(... && "deskmate: 返回键越界")`，补 `#include <assert.h>`
- **验证**：固化 ok-20260810-23（镜像 md5 c3924cd5），用户上板确认子页 OK

## 节点 3：-24 锁屏状态栏修复（关键教训，固化 ok-20260810-24）

- **现象**：用户反馈"你没有把整个页面往下拉，你是把状态栏拉下来了"——锁屏状态栏不在 y=0，整页也没下移
- **根因**（源码核实，非猜测）：
  1. `lv_obj_align` 是相对父容器**内容区**（含 pad）定位：`lv_obj_align` → `lv_obj_set_style_align + set_pos`；`lv_obj_align_to` 源码 `case LV_ALIGN_TOP_MID: y = btop`（btop = 父 `space_top` 即 pad）→ -23 的 `pad_top=132` 让 TOP_MID 落在 **y=132**，状态栏被 pad 顶下去了
  2. **FLOATING 子元素被 flex 布局跳过**（`lv_flex.c` 判 `LV_OBJ_FLAG_FLOATING` 跳过）→ sbar 位置完全由 align 决定，`lv_obj_align(TOP_MID)` 失效且被 pad 拉走
- **方案**：去掉 `pad_top + FLOATING + align` 三件套，让 **sbar 作为 flex column 的第一个普通子元素**：flex 从 y=0 顺序排布 → 状态栏钉死顶部，其余内容（光环/时钟/日期/音乐/健康卡）自然排在其下方，grow 间隔器在剩余空间内居中。状态栏不动、页面整体下移让位。
- **验证**：固化 ok-20260810-24（镜像 md5 06a56fac），用户确认锁屏 OK
- **教训**：LVGL 里 `lv_obj_align/set_pos` 均相对父**内容区**（padding 之后）；FLOATING 脱离布局但 align 基准仍含 pad——**固定顶部元素不要用"父 pad + 子 align"组合，直接让子元素当 flex 首项**最稳。

## 节点 4：-25 文件管理器图标（存储/TF + 复制剪切工具栏，固化 ok-20260810-25）

- **现象**：
  1. 内部存储 / TF卡 快捷入口图标**符号太小**（86px 圆角徽章里符号只有 ~16px）
  2. 多选工具栏（长按文件进入）复制/剪切等圆钮**"上半部分消失"**——圆圈边界与工具栏白色融在一起
- **根因**：
  1. 图标 label 漏设字体 → 走 LVGL 默认 16px（文件行/工具栏都显式 `FONT_ICON`，唯独存储图标漏了）
  2. 工具栏条白底 `COL_CARD@OPA_90`，按钮白圆 `COL_CARD@OPA_80`、**零描边零阴影** → 白圆叠白条边界不可见，只剩蓝色符号浮着
- **方案**（按修正版方案，对齐 skill 要求，用户拍板"搞了再说"）：
  1. 存储/TF 图标 label 补 `FONT_ICON`(36px)，与文件行/Settings 行同档
  2. 工具栏条白底 → **浅灰磨砂 `COL_BG_GRAD`@90%**（与页面背景同色系），白玻璃按钮靠明度差浮现
  3. 按钮加**柔和阴影**（黑 8px @ `LV_OPA_20`），**不描边**（玻璃配方=半透明白底+柔和阴影，更简约）
  4. 按钮间距 `DM(4)≈10px → DM(10)≈24px`（对齐 skill 留白 ≥20px 规范）
- **编译踩坑**：`LV_OPA_18` 不存在（LVGL 的 OPA 枚举只取 10 的倍数，skill 清单 15 明确要求）→ 改 `LV_OPA_20`，重编通过
- **验证**：固化 ok-20260810-25（镜像 md5 d8a02da0）

## 节点 5：-26 存储/TF 图标"上下拉伸条"（固化 ok-20260810-26）

- **现象**：-25 补 FONT_ICON 后，内部存储/TF卡图标出现**竖向"上下拉伸条"**（用户质疑"图标要这拉伸条干嘛"）
- **根因**：图标容器 `ib/tb` 用 `lv_obj_create()` 创建，**保留了默认 `LV_OBJ_FLAG_SCROLLABLE` + AUTO 滚动条**（只清了 CLICKABLE）——36px 字号后内容盒超出容器边缘，AUTO 滚动条触发显示。对照：文件行 `icon_box`、Settings `badge` 全部显式 `LV_SCROLLBAR_MODE_OFF + 清 SCROLLABLE`，唯独存储图标漏了
- **方案**：`ib/tb` 容器 + `ii/ti` label 各补 `LV_SCROLLBAR_MODE_OFF` + 清 `LV_OBJ_FLAG_SCROLLABLE`（与文件行/Settings 徽章完全一致）
- **验证**：固化 ok-20260810-26（镜像 md5 2b30757c），**用户确认问题解决** ✅

## 字体覆盖核实（-26 排查手段，可复用）

用 fontTools 检查三字体对 LVGL 符号私有区（U+F000~F8FF）的覆盖：

| 码点 | 符号 | fa-solid-900 | fa-brands-400 | MiSans-Normal |
|------|------|-------------|---------------|---------------|
| U+F015 | HOME（内部存储） | house ✓ | MISSING | MISSING |
| U+F7C2 | SD_CARD（TF卡） | sd-card ✓ | MISSING | MISSING |
| U+F114 | DIRECTORY（文件行） | folder ✓ | MISSING | MISSING |
| U+F001 | AUDIO | music ✓ | MISSING | MISSING |

- 结论：MiSans 完全不含 LVGL 符号码点，**LV_SYMBOL_* 渲染全部依赖 fallback 链 MiSans→fa-solid→fa-brands**；状态栏/文件行符号正常证明 fallback 生效，存储图标"拉伸条"与字形无关，是滚动条问题
- 命令：`python3 -c "from fontTools.ttLib import TTFont; ...cmap.get(0xF015)..."`

## 改动文件表（-22~-26）

| 文件 | 改动 |
|------|------|
| `luncher_dm/ui/ui_home.c` | `create_status_bar_ex` 共享构建器（-21 已提交）；`show_standby` 状态栏首版（-22）→ flex 首子元素方案（-24）；`clock_update_cb`/`weather_update_cb` 加 standby_bar_* 刷新钩子 |
| `luncher_dm/deskmate_ui.c` | `show_subpage` content pad_top / 返回键下移 + 边界断言（-23）；`dm_net_status_refresh` 加 standby 图标刷新；standby_bar_* 全局定义（**必须放在文件前部 Standby 区，deskmate_ui.c 不 include 自己头文件**） |
| `luncher_dm/deskmate_ui.h` | standby_bar_* extern 声明 |
| `luncher_dm/ui/ui_files.c` | 存储/TF 图标补 FONT_ICON + 滚动条关闭（-25/-26）；工具栏条浅灰磨砂/按钮柔和阴影/间距 DM(10)（-25） |

## 产物

| tag | 内容 | 镜像 md5 |
|-----|------|----------|
| ok-20260810-22 | 子页状态栏固化 + 内容下移初版 + 锁屏状态栏首版 | eb2638cc |
| ok-20260810-23 | 子页返回键下移 + content pad_top 加大 | c3924cd5 |
| ok-20260810-24 | 锁屏状态栏 flex 首子元素修复 | 06a56fac |
| ok-20260810-25 | 文件管理器存储/TF + 工具栏图标 | d8a02da0 |
| ok-20260810-26 | 存储/TF 图标滚动条拉伸条修复 | 2b30757c |

命令：`./build.sh <config> -j$(nproc)` → `lunch_nuttx 2 && pack` → `bash /data/vela/git_snapshot.sh`

## 节点 6：-27 状态栏 z-order 顶层修复（固化 ok-20260810-27）

- **现象**：Settings 等长页**向上滑动时内容覆盖状态栏**（AI 子APP 同样），用户质疑"状态栏按图层应该在顶层"
- **根因**：`show_subpage` 中 `content`（滚动容器）创建晚于 `sbar`（状态栏），LVGL 后创建的子对象绘制在上层；代码只对 `back_btn` 做了 `lv_obj_move_foreground`，**sbar 从未提到顶层** → 内容滚动滑入 0~132 状态栏区域时盖住它
- **方案**：`lv_obj_move_foreground(sbar)` 加在 `move_foreground(back_btn)` 之前（back_btn 保持最顶），一处修改全子页生效（Settings/WiFi/蓝牙/Music/Games/Books/Files/**AI** 全走 show_subpage）
- **验证**：固化 ok-20260810-27（镜像 md5 6e7efcf4），待上板

## 节点 7：-28 状态栏 WiFi/蓝牙图标改"开关语义"（固化 ok-20260810-28）

- **现象**：用户反馈状态栏 WiFi/蓝牙图标"不显示"——排查发现图标其实在渲染，但 WiFi(0x27 断开)/BT(110 关闭)都被染成 `COL_SEC` 浅灰，浅色底上 ≈ 不可见；电池恒绿所以显眼（"只显示电池"）
- **根因**：`dm_net_status_refresh` 用"颜色"表达状态（开=蓝/断=灰），灰在浅色半透明状态栏上对比度不足；用户拍板：**不要断开灰逻辑，套用 Settings 开关语义**——开了就显示符号，没开就不显示
- **方案**：重写 `dm_net_status_refresh` + 新增 `dm_status_icon_refresh(lbl, on)` 助手：开 → `clear_flag(HIDDEN)` + 蓝；关 → `add_flag(HIDDEN)` 整枚隐藏。Home/子页/锁屏三处指针统一处理（status_*/subpage_*/standby_bar_*），隐藏后 flex 自动收拢不占位。字体覆盖已核实：WIFI U+F1EB 在 fa-solid、BT U+F293 在 fa-brands（fallback 链 MiSans→fa-solid→fa-brands），符号渲染无问题
- **验证**：固化 ok-20260810-28（镜像 md5 837665d7），待上板——WiFi/BT 未开时状态栏只有电池；开了显示蓝色符号

## 节点 8：-29 音乐播放器播放列表下移（固化 ok-20260810-29）

- **现象**：音乐页右上角播放列表（三点菜单键 + 下拉卡片）跑进状态栏区域，用户要求"挪下来、跟返回键两对面"
- **根因**（读码计算）：
  - 三点菜单键 `lv_obj_align(TOP_RIGHT, -DM(16), DM(4))` → y≈10px，落在 0~132 状态栏内；且创建晚于 -27 的 `move_foreground(sbar)` → 盖在状态栏上层
  - 下拉卡片 `lv_obj_align(TOP_RIGHT, -DM(16), DM(52))` → y≈125px，卡片顶也在状态栏区域内
  - 返回键（-23 后）在**左下** `y=TOP_BAR_H+DM(4)`=142px —— 菜单键应镜像到右上同高
- **方案**：
  - 菜单键 `y: DM(4) → TOP_BAR_H+DM(4)`（=142px，与返回键同高、左右对开）+ 边界断言
  - 卡片 `y: DM(52) → TOP_BAR_H+DM(52)`（=257px，与菜单键保持原 DM(8) 相对间距：键底 +DM(44)、卡顶 +DM(52)）+ 边界断言
- **验证**：固化 ok-20260810-29（镜像 md5 7ea97099），待上板——播放列表按钮/面板不再压状态栏，与返回键对称

## 节点 9：-30 WiFi 密码弹窗 + 键盘优化（固化 ok-20260810-30）

- **现象**：WiFi 子页选中加密 SSID 后弹密码输入层，LVGL 键盘**完全挡住密码弹窗**（用户原话"全挡住了"）
- **根因**（读码计算，1920×1200 横屏）：
  - 密码弹窗 `box`：`lv_pct(80)` 宽 + `lv_obj_center` 居中，内容高≈418px → **y≈391~809**
  - LVGL 键盘 `kb`：默认 `height_def=LV_PCT(50)`=**600px**，`BOTTOM_MID` 贴底 → **y≈600~1200**
  - 弹窗下半（密码框 + 连接/取消按钮，y 600~809）全被键盘盖住
- **方案**（用户选定 C：弹窗上移 + 键盘压扁）：
  1. 弹窗 `lv_obj_center` → `lv_obj_align(TOP_MID, 0, TOP_BAR_H+DM(20))` → y≈180~598，**零重叠**
  2. 键盘 `lv_obj_set_size(kb, lv_pct(100), LV_PCT(35))` → 600px 压到 420px（键区更紧凑）
  3. **手动加入输入层**（"其他网络…"，同样结构）一并同样处理，保持一致
- **验证**：固化 ok-20260810-30（镜像 md5 9898995d），待上板——输密码时弹窗完整可见、键盘在下方不遮挡

## 节点 10：-31 WiFi 密码弹窗 redesign + 键盘恢复默认（固化 ok-20260810-31）

- **现象**：用户反馈 -30 的键盘压扁"不是压缩、是直接下移"，要求恢复键盘；同时密码弹窗"太丑、不符合设计美学，连接按钮不能那样搞"
- **方案**（用户拍板）：
  1. **键盘恢复默认**：去掉 -30 两处 `LV_PCT(35)`（50% 屏高贴底回原样）——弹窗已在状态栏下方、键盘上方，无需再压
  2. **密码弹窗 redesign**（对齐 skill 玻璃配方 + Settings/WiFi 卡片语言）：
     - box 补 Liquid Glass 卡片：半透明白底 `COL_CARD@OPA_90` + 白边 1px@50 + 柔和阴影 10@20 + pad DM(24)/行距 DM(16)
     - 标题加 `LV_SYMBOL_WIFI` 图标前缀 + `LONG_DOT` 防长 SSID 溢出
     - 输入框白底 + `COL_SEP` 灰边@60 + 圆角 DM(8) + pad DM(8)
     - 按钮区加 `pad_column DM(8)` 间距；**连接=主按钮**（`COL_BLUE` 底白字圆角 `RAD_CARD` 高 DM(40)），**取消=次按钮**（`COL_BG_GRAD` 浅灰底深字同款）
  3. **手动加入弹窗**（"其他网络…"）同款 redesign，保持一致
- **验证**：固化 ok-20260810-31（镜像 md5 6348785d），待上板

## 节点 11：-32 WiFi 密码弹窗缩小 + 上移 + 眼睛图标（固化 ok-20260810-32）

- **现象**：用户反馈卡片太大、键盘仍挡一点点窗口，要求：卡片缩小 50%、密码框右侧加眼睛图标切换明文、卡片再上移
- **方案**：
  1. 卡片宽 `lv_pct(80)` → `lv_pct(40)`（缩小 50%），上移 `TOP_BAR_H+DM(20)` → `+DM(8)` → 键盘完全不挡
  2. 密码框改为**行容器**（输入框 flex_grow + 眼睛圆钮 DM(40)）：`wifi_ta_eye_cb` 切换 `lv_textarea_set_password_mode` 并换 `LV_SYMBOL_EYE_OPEN`/`LV_SYMBOL_EYE_CLOSE`（图标取自 fa-solid eye U+F06E / eye-slash U+F070，已核实字形存在）
  3. **手动加入弹窗**同款处理（缩小 + 上移 + 密码眼睛），回调需前向声明（手动弹窗在文件前部）
- **验证**：固化 ok-20260810-32（镜像 md5 500f3987），待上板

## 节点 12：-33 眼睛按钮缩小 50% + 纵向居中（固化 ok-20260810-33）

- **现象**：用户反馈 -32 的眼睛按钮太大 → 缩小 50%，并要求与密码栏纵向居中
- **方案**：
  1. 眼睛按钮 `DM(40)` → `DM(20)`（两处弹窗 replace_all）
  2. 密码行容器（pwd_row/psk_row）显式 `lv_obj_set_flex_align(START, CENTER, CENTER)` → 眼睛按钮与密码栏 cross 轴纵向居中
- **验证**：固化 ok-20260810-33（镜像 md5 74ba32f0），待上板

## 遗留事项

- 🔴 **蓝牙 H4 110 / WiFi 0x27 待上板硬件排查**（示波器测 PG6/PG7 波形、芯片 BT 域使能、ROM 波特率、供电；勿擅改驱动，见 8-10part2devlog.md）
- 上板回归：Files/Books/Games UI（-21~-25 未动）、蓝牙各子页功能回归（-13 iPad 风格）

*DevLog by AtomCode (deepseek-v4-flash)*
