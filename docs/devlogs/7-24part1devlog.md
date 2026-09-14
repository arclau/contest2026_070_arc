# Vela Desktop Mate — 开发日志 (DevLog)

> 最后更新：2026-07-24
> 对应技能：`deskmate-ui`（详见 `.atomcode.md`）

---

## 零、v4.0 — LVGL Layout System Refactor（2026-07-24）

### 背景

按照 desktop-mate-ui 技能 **Part 2 LVGL Layout System** 规范重构代码结构。目标是 **Simple Layout** — 不使用 Web/Qt/Flutter 式布局，优先 align > flex > align_to > set_pos。

### 变更清单

#### 1. Screen 三级结构（Header / Content / Dock）

```
Screen
├── Header   (固定顶部 96px) → 状态栏
├── Content  (填充中间区域)   → 大时钟 + 天气卡 + AI 卡 + 锁屏按钮
└── Dock     (固定底部 176px) → 应用栏
```

- ❌ 之前：所有元素直接挂载 `scr`（平铺 ~10 个子对象）
- ✅ 现在：Screen 只有 3 个直接子容器，零浮动 Label/Button/Card

#### 2. 零 `set_pos` — 替换为 align / flex

| 位置 | 原写法 | 新写法 |
|------|--------|--------|
| 状态栏 `bar` | `lv_obj_set_pos(bar, 0, 0)` | `lv_obj_align(bar, LV_ALIGN_TOP_MID)` |
| 通用卡片 `create_card` | 参数 `x, y` + `set_pos` | 调用方用 `lv_obj_align` / `lv_obj_align_to` |
| Dock 背景 | 手动 `dock_x = (sw-dock_w)/2` | `lv_obj_align(dock_bg, LV_ALIGN_CENTER)` |
| Dock 图标 | `set_pos(x, y)` 循环计算 | `lv_flex ROW` 自动排列 |
| 锁屏按钮 | `set_pos(EDGE_PADDING, sh-84)` | `lv_obj_align(BOTTOM_LEFT, EDGE_PADDING)` |
| Standby overlay | `set_pos(0, 0)` | 删除（尺寸已覆盖全屏） |
| Subpage overlay | `set_pos(0, 0)` | 删除 |
| Subpage topbar | `set_pos(0, 0)` | `lv_obj_align(TOP_MID)` |
| Settings 分组卡 | 3 处 `set_pos` + `cur_y` 追踪 | `lv_flex COLUMN` 自动排列 |
| Games 网格 | `set_pos` 手动 2×2 | `lv_flex ROW_WRAP` |
| Books hero | `set_pos(EDGE_PADDING, 80)` | `lv_obj_align(TOP_MID, 0, 80)` |
| Books prog_fill | `set_pos(0, 0)` | `lv_obj_align(LEFT_MID)` |
| Books 书架 | `set_pos` 手动 2×2 | `lv_flex ROW_WRAP` |
| Files storage | `set_pos(EDGE_PADDING, 80)` | `lv_obj_align(TOP_MID, 0, 80)` + 共享样式 |
| Files file_list | `set_pos(EDGE_PADDING, list_top)` | `lv_obj_align_to(storage, OUT_BOTTOM_MID)` |
| AI hero | `set_pos(EDGE_PADDING, 80)` | `lv_obj_align(TOP_MID, 0, 80)` + 共享样式 |
| AI 快捷入口 | `set_pos` 手动 2×3 | `lv_flex ROW_WRAP` + 共享样式 |

#### 3. 共享样式

新增 `init_shared_styles()` + `static lv_style_t style_card`，统一 Liquid Glass 配方：

```c
lv_style_set_bg_color(&style_card, lv_color_hex(COLOR_CARD));
lv_style_set_bg_opa(&style_card, LV_OPA_80);
lv_style_set_border_width(&style_card, 0);
lv_style_set_radius(&style_card, CARD_RADIUS);
lv_style_set_shadow_width(&style_card, 8);
lv_style_set_shadow_color(&style_card, lv_color_hex(0x000000));
lv_style_set_shadow_opa(&style_card, LV_OPA_10);
lv_style_set_pad_all(&style_card, 20);
```

使用 `lv_obj_add_style(card, &style_card, 0)` 一行替代原来 8 行重复属性设置。

