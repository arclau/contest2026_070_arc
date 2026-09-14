# DevLog 2026-08-04 — luncher 独立注册 + 1920x1200 横屏适配 + 触摸测试子 APP

> 工程：openvela (R528) — nsh 配置
> 工作目录：`/data/dm`
> 工具链：SDK 自带 `prebuilts/gcc/linux-x86_64/arm-none-eabi` (GCC 13.4.0)
> 打包流程：`lichee` 下 `source envsetup.sh && lunch_nuttx 2 && pack`，镜像输出至 `lichee/out/r528s3/gemini-s1_nand/`
> **AI 交接：新会话请先读 `AGENTS.md`（环境/命令/进度/已知坑），本文件是当日归档，仅按需查阅。**

---

## 一、任务背景

昨日完成 BOE 1200x1920 屏 + GT9271 触摸驱动移植。今日目标：
1. 开机 APP 由 LVGLDEMO 切换为 luncher（从 luncher_mini 复制而来，针对 1920x1200 BOE 横屏放大适配）
2. 新增 GT9271 触摸测试，并作为 luncher 的子 APP（点击 Touch 图标进入）
3. 解决横屏旋转、触摸映射、UI 元素边界三大上板问题

---

## 二、luncher 独立注册（由 luncher_mini 改名）

复制 `apps/luncher_mini` → `apps/luncher`，全部配置名从 `LUNCHER_MINI_APP` 改为 `LUNCHER_APP`：

| 文件 | 改动 |
|------|------|
| `apps/luncher/Kconfig` | `LUNCHER_APP`，新增 `LUNCHER_APP_INPUT_DEVPATH`（默认 `/dev/input0`） |
| `apps/luncher/Make.defs` / `Makefile` | 配置名与路径改为 luncher |
| `apps/luncher/luncher_mini.c` | 条件编译宏 `CONFIG_LUNCHER_APP`；入口函数 `luncher_mini_main` → `luncher_main`（否则链接 undefined reference） |
| `apps/Kconfig` | 注册 `source .../luncher/Kconfig`（该文件被 .gitignore 忽略，改动仍生效） |
| `configs/nsh/defconfig` | `CONFIG_EXAMPLES_LVGLDEMO=y` → `CONFIG_LUNCHER_APP=y`；补 `CONFIG_LV_FONT_MONTSERRAT_12=y`（fallback 字体依赖） |
| `src/etc/init.d/rcS.nsh` | `luncher_mini &` → `luncher &` |

### 编译期问题（3 个）

1. `lv_font_montserrat_12` 未声明：nsh defconfig 只启用了 MONTSERRAT_16/48，补 `CONFIG_LV_FONT_MONTSERRAT_12=y`
2. `CONFIG_EXAMPLES_LVGLDEMO_INPUT_DEVPATH` 消失：删 LVGLDEMO 后宏不存在，改为 luncher 自有 `CONFIG_LUNCHER_APP_INPUT_DEVPATH`
3. `undefined reference to luncher_main`：NuttX 按 PROGNAME 期望入口 `luncher_main`，重命名源文件入口函数

---

## 三、UI 参数按 1920x1200 缩放（原 320x240 基准）

- `SCREEN_WIDTH/HEIGHT` = 1920/1200
- 4 个主窗口 60x50 → 320x300、起始 y=140 → 700、间距 10 → 60
- 时间区 100 → 500 高、时钟偏移 -48 → 0（修复出左边界）
- 温湿度标签偏移 ±50 → ±40（修复 66px 字体两行重叠）
- 灯光弹窗 240x160 → 1440x800、About 弹窗 260x200 → 1560x1000
- MiSans 字体 11/12/16 → 66/72/96（freetype 按像素加载）

---

## 四、横屏旋转排障（核心问题）

### 4.1 现象

上板后：触摸方向 OK，但 luncher 仍竖屏显示，UI 元素出界/重叠。

### 4.2 尝试一：应用层 LVGL 旋转（失败）

`lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90)` + 启用 `LV_USE_MATRIX`/`LV_DRAW_TRANSFORM_USE_MATRIX`。
- 触摸映射生效（LVGL indev 按 rotation 转换坐标）
- 但像素未旋转 → 竖屏依旧

### 4.3 尝试二：矩阵旋转（死机）

