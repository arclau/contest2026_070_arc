# DevLog 2026-08-05 (P3) — 上板开机启动切换 + Data Abort 死机根因定位与修复

> 工程：openvela (R528) — nsh 配置
> 工作目录：`/data/dm`
> 工具链：SDK 自带 prebuilts/gcc/linux-x86_64/arm-none-eabi (GCC 13.4.0)
> 打包流程：`lichee` 下 `source envsetup.sh && lunch_nuttx 2 && pack`，镜像输出至 `lichee/out/r528s3/gemini-s1_nand/`
> **AI 交接：新会话请先读 `AGENTS.md`，本文件是当日归档，仅按需查阅。**

---

## 一、任务背景

模拟器上调整好的 Desktop Mate UI 同步到板端（luncher_dm）后准备上板验证，暴露两个问题：

1. **开机仍是旧 luncher**：`rcS.nsh` 只有 `CONFIG_LUNCHER_APP` 的启动项，`CONFIG_LUNCHER_DM_APP` 虽已编译但无开机启动；
2. **切到 luncher_dm 后 Data Abort 死机**：create_app_dock 完成后立即崩溃（PC: 415e5104, DFAR: eb00002a）。

## 二、开机启动切换（rcS.nsh + distclean 全量重编）

### 现象 → 根因

| 项 | 内容 |
|----|------|
| 现象 | 刷机后开机仍是旧 luncher 界面 |
| 根因 1 | `src/etc/init.d/rcS.nsh` 只有 `#ifdef CONFIG_LUNCHER_APP → luncher &`，无 luncher_dm 启动项 |
| 根因 2 | **rcS.nsh 是 prebuilt 脚本文件，增量编译不重新处理进固件**——改完源码后编译打包两次，固件内仍是旧内容（`strings vela.bin` 无 `luncher_dm &`） |

### 方案

`rcS.nsh` 增加 luncher_dm 优先启动、旧 luncher 回退：

```c
#ifdef CONFIG_LUNCHER_DM_APP
luncher_dm &
#elif defined(CONFIG_LUNCHER_APP)
luncher &
#endif
```

随后 **distclean 后全量重编**（`./build.sh <config> distclean` → `./build.sh <config> -j$(nproc)`）。

### 验证结果

- 固件内直接验证：`strings -a vela.bin | grep luncher_dm` → 出现 `luncher_dm &`（rcS.nsh 新启动行），无 `luncher &` 残留
- 产物：nsh.fex == vela.bin（md5 934cec9c，5311936B）

## 三、上板 Data Abort 死机排查（静态定位）

### 现象

开机进 luncher_dm 后约 3 秒死机（约在 `create_app_dock: Dock: 7 apps` 日志之后）：

```
[LVGL] create_app_dock: Dock: 7 apps deskmate_ui.c:2249
Data abort. PC: 415e5104 DFAR: eb00002a DFSR: 00000005
Assertion failed panic: task(CPU0): luncher_dm
backtrace|78: 0x4158136c 0x415e5102 0x415e61ea 0x4160b4ec 0x416503ea 0x4165404e 0x415da026 ...
```

### 定位过程

1. **addr2line 解析 backtrace**（用 `nuttx.elf`，nuttx 已被 strip）：
   ```
   luncher_dm_main (luncher_dm.c:1347)
     → create_deskmate_screen (deskmate_ui.c:2567)   ← clock_update_cb(NULL)
       → clock_update_cb (deskmate_ui.c:230)          ← lv_label_set_text(clock_label)
         → lv_label_set_text (lv_label.c:99)
           → lv_obj_invalidate (lv_obj_pos.c:840)
             → _lv_obj_get_ext_draw_size (lv_obj_draw.c:384)  ← Data Abort
   ```
2. **反汇编确认崩溃指令**（`arm-none-eabi-objdump -d nuttx.elf`）：
   ```
   415e50f8: ldr r0, [r0, #8]   ; obj->spec_attr（obj 有效，R0=0x4192ad40）
   415e5104: ldr r0, [r0, #44]  ; ← 崩溃！spec_attr 指针 = 垃圾 0xEB00002A
   ```
   结论：clock_label 对象内存可读，但其 `spec_attr` 字段被写成了垃圾值 0xEB00002A（非 NULL，cbz 跳过），访问 `spec_attr->ext_draw_size` 时翻译失败。

### 中间判断（排除项）