应用范围：`create_card` → 天气卡 / AI 卡，`create_app_dock` → dock_bg，`create_files_subpage` → storage，`create_books_subpage` → bcard，`create_ai_subpage` → hero / 快捷卡

#### 4. Hidden > Delete（Standby 对象复用）

- ❌ 之前：每次 `show_standby()` 创建全屏 overlay + 全部子对象，`wake_from_standby()` 全部 delete
- ✅ 现在：首次调用创建一次，后续 `lv_obj_clear_flag(HIDDEN)` / `lv_obj_add_flag(HIDDEN)` 切换，仅重启动画

#### 5. Flex 合规审计

| 位置 | 模式 | 判定 |
|------|------|------|
| Dock 背景 | `ROW` | ✅ Toolbar |
| 天气/AI 卡片 | `COLUMN` | ✅ 列布局 |
| Settings | `COLUMN` | ✅ Settings Rows |
| Files 文件列表 | `COLUMN` | ✅ Lists |
| Games/Books/AI 子页网格 | `ROW_WRAP` | ✅ 非 dashboard 子页内容 |
| Standby 健康卡片 | `ROW` | ✅ 行布局 |

#### 6. 深度 & 父级规则

- 最大深度：`Screen → Dock → dock_bg → btn → icon` = **4**（≤ 4 ✅）
- 零浮动对象：`lv_label_create(scr)` = 0 ✅，`lv_btn_create(scr)` = 0 ✅

### 变更文件

| 文件 | 说明 |
|------|------|
| `/data/lv_port_linux/src/main.c` | 全部重构改动位于此文件 |

### 涉及函数

`create_deskmate_screen`（重写）, `create_status_bar`, `create_card`, `create_weather_card`, `create_ai_card`, `create_app_dock`, `show_standby`, `wake_from_standby`, `idle_timer_cb`, `show_subpage`, `create_settings_subpage`, `create_games_subpage`, `create_books_subpage`, `create_files_subpage`, `create_ai_subpage`, `init_shared_styles`（新增）

### 编译验证

- ✅ `make -j$(nproc)` → `[100%] Built target lvglsim`
- ✅ 模拟器运行正常（PID 可见）

---

## 一、UI 改动总览（v2.0 → v3.0 大改）

### 配色方案

| 方面 | 旧版 (v2.0) | 新版 (v3.0) |
|------|-------------|-------------|
| 主背景 | `#0f172a` 深蓝灰 | `#F2F2F7` 极浅灰（Apple 风格） |
| 状态栏 | `#1e293b` 深色 80% | `rgba(255,255,255,0.72)` 半透明白 |
| 主文字 | `#FFFFFF` 纯白 | `#1C1C1E` 近黑 |
| 正文 | `#E2E8F0` 浅灰 | `#3A3A3C` 深灰 |
| 辅助文字 | `#94a3b8` 中灰 | `#8E8E93` 中灰 |
| 强调色 | `#38bdf8` 天蓝 | `#007AFF` Apple 蓝 |
| 状态栏图标 | `#4ade80` / `#60a5fa` | `#34C759` / `#007AFF` |

### 锁屏布局（最终版 v3.1）

```
┌──────────────────────────────────────────────────┐
│  ┌────────────── 560×170 ──────────────┐         │ ← 毛玻璃卡片 24px 圆角
│  │           12:30                      │ ← 48px  │   半透明白 bg_opa=120
│  │        Thu  July 23                  │ ← 48px  │   阴影 width=20, opa=10%
│  └──────────────────────────────────────┘         │   y=264
│                                                    │
│          ~116px 留白                                │
│                                                    │
│               ◉ 呼吸光环 160×160  ← y=520          │ lv_arc + 透明度呼吸动画
│                                                    │
│       You have been sitting...  ← 48px, y=720      │ AI 语录
│                                                    │
│  ┌────────────── 720×80 ──────────────────┐         │ ← 毛玻璃卡片 24px 圆角
│  │  🛎 Sit 45min    ⚡ Water 3/8    👁 Rest --  │ ← 30px  │   同款毛玻璃风格
│  └────────────────────────────────────────┘         │   y=1100
│                                                    │
└──────────────────────────────────────────────────┘
```

### 设计语言升级

