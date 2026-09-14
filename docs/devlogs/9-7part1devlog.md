# 9-7 Part 1 — 模拟器移植尝试（P188，✅ 已闭环）

> 日期：2026-09-07
> 目标：把板端最新 UI 源码复制到 PC 模拟器（`/data/lv_port_linux/`），免刷机预览
> 状态：✅ 已闭环（lvglsim 启动成功，VNC 链路通）

## 一、背景

用户不想刷机了，要求复用旧模拟器环境（`/data/lv_port_linux/`，8 月搭建的 LVGL SDL 模拟器）。
模拟器架构：`Xvfb(:99) → lvglsim(SDL) → x11vnc(:5900) → websockify(:9009) → 浏览器`
访问地址：`http://192.168.*.*:8080/vnc.html?host=192.168.*.*&port=9009&autoconnect=1&resize=scale`

## 二、已完成的工作

### 2.1 源码复制（板端 → 模拟器）

从 `/data/dm/vendor/allwinnertech/apps/luncher_dm/` 复制到 `/data/lv_port_linux/src/`：

| 类别 | 文件 |
|------|------|
| UI 核心 | `deskmate_ui.c`, `deskmate_ui.h`, `deskmate_icons.c/h` |
| UI 子页 | `ui/ui_home.c`, `ui_wifi.c`, `ui_bt.c`, `ui_files.c`, `ui_books.c`, `ui_settings.c`, `ui_music.c`, `ui_uart_dbg.c` |
| 头文件 | `dm_net.h`, `dm_ai.h`, `dm_weather.h/c`, `dm_voice.h`, `dm_health.h`, `dm_health_cfg.h/c`, `dm_pet.h`, `dm_uart_dbg.h`, `dm_als.h`, `dm_ld2410b.h`, `pet_sprites.h`, `pet_cat_sprites.h`, `pet_frame_table.c`, `aibg.h/c` |
| 精灵 | `pet_cat_*.c` ×44 + `pet_dog_*.c` ×44（128×128 像素画数据） |

**⚠️ 铁律：只改 `/data/lv_port_linux/`，不碰 SDK `/data/dm/`。**

### 2.2 Stub 层创建

模拟器没有硬件，为每个服务模块创建 stub（返回安全默认值）：

| Stub 文件 | 对应模块 | 策略 |
|-----------|---------|------|
| `dm_net_sim.c` | WiFi/BT（dm_net.h） | 全返回 0/-1/空，扫描/连接不可用 |
| `dm_ai_sim.c` | AI Agent（dm_ai.h） | 全返回 -1/0，语音不可用 |
| `dm_voice_sim.c` | Voice Director（dm_voice.h） | 决策全返回 VOICE_NONE |
| `dm_health_sim.c` | 健康助理（dm_health.h） | 返回默认值（未在座，0 次喝水/起立） |
| `dm_pet_sim.c` | 宠物引擎（dm_pet.h） | 创建返回成功，事件空处理 |
| `dm_uart_sim.c` | UART 调试（dm_uart_dbg.h） | 全返回 -1/0，串口不可用 |
| `dm_sensors_sim.c` | ALS + LD2410B | 全返回 -1，传感器不可用 |

### 2.3 deskmate_main.c 更新

- 新增 `dm_font_clock`（56px 状态栏时间专用档，P151 新增）
- combo_fonts 数组 5→6 个
- 模拟器字体：Montserrat + fa-solid/fa-brands combo（无 MiSans）

### 2.4 CMakeLists.txt 更新

`add_executable(lvglsim ...)` 加入所有新源文件：
- `SIM_CORE_SRC`：核心 + stubs + aibg.c
- `SIM_UI_SRC`：`src/ui/*.c`（glob）
- `SIM_PET_SRC`：`src/pet_cat_*.c` + `src/pet_dog_*.c` + `src/pet_frame_table.c`
- `target_include_directories(lvglsim PRIVATE ${CMAKE_SOURCE_DIR}/src)`

### 2.5 编译修复（全部通过）