- 图标数据/header 正确：`deskmate_icons.c` 7 图标各 94864B（=154×154×4 ✓），dsc 用符号常量位置初始化，两端各自编译正确；
- 字体有 NULL 回退逻辑，不是直接崩溃源；
- 镜像内无 0xEB00002A 静态字节（`2a 00 00 eb` 搜索 0 处）→ 运行期产生的垃圾值；
- 板端 LVGL 9.1.0 vs 模拟器 9.6.0 版本差异巨大（AGENTS.md 已知 9.1 有堆损坏 bug 先例）。

## 四、模拟器 LVGL 降级到与 SDK 同版本（关键决策）

### 决策

**SDK 的 LVGL 版本绝不更新**（openvela 定制 trunk-5.5-3，含 ArtInChip GE2D GPU/MPP 适配，升级代价大）。改为**模拟器降级到与板端一模一样**，在模拟器复现/修复，再同步回 SDK。

### 降级步骤

| 步骤 | 操作 |
|------|------|
| 备份原模拟器 lvgl | `mv lvgl lvgl.9.6.bak`（原为 v9.5.0-354≈9.6.0，git 仓库） |
| 复制板端源码 | `rsync -a --exclude={.git,docs,examples,demos,tests} /data/dm/apps/graphics/lvgl/lvgl/ lvgl/`（openvela 9.1.0 定制版） |
| 隔离旧构建产物 | `mv build build.9.6.bak`（关键：旧 .o 是 9.6 编译的，链接仍用旧库 → 符号 `_lv_log_add` 缺失，必须全量重建） |

### 模拟器构建适配（文件 | 改动）

| 文件 | 改动 |
|------|------|
| `CMakeLists.txt` | ① `generate_lv_conf.py` 不存在时跳过生成（9.1 无此脚本）；② 禁用 examples/demos/thorvg（目录已排除，空源 add_library 报错）；③ `set(CONFIG_LV_USE_FREETYPE ON)`/`CONFIG_LV_USE_SDL ON`/`CONFIG_LV_USE_EVDEV OFF`（evdev.c 是 9.6 代码用 `lv_evdev_type_t` 不兼容）；④ 链接去 `lvgl_demos`；⑤ `-DASAN=ON` 开关（sanitizer 复现） |
| `lvgl/lv_conf.h` | 基于 9.1 `lv_conf_template.h` 复制生成（**必须把文件头 `#if 0` 改 `#if 1`，否则整个配置被禁用**）；开 LV_COLOR_DEPTH 32、LV_USE_SDL、LV_USE_LOG、LV_USE_FREETYPE、LV_FONT_MONTSERRAT_12/14/16/24/30/36/48、LV_USE_LODEPNG；**定义 `CONFIG_LV_COLOR_DEPTH 32`**（openvela lv_conf_internal.h 在无 CONFIG_LV_COLOR_DEPTH 时强制 LV_COLOR_DEPTH=16） |
| 项目根 `lv_conf.h` | 与 lvgl/lv_conf.h 一致（lvgl_linux/driver_backends.c 目标用的 include 路径不同，两端必须统一） |
| `lvgl/src/draw/artinchip` | 移出 lvgl 树（`mv` 到 `artinchip.off`），openvela 硬件定制代码需要 aic_core.h/mpp_ge.h 芯片 SDK 头，PC 无法编译 |

### 关键坑（本次新增）

1. **lv_conf_template.h 默认 `#if 0` 禁用全部配置**——复制模板必须改 `#if 1`，否则 LV_CONF_H 未定义、所有选项走默认值（预编译 `LV_USE_LOG=0` 但文件里写 1，症状"pragma message Possible failure"）；
2. **openvela lv_conf_internal.h 用 Kconfig（`CONFIG_LV_*`）作配置源**：`CONFIG_LV_COLOR_DEPTH` 未定义时**无条件强制 16**（无 `#ifndef` 保护），其余选项（LV_USE_LOG 等）有 `#ifndef` 保护可被 lv_conf.h 覆盖；
3. **旧 build 目录的 .o 不随源码替换失效**：rsync 的 9.1 源码时间戳比 9.6 的 .o 旧，增量 make 不重编，链接仍是 9.6 库 → 必须隔离重建；
4. **`#include "lv_conf.h"` 的查找**：`lv_conf_internal.h` 在 `lvgl/src/` 下，编译器先查**当前目录**再查 include 路径——lvgl 目标（-isystem lvgl/）与 lvglsim 目标（项目根在 include 路径）用的 lv_conf.h 可能不同，务必两端统一。