启用矩阵旋转后日志：
```
lv_draw_buf_goto_xy: coordinates out of range, x: 1260, y: 844, w: 1200, h: 1920
Data abort. PC: 416adbf0 ...
```
**根因**：fb 后端是 `LV_DISPLAY_RENDER_MODE_DIRECT`，draw buffer 直接映射物理 1200x1920；旋转后逻辑分辨率 1920x1200，渲染坐标（x=1260）超出物理 buffer → 越界 Data Abort。矩阵旋转与 DIRECT 渲染模式不兼容。

### 4.4 最终方案：驱动层 G2D 旋转（成功）

| 文件 | 改动 |
|------|------|
| `BOE_1200x1920_mipi_config.c` | `degree0` 2(180°) → **3(270°)**：dev_fb.c 对 90/270 度自动交换 fb 宽高 → fb 逻辑分辨率 **1920x1200 横屏**，G2D 硬件旋转显示到 1200x1920 面板 |
| `gt9271_iic_touch.c` | 触摸坐标顺时针 90° 旋转映射：`X = 1920-1-py, Y = px`（与 G2D 旋转方向一致） |

回退：defconfig 移除 `LV_USE_MATRIX`/`LV_DRAW_TRANSFORM_USE_MATRIX`，luncher 删除 `lv_display_set_rotation`。

---

## 五、触摸测试改为 luncher 子 APP（角色重构）

### 5.1 初版问题（角色错位）

最初用 `task_create` 拉起独立 `gt9271_touch_test` 任务，与 luncher **平级**运行——两个任务同时 open `/dev/input0` 抢触摸事件，touch test 启动成功但读不到样本，表现为"点了打不开"。

### 5.2 子 APP 方案（按手机心智模型）

点击 Touch 图标 → **luncher 内部切换全屏触摸测试子页面**：
- 触摸事件由 LVGL indev 直接派发（LVGL 本就持有 /dev/input0，无第二个 reader，无竞争）
- 页面内容：标题 "Touch Test"、红色 Back 返回按钮、居中坐标显示（X/Y 实时更新）、蓝色触点圆点跟随手指
- `close_touch_window_cb` 点 Back 删除页面返回主界面

### 5.3 拖动 BUG 修复

**现象**：触摸测试页拖动时整个画面被移动，蓝点不跟随。
**根因**：`lv_obj_create` 全屏对象默认带 `LV_OBJ_FLAG_SCROLLABLE`，拖动被当作滚动而非派发坐标。
**修复**：`lv_obj_remove_flag(touch_window, LV_OBJ_FLAG_SCROLLABLE)`。

---

## 六、gt9271_touch_test 改名注册（保留为命令行工具）

- `apps/gt9271_touch_test/`：由 t070s140b_touch_test 改名，`EXAMPLES_GT9271_TOUCH_TEST`，`depends on GT9271_IIC_TOUCH`
- 分辨率宏 1024x600 → 1200x1920（GT9271 驱动坐标缩放目标）
- defconfig 启用（`CONFIG_EXAMPLES_GT9271_TOUCH_TEST=y`）
- 说明：仍保留在固件中可作 NSH 命令行工具，但不再从 luncher 启动

---

## 七、编译打包验证

| 项目 | 结果 |
|------|------|
| 全量构建 `./build.sh` | ✅ LD 通过，`nuttx/vela.bin`（~4.6MB）生成 |
| 打包 `lunch_nuttx 2 && pack` | ✅ `rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img`（26.5MB）生成 |
| `nsh.fex` 与 vela.bin | ✅ 字节一致 |
| 固件符号检查（strings） | ✅ luncher / Touch / gt9271_touch_test 均在镜像内 |

---

## 八、遗留事项 / 下一步

1. **背景图**：`/resource/imgs/luncher_mini_bg_new.png` 为 320x240 设计，1920x1200 下被拉伸且 PNG 解码有告警，建议替换为 1920x1200 横屏图
2. **旋转方向**：当前 `degree0=3`（270°）。若横屏方向颠倒，改回 `degree0=1`（90°）并同步调整 `gt9271_iic_touch.c` 的触摸映射公式
3. **触摸测试增强**（可选）：子页面可加网格/多点触控显示、画轨迹线等功能
4. 传感器/灯光窗口内文字随缩放后仍需上板复核可读性

---

*DevLog by AtomCode (deepseek-v4-flash)*