| 错误 | 修复 |
|------|------|
| `dm_als.h` 缺 `bool` | `dm_sensors_sim.c` 加 `#include <stdbool.h>` |
| `dm_uart_dbg.h` 缺 `uint8_t` | `dm_uart_sim.c` 加 `#include <stdint.h>` |
| `dm_health_cfg_set_*` 返回类型冲突 | `int` 而非 `void`（对齐头文件） |
| `aibg.h` 缺失 | 从板端复制 `aibg.h/c` |
| `lv_demo_panel_rgb_control.h` | 已有 `#ifdef CONFIG_LED_RGB_WS2812` 守卫，不需处理 |
| `dm_net.h`/`dm_pet.h` 在 `#ifdef` 内 | 移到 `#ifdef CONFIG_LUNCHER_DM_APP` 守卫外 |
| `g_audio_tone_done` 未声明 | 在 `#else` 分支补声明 |
| `dm_tone_play`/`dm_tone_play_seq` 未定义 | 在 `#else` 分支加空 stub |
| `assert` 链接失败 | `ui_music.c`/`ui_uart_dbg.c` 补 `#include <assert.h>` |
| `dm_net_status_refresh` 重复定义 | 从 `dm_net_sim.c` 删除（`deskmate_ui.c` 已定义） |
| `dm_health_cfg_*` 重复定义 | 从 `dm_health_sim.c` 删除（`dm_health_cfg.c` 已定义） |
| `ui_music.c` `sw`/`sh` 作用域 | `ui_music_create()` 开头加局部变量声明 |

## 三、已闭环 — 修复总结

### 3.1 lvglsim 启动 crash（SEGV）— 已修复

**根因**：
- `LV_MEM_SIZE` 实际生效值仍为 64KB（CMake 生成的 `build/lv_conf.h` 被 `generate_lv_conf.py` 覆盖为默认值，且 lvgl 9.1 无此脚本导致保留旧值）
- `music_play_btn_create()` 内 `lv_btn_create()` 返回 null → `lv_label_create(null)` SEGV

**修复动作**（仅在 `/data/lv_port_linux/`，未碰 SDK）：
1. `lv_conf.h` 根目录设 `LV_MEM_SIZE (128 * 1024 * 1024U)`（128MB）
2. CMakeLists.txt 第 91 行 `set(LV_CONF_PATH "${CMAKE_SOURCE_DIR}/lv_conf.h")` 强制 lvgl 库与应用同用此配置
3. `music_play_btn_create()` 已有 null 防御（parent/btn 均判空）

**验证**：
- 重新 cmake + make 后，`build/lv_conf.h` 第 59 行确认为 128MB
- 启动 lvglsim 无 crash，日志：`DBG: LVGL mem total=134211304 used=61536 free=134142600 frag=0%`
- `DISPLAY=:99` 下 `xwininfo` 可见 "LVGL Simulator" 窗口（1920×1200）
- VNC 链路：Xvfb:99 → x11vnc:5900 → websockify:9009 已跑通

### 3.2 模拟器服务状态（更新）

用户服务器 `192.168.*.*` 上：
- ✅ Xvfb :99 运行中
- ✅ x11vnc :5900 运行中
- ✅ websockify :9009 运行中
- ✅ lvglsim — **在 DISPLAY=:99 稳定运行，可通过浏览器 VNC 预览**

### 3.3 文件清单（模拟器已改动）

| 文件 | 改动 |
|------|------|
| `src/deskmate_main.c` | 新写：+dm_font_clock, combo_fonts[6] |
| `src/deskmate_ui.c` | 板端副本 + 3 处修改（dm_net/dm_pet include 移出 ifdef、#else 补 g_audio_tone_done/dm_tone_play stub） |
| `src/ui/ui_music.c` | 板端副本 + 2 处修改（assert include、sw/sh 作用域、null 防御） |
| `src/ui/ui_uart_dbg.c` | 板端副本 + assert include |
| `src/dm_net_sim.c` | 新写 |
| `src/dm_ai_sim.c` | 新写 |
| `src/dm_voice_sim.c` | 新写 |
| `src/dm_health_sim.c` | 新写 |
| `src/dm_pet_sim.c` | 新写 |
| `src/dm_uart_sim.c` | 新写 |
| `src/dm_sensors_sim.c` | 新写 |
| `src/stub_nuttx.h` | 新写（uORB/PWM stub，未使用） |
| `CMakeLists.txt` | 加入所有新源文件 + include 路径 |
| `lv_conf.h` | LV_MEM_SIZE 64KB → 4MB |

### 3.4 模拟器服务状态

用户服务器 `192.168.*.*` 上：
- ✅ Xvfb :99 运行中
- ✅ x11vnc :5900 运行中
- ✅ websockify :9009 运行中
- ❌ lvglsim — crash 待修复

---

*DevLog by AtomCode (mimo-v2.5-pro)*