- **两张毛玻璃卡片遥相呼应**：时钟卡片（560×170）和健康卡片（720×80），统一 24px 圆角、半透明白 bg_opa=120、大半径柔和阴影
- **三区布局**：时钟区 → 光环区 → 健康卡区，中间大量留白自然隔开
- **所有文字英文**：Montserrat 字体不支持中文，杜绝"口口"
- **日期格式**：`Thu  July 23`（去掉年份，更自然）
- **健康卡片**：720×80px，30px 字体，三栏 flex 均匀分布

### 应用栏

| 维度 | 旧版 | 新版 |
|------|------|------|
| 位置 | 主内容区居中 | 底部 dock |
| 图标尺寸 | 80px | 64px |
| 间距 | 24px | 32px |
| 角色 | 主要操作入口 | 从属地位，不喧宾夺主 |

### 新增锁屏/待机

- 30 秒无操作自动进入待机（`IDLE_TIMEOUT = 30`）
- **浅色背景** `#F2F2F7`（不再是深色），与桌面风格统一
- 大时钟 48px + 日期 48px，包在毛玻璃卡片内
- 呼吸光环（lv_arc + 透明度动画）作为 AI 状态指示
- AI 语录 48px 显示健康提醒
- 底部健康状态卡片（久坐/喝水/护眼）毛玻璃风格
- 点击/按键唤醒

### 已知限制

- 时钟最大 48px（FreeType 因 CMake/libevdev 依赖问题暂不可用，PC 模拟器构建中断）
- 呼吸光环为简化版单色 lv_arc（HTML 原版是多色 Canvas 粒子环）

### 焦点高亮

- 缩放效果：`lv_obj_set_style_transform_scale(focused, 256, 0)`
- 蓝色阴影：`#007AFF`, width=16, opa=50%, spread=2

---

## 二、文件清单

| 文件 | 行数 | 说明 |
|------|------|------|
| `/data/lv_port_linux/src/main.c` | ~772 行 | PC 模拟器主代码（v3.1 锁屏毛玻璃卡片 + 布局重排） |
| `/data/openvela/apps/examples/deskmate/deskmate.c` | ~965 行 | NuttX 目标代码（同步 v3.1 + 按键轮询 + overlay 方案） |
| `/data/vela/novnc-web/index.html` | 434 行 | Web UI 前端（自研定制版，深色主题 + 缩放 + 自动重连） |
| `/data/vela/start-deskmate-web.sh` | 70 行 | 一键启动脚本（Xvfb → x11vnc → websockify） |
| `/data/vela/vela-dev.sh` | 118 行 | 开发模式脚本（监听文件变化自动编译+重启） |
| `/data/vela/devlog.md` | 本文件 | 开发日志 |
| `/data/fonts/Montserrat-*.ttf` | 3 个 | 已下载的 FreeType 字体（Light/Regular/Medium，待启用） |

---

## 三、注意事项（踩坑记录）

### 3.1 锁屏待机不要用 `lv_scr_load()` 切换屏幕

**背景：** 最初尝试用 `lv_obj_create(NULL)` 创建独立屏幕，然后用 `lv_scr_load()` 切换。但 LVGL 的独立屏幕（`lv_obj_create(NULL)`）没有默认主题样式，在新屏幕上创建的子对象 `style_cnt == 0`，设置样式时触发断言崩溃。

**正确做法：** 在主页上创建全屏覆盖层容器：

```c
// ✅ 正确：建在 active screen 上
standby_overlay = lv_obj_create(lv_scr_act());
lv_obj_set_size(standby_overlay, sw, sh);
lv_obj_set_pos(standby_overlay, 0, 0);
// 所有子对象建在 overlay 上，继承主页的样式

// ❌ 错误：建独立屏幕然后切换
standby_scr = lv_obj_create(NULL);
lv_scr_load(standby_scr);  // 主题样式未生效，子对象没有 style
```

### 3.2 动画回调不要类型强转

**背景：** `lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_style_arc_opa)` 这个强转是合法的 C 语法，但 `lv_obj_set_style_arc_opa` 有 3 个参数 `(lv_obj_t *, lv_opa_t, int32_t)`，而动画回调 `lv_anim_exec_xcb_t` 只传 2 个 `(void *, int32_t)`，栈被破坏，崩在 `get_local_style`。

