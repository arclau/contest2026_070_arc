# P295 — V1 LVGL 量测入口（B0）与埋点（B1）

> 2026-10-28。依据 [V1 Home/锁屏 LVGL 渲染调优工作单](../specs/ui/v1-lvgl-render-direction.md)。
> 目标：先确认能区分"绘制 / flush / 状态更新"的量测入口，再做可撤销的低开销埋点；不猜根因、不改视觉。

## B0 只读结论

完整版见工作单 §6，要点：

- 板端实际走 `/dev/fb0` 的 **fbdev** 后端，不是 LCD 的 `LCDDEVIO_PUTAREA`：`lv_nuttx_dsc_init()` 默认 `fb_path="/dev/fb0"`（`lv_nuttx_entry.c:99`），`CONFIG_LV_USE_NUTTX_LCD=n` 使 `luncher_dm.c` 的 `/dev/lcd0` 分支不成立。
- `lv_display_set_draw_buffers()` + `LV_DISPLAY_RENDER_MODE_DIRECT`（`lv_nuttx_fbdev.c:152,154`）：LVGL 直接画进 fb `mmap`，`FB_NUM=1` 无双缓冲，flush 里没有 memcpy/pan。
- flush 只剩一次 `FBIO_UPDATE`（`lv_nuttx_fbdev.c:229`）→ `sunxifb_updatearea`：脏区 dcache 清理 + G2D 旋转 + layer 配置（`dev_fb.c:212`）。所以"传输"层在本板没有独立的软件成本，真正可测的是绘制提交与 flush ioctl。
- `CONFIG_LV_USE_DRAW_G2D=y` 且 G2D 单元无 `wait_for_finish`：`RENDER_READY` 不等于 GPU 完成，端到端只能看 `REFR_START→REFR_READY`。
- 已有入口：LVGL sysmon 浮窗（FPS/CPU/render%/flush%，1 s 平均）、驱动 `[fb] upd avg`（每 300 帧 clean/g2d 平均 us）。都只有均值，无 p50/p95、无脏区像素、无堆。

## B1 改动

新增 `dm_perf.c/.h`（V1 app，`sources.txt` 与 `Makefile` 双清单已加），在默认 display 上挂 `LV_EVENT_ALL`，复用 LVGL 事件计时：

| 指标 | 事件对 | 单位 |
|---|---|---|
| 一轮刷新总耗时 | `REFR_START`→`REFR_READY` | us（p50/p95/max） |
| 绘制提交 | `RENDER_START`→`RENDER_READY` | us（p50/p95） |
| flush ioctl | `FLUSH_START`→`FLUSH_FINISH` | us（p50/p95/max） |
| 脏区像素 | `FLUSH_*` 的 `lv_area_t` 参数 | px |
| flush 次数 / 轮数 / 间隔 | 同上 | 次 / ms |
| LVGL 堆 | `lv_mem_monitor()` | byte（used/peak/total） |

- 窗口 60 s 输出一行 `[perf] ...`，与驱动 `[fb] upd avg` 同风格；不逐帧打印。
- 关闭开关：`dm_perf.h` 的 `DM_PERF_ENABLE` 改 0，重新编译即回到无埋点基线（用于量化埋点自身开销）。
- 计时 `clock_gettime(CLOCK_MONOTONIC)`；`USEC_PER_TICK=1000` 表示实际量化约 1 ms，亚毫秒样本读 0，报告按 us 注明量化。
- 未改 LVGL、驱动、页面构建与视觉；`luncher_dm.c` 只加一行 `dm_perf_init()`。

## 验证

| 层级 | 结果 |
|---|---|
| 静态 | 新增文件通过 arm64 CMake `-Werror` 编译；`nm` 见 `dm_perf_init`、`perf_event_cb` 已进 `vela_ap.elf` |
| SIM 编译 | goldfish V1 `./build.sh ... --cmake -j16` 成功（CMake 前置：临时移走 `nuttx/.config`、`include/nuttx/config.h`、`include/arch`，构建后已按原内容恢复） |
| SIM 运行 | **未取得**：`emulator.sh ... -no-window` 94 ms 内退出（EXIT=1），日志停在 DNS 行，无客体输出；与 P294 的沙箱 QEMU 限制一致。埋点输出未经运行验证 |
| 板端包 | `pack_deskmate.sh ynm3000` PASS：config、ELF entrypoint、startup、37 资源、nsh、整镜像；`System.map` 含 `dm_perf_init`（0x4164b190）；镜像 `output/ynm3000/ynm3000.img` SHA256 `81ec2a4e7956d3c3…` |
| 硬件 | 未刷机；H0/L0/T0/D0 基线、p50/p95、脏区、堆峰值全部待测 |

## 下一步

1. 上板跑 B1 基线：H0/L0 各预热 30 s 采样 60 s，3 次；T0 往返 10 次；D0 状态边沿。记录 `[perf]` 行与驱动 `[fb] upd avg`。
2. 用 `DM_PERF_ENABLE=0` 的同场景结果量化埋点开销；开销不可忽略则先降采样再谈优化。
3. 有基线后再按 §2 分层排名选 E1 唯一变量，未取得数据前不指定优化点。
## 收尾三段

- **代码**：新增 `dm_perf.c/.h` 与三处接入（`sources.txt`、`Makefile`、`luncher_dm.c`）；SIM 编译通过、板包 PASS、`System.map` 含 `dm_perf_init`。SIM 运行未取得证据，硬件性能与视觉均待测。既有未提交差异未提交、未回滚。
- **文档**：工作单补 §6（B0 结论）并修正 §1 的"32 位颜色和 NEON"描述（补 G2D 与 `FBIO_UPDATE` 路径）；`doc_check.py` 14 条既有 handoff 警示，无新增断链；`sync_docs.sh` 已导出，镜像一致。
- **会话日志**：`finalize_check.py` 报 LOGS MISSING（CodeBuddy 会话不在 `logs/` 采集范围），未导出、未伪造补录。