## 五、ASAN 定位根因（复现 + 精确报告）

### 复现

模拟器降级后**运行不崩**（堆布局与板端不同，越界写未命中关键对象）——改用 **ASAN**（`cmake -DASAN=ON` 全量重编，隔离 build 目录）强制暴露内存错误：

```
ERROR: AddressSanitizer: global-buffer-overflow
WRITE of size 4 at ... (lv_canvas_fill_bg)
  #0 lv_canvas_fill_bg
  #1 draw_weather_glyph
  #2 create_weather_card
  #3 create_deskmate_screen
global variable 'w_icon_buf' (deskmate_ui.c:145) of size 49152
```

### 根因

**`w_icon_buf` 缓冲区越界（deskmate_ui.c）**：

- `lv_color_t` 在 LVGL 9.1（32 位）实测 `sizeof = 3`（XRGB8888，非 4 字节）——`gcc` 实测输出：`LV_COLOR_DEPTH=32 sizeof(lv_color_t)=3 DM_WEATHER_DAYS=4 sizeof(w_icon_buf)=49152`；
- `w_icon_buf[DM_WEATHER_DAYS][64*64]` 每个 canvas 只有 64×64×3 = **12288 字节**；
- canvas 用 `LV_COLOR_FORMAT_ARGB8888`（4 字节/像素），`lv_canvas_set_buffer` 需要 64×64×4 = **16384 字节**；
- `lv_canvas_fill_bg` 对每个 canvas **越界写 4096 字节，4 个 canvas 共写穿 16KB**，破坏相邻全局变量内存（ASAN 报 w_icon_buf 后 32B 即 w_temp_lbl）；
- 板端时序吻合：create_deskmate_screen 创建天气卡 → 越界写 → 紧接着 `clock_update_cb(NULL)` 访问被破坏内存（spec_attr=0xEB00002A）→ Data Abort。

## 六、修复与验证

### 修复（文件 | 改动）

| 文件 | 改动 |
|------|------|
| `apps/luncher_dm/deskmate_ui.c`（模拟器 `src/deskmate_ui.c` 同改） | `static lv_color_t w_icon_buf[DM_WEATHER_DAYS][64*64]` → `static uint32_t w_icon_buf[DM_WEATHER_DAYS][64*64]`（4 字节/像素匹配 ARGB8888，加注释说明 lv_color_t 3 字节之坑） |

### 验证（模拟器）

- ASAN 复验：`ERROR: AddressSanitizer` 计数 = **0**，模拟器正常运行 30s（timeout 杀掉而非崩溃）；
- 同步：`bash /data/vela/sync_deskmate.sh`（模拟器 → 板端），两侧 `diff` 一致。

### 验证（板端）

- 编译：`./build.sh r528s3-gemini-s1/configs/nsh -j$(nproc)` ✅
- 打包：`source envsetup.sh && lunch_nuttx 2 && pack` → `pack finish` ✅
- 产物：nsh.fex == vela.bin（md5 824a0f63，5311936B）
- 镜像：`lichee/out/r528s3/gemini-s1_nand/rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img`（2026-08-05 09:02）

## 七、遗留事项 / 下一步

1. **上板验证（重点）**：重新烧录后确认（a）开机进 luncher_dm；（b）不再死机，天气卡 canvas 正常渲染；（c）其余 8-5 遗留项（WiFi 真实天气、Settings 亮度滑块真实 LED、Auto-Lock 实测、dock PNG 渲染）
2. **模拟器环境备注**：lvgl 已降级为板端同版本（原 9.6 备份 `lvgl.9.6.bak`、旧构建 `build.9.6.bak`/`build.noasan`）；ASAN 构建用 `cmake -DASAN=ON`；后续改 UI 仍走 `sync_deskmate.sh` 双向同步
3. **同步脚本改进（延续 8-5）**：`sync_deskmate.sh` 构建失败检测仍只认二进制存在，建议加时间戳/日志校验
4. **`lv_color_t` 3 字节之坑**：LVGL 9.1 下凡手动分配 canvas/图像 buffer，必须按 cf 字节数（ARGB8888=4B/px）计算，勿用 `sizeof(lv_color_t)`——建议写入 AGENTS.md 已知坑

---

*DevLog by AtomCode (deepseek-v4-flash)*