**正确做法：** 用包装函数：

```c
// 包装函数
static void arc_opa_anim_cb(void *obj, int32_t v)
{
    lv_obj_set_style_arc_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

// 使用
lv_anim_set_exec_cb(&a, arc_opa_anim_cb);  // ✅ 正确
// lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_style_arc_opa);  // ❌ 崩溃
```

### 3.3 FreeType 暂时不可用（PC 模拟器 + NuttX 双端）

PC 模拟器：系统安装 FreeType 2.11 开发库，但 CMake 构建配置因 `libevdev` 依赖问题（无 root 权限安装 `libevdev-dev`）无法完成配置，导致 FreeType 无法启用。

NuttX 目标：系统 FreeType 版本（24.1.18）与 LVGL 期望的内部头文件不兼容：
```
fatal error: freetype/internal/ftdebug.h: No such file or directory
```

当前用内置 Montserrat 位图字体（48px/30px/16px）替代。后续需要修复构建环境才能启用矢量字体。

### 3.4 模拟器没有输入设备

`indev_read_cb is not registered` 警告是正常的，不影响画面显示。后续如需触控交互再添加。

### 3.5 应用图标点击只是日志

点击应用图标只打印 `Launch app: xxx`，没有实际启动子页面。TODO。

### 3.6 PC 模拟器额外内置字体

PC 模拟器（`lv_conf_expanded.h`）比 NuttX 目标多出大量内置 Montserrat 字体：
- 可用：12/14/16/18/20/22/24/26/28/30/32/34/36/38/40/42/44/46/48px
- NuttX 目标仅 8/16/48px
- 所以 `&lv_font_montserrat_30` 只在 PC 模拟器可用，NuttX 上需要回退到 16px 或 48px

### 3.7 CMakeCache.txt 不可删除

删除 `build/CMakeCache.txt` 会导致全量 CMake 重新配置，libevdev 依赖检查失败（无 root 权限安装 `libevdev-dev`），构建永久中断。如需重建，需先恢复 CMakeCache 或修复 libevdev 依赖。

---

## 四、开发工作流

### 标准流程（推荐）

```
编辑 main.c → 保存
      ↓
 [自动] make 编译新 lvglsim
      ↓
 [自动] 重启 lvglsim 进程
      ↓
 [自动] 浏览器 VNC 重连 → 看到新界面
```

**启动开发模式：**

```bash
/data/vela/vela-dev.sh
```

然后编辑 `/data/lv_port_linux/src/main.c`，保存即自动刷新。

### 手动流程

如果开发模式不可用（缺少 `inotifywait`）：

```bash
# 1. 编辑代码后编译
cd /data/lv_port_linux/build && make -j$(nproc)

# 2. 重启 lvglsim（保留 Xvfb/x11vnc/websockify 运行）
kill $(pgrep -x lvglsim) 2>/dev/null
export DISPLAY=:99
/data/lv_port_linux/build/bin/lvglsim -b SDL -W 1920 -H 1200 &

# 3. 浏览器刷新或等自动重连
```

### 完整编译 + 打包（目标板）

```bash
cd /data/vela
./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh_dm/ -j12
```

注意：`build.sh` 当前是损坏的符号链接，需要修复后才能编译目标板。

### NuttX 目标同步

修改 `main.c`（PC 模拟器）→ 验证通过 → 复制到 `deskmate.c`（NuttX 目标）：

```bash
# 手动同步 UI 改动（deskmate.c 多按键轮询和 NuttX 初始化）
# 编辑 /data/openvela/apps/examples/deskmate/deskmate.c
```

---

## 五、待办

- [ ] FreeType 矢量字体（修复 PC 模拟器 CMake/libevdev 依赖 + NuttX 内部头文件问题）
- [ ] 天气卡片对接真实数据源（当前占位符）
- [ ] 音乐卡片对接真实播放器
- [ ] 应用图标点击启动子页面
- [ ] 健康提醒逻辑（久坐/喝水/护眼计时器）
- [ ] 修复 `build.sh` 符号链接，恢复 NuttX 编译
- [ ] 锁屏健康卡片数据实时更新
- [ ] 锁屏呼吸光环升级为多色效果（当前为单色 lv_arc 简化版）