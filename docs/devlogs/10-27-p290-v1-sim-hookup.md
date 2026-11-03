# P290 — V1 接入 goldfish 模拟器（1920×1200）

> 2026-10-27。目标：把 V1 `luncher_dm` 接到 goldfish，分辨率同步 1920×1200，供 V1 UI 改造做预览与回归。
> 平台：goldfish ARM64。验收：构建成功 + 运行 + 截图 + 点击导航。

## 1. 结果

- goldfish 现在编 **V1 `luncher_dm`**，客户机与窗口均为 1920×1200。
- 主屏正常渲染，底部 7 项 Dock（音乐/日历/书籍/文件/AI对话/设置/锁屏）图标与标签齐全；点击「设置」进入设置子页，触摸坐标 1:1 无缩放。
- 截图：[主屏](../specs/ui/v1-sim-home-1920x1200.png)、[设置](../specs/ui/v1-sim-settings-1920x1200.png)。
- 抓图注意：默认自动锁定 30 秒，闲置后进 standby（全屏不透明遮罩、Dock 隐藏）；早期一版证据图误存成锁屏，已重抓。
- 已知 SIM 差异：goldfish 是 `CONFIG_LV_COLOR_DEPTH=16`（板端 32），近白渐变被 RGB565 量化成可见横条；不是 UI 改动。天气卡显示「加载中…」是 SIM 无网（`dm_net` 走 stub），人体/光感读数 0 是 SIM 无传感器。
- **板端已补编译打包**：`pack_deskmate.sh ynm3000` 通过，`output/ynm3000/ynm3000.img` = `b668eb93…`；硬件未验，见 §5。

## 2. 改动

| 文件 | 改动 |
|---|---|
| `apps/luncher_dm/Kconfig` | 恢复 `LUNCHER_DM_SIM` 选项 |
| `apps/luncher_dm/sim/` | 从 HEAD 恢复 `dm_net_sim.c`、`dm_sim_stubs.c`、`stubs/` |
| `sim/dm_math_compat.h` | 新增：补 `sqrt/sin/cos/floor/atan2/sinf` 原型（SIM 专用） |
| `apps/luncher_dm/sources.txt` | 按当前 Makefile 清单生成 176 项；不含 `dm_net.c`（CMake 条件加） |
| `apps/luncher_dm/CMakeLists.txt` | 从 HEAD 恢复；SIM 时 `-include dm_math_compat.h` |
| `apps/luncher_dm/dm_res.h` | 新增资源根：SIM `/data/data`，板端 `/resource` |
| `luncher_dm.c`、`deskmate_ui.c`、`ui/ui_files.c` | 硬编码 `/resource` 改走 `DM_RES_ROOT` |
| `luncher_dm.c` | 外层 guard `CONFIG_LUNCHER_APP` → `CONFIG_LUNCHER_DM_APP`（原符号属旧 luncher） |
| `goldfish-arm64-v8a-ap/defconfig` | APP / SIM / entrypoint 切 V1 |
| `/data/vela/run-emu.sh` | 1920×1200；Xvfb 2100×1400；stop 时一并杀 Xvfb |
| `dm_voice.h` | `recent_ids` 由 `int[8]` 改 `const char *[8]`（arm64 指针 8 字节，原 memcpy 越界） |

## 3. 构建期修复（arm64 `-Werror`）

arm32 板端不带 `-Werror`，以下问题在板端只是警告或侥幸通过；arm64 CMake 全部拦下：

- 缺原型：`music_play`、`dm_audio_fg_release`、`ui_home_create`、`create_status_bar_ex`、`dm_alarm_tone_*`、`ioctl`、`fsync`。
- NuttX `math.h` 在 `CONFIG_LIBM_TOOLCHAIN` 下不导出原型 → SIM 统一 `-include dm_math_compat.h`。
- `ui_music.c`：`ui_music_create` 内 `sw/sh` 未定义（真错误，补局部变量）。
- 未使用变量、嵌套注释、`%lX` 格式串、`sbuf` 截断告警 → 按最小改动修正。

## 4. 证据

- 构建：`./build.sh .../goldfish-arm64-v8a-ap --cmake -j16` → `build completed successfully`。
- 启动：`/tmp/emu.log` 有 `Setting display: 0 configuration to: 1920x1200, dpi: 320x320`；`xwininfo` 窗口 1920x1200。
- 交互：console 注入 `event mouse 1402 1080`（Dock 设置图标）→ 进入设置子页；坐标未缩放。

## 5. 边界与未做

1. **板端只到编译打包**：`pack_deskmate.sh ynm3000` 已通过（配置/入口/启动/36 资源/整镜像全项校验，`hardware_test: NOT_RUN`），**未上板**。
   arm32 无 `-Werror`，另做了一次 app 全量重编告警审计：`luncher_dm` 仅剩 `luncher_dm.c:262` 的 `%d` vs `int32_t`，已加 `(int)` 转换，重编后该目录零告警。
2. **V2 模拟器不再默认**：切回需改 goldfish defconfig 的 APP/SIM/entrypoint 三项。
3. **CMake 前置**：源码树不能有 `nuttx/include/nuttx/config.h` 与 `nuttx/include/arch` 软链（板端 make 生成物）。跑完板端打包后这两个又会被重新生成，下次 `--cmake` 前需再次移走；`workspace_check` 会报 `config headers: both ... exist` 提示。
4. 资源挂载沿用 P260：`/dev/virtblk1 → /data`，资源在 `/data/data`。

## 6. 下一步

- 上板确认 V1 固件未回归（显示、GT9271 触摸、Dock 导航）。
- 用该模拟器做 V1 样式/页面生命周期改造的逐页回归；验收仍以逐页像素一致为准。
