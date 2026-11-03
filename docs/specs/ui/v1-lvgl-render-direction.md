# V1 Home 与锁屏 LVGL 渲染调优工作单

> 平台：`ynm3000`，BOE 面板横屏 1920×1200，应用 `vendor/allwinnertech/apps/luncher_dm/`。
> 目标：在保持 Home、锁屏视觉和交互的前提下，找出实际刷新成本并降低它。
> 用法：AI 或人工每次只领取一个工作包。本文规定技术方向和验收；构建、导出、日志按[开发流程](../misc/development-workflow.md)。

## 1. 起点与边界

- [P294](../../devlogs/10-28-p294-v1-lvgl-refresh.md) 已让时钟文字与网络图标在值不变时跳过 setter；SIM 编译、V1 板包校验通过，客体运行与板端性能未验证。它是待量测的现状，不是已证明的流畅度收益。
- 当前尚无同一 V1 板端的刷新耗时、脏区面积、堆峰值基线，也没有被复现并量化的具体卡顿现象。第一步先定义、复现和测量现象，不先猜“壁纸”“阴影”或“LVGL”是根因。
- V2 `luncher_dm_v2` 已冻结。goldfish 为 RGB565 软件绘制；板端有效配置为 32 位颜色、NEON 软件绘制单元和 G2D 绘制单元（`CONFIG_LV_USE_DRAW_G2D=y`），flush 走 `/dev/fb0` 的 `FBIO_UPDATE`（详见 §6）。模拟器可检验调用与生命周期，不能替代板端性能结论。
- 用户已要求停止截图。视觉和触摸由用户/板端人工复核；日志、哈希和计数不能证明观感通过。
- 保留所有既有未提交差异；首轮不改导航、触摸驱动、页面布局或玻璃材质。若证据指向阴影/材质成本，先列清性能收益与视觉代价再做产品取舍。资源与缓存改动必须先有内存和解码证据，见[材质与资源方案](v1-glass-ui-optimization.md)。

## 2. 思考模型：先判定成本落在哪一层

| 层 | 先看什么 | 可区分的证据 |
|---|---|---|
| 状态更新 | 1s 时钟、健康事件、天气、宠物等是否给未变化对象调用 setter | 调用次数、对象是否可见、由此产生的失效区域 |
| LVGL 绘制 | 脏区合并、壁纸重绘、文字/圆弧/阴影绘制 | 每轮脏区像素、绘制耗时、缓存命中与分配 |
| flush/传输 | display flush、G2D/FB 路径和像素格式 | flush 次数、面积、耗时；与绘制耗时分开 |
| 面板/观感 | 实际刷新节奏、残影、掉帧或触摸迟滞 | 同次板端运行记录和人工现象；不能从 SIM 推断 |

一项证据只支持对应层的结论。例如 setter 调用减少只证明少做了该调用；要宣称“更流畅”，还须看到同平台的端到端耗时改善。`CONFIG_LV_DEF_REFR_PERIOD=16` 是调度配置，不是已测得的 60 FPS。

## 3. 固定验收场景与量测口径

先在 V1 板端记录现象、有效配置、ELF 和资源哈希。每种场景先预热 30 秒，再采样 60 秒；基线和改后各做至少 3 次，保持页面内容、传感器状态、网络状态、宠物开关、亮度及后台负载一致。宠物开启时的动画另列场景，不与静止无动画场景混算。

| 场景 | 必查现象 |
|---|---|
| H0 Home 静止 | 无交互时脏区/flush 是否仍持续；跨分钟时钟是否正确更新 |
| L0 锁屏静止 | 进入锁屏后计时、健康环、宠物和状态栏的正常变化与无变化刷新 |
| T0 往返 | Home→锁屏→唤醒→再次锁屏，连续 10 次；关注首次创建、复用、指针与内存 |
| D0 状态边沿 | 在座/离座、喝水/起立、WiFi 状态变化；关注一次变化能否立即显示 |

记录每轮 `LVGL 绘制耗时 p50/p95`、`flush 耗时 p50/p95`、脏区像素总量、flush 次数和堆峰值；无刷新时用零次数/零面积描述，不给不存在的耗时分位数。无法取得的指标写“未测”并说明入口。埋点只汇总窗口数据，不逐帧打印，用启用/关闭埋点的同场景结果量化其开销。页面切换时另记录可见响应延迟。

功能通过还要求 Home 壁纸、玻璃卡、天气/音乐/健康文字和图标不丢失；锁屏双圈、宠物、欢迎卡及唤醒/再次进入正确；跨分钟和 D0 状态边沿及时更新；无崩溃或持续内存增长。用户未在板端复核时，视觉与触摸必须标待验。

**保留补丁的门槛**：功能场景全过；预先选定的主指标在 3 次测量的中位数上改善至少 10%，且改善大于基线轮次波动；其他场景 p95 耗时不持续恶化超过 5%，堆峰值无可重复的增长。若只减少 setter/脏区、端到端耗时未改善，记录为局部成本降低，不宣称流畅度提升。测量波动过大时先修观测方法，不放宽门槛。

## 4. 一轮只做一个可区分实验

1. **观察 O**：写清哪一场景慢或持续重绘、最早证据、源码/配置/ELF 身份和已有差异。无可复现现象时先做量测，不动优化代码。
2. **假设 H**：最多列 3 个候选，每个都说明若成立和不成立应看到什么。候选可从健康环每秒 setter、壁纸解码/缓存、阴影/文字绘制中选，但它们目前都不是根因。
3. **实验 E**：只改一个因素，优先修改最小且能区分 H 的位置。改前读完整构建、刷新、回调和销毁路径，查本工程 LVGL 版本的实际 API 行为。不要同时改样式、缓存、周期和资源。
4. **对照 C**：同板同场景做 A/B；核对运行的是新 ELF 和对应资源。结果只支持或削弱本次预测。连续两次未缩小范围，停补丁并回查基线/埋点。
5. **去留**：达门槛才保留；失败只撤销本轮实验改动。不得靠关闭日志、断言、检查器或吞错误获得“改善”。

## 5. 可分给 AI 或人工的工作包

| 包 | 范围与交付 | 进入下一包的条件 |
|---|---|---|
| B0 观测入口 | 只读追踪 `luncher_dm.c` 的 LVGL 启动与 display/flush 实际路径、板端 `dev_fb.c` 及有效配置；给出最小埋点位置和指标单位 | 主控确认能区分绘制与 flush；找不到就报告未知 |
| B1 基线 | 仅在 B0 确认的位置做可撤销的低开销埋点；跑 H0/L0/T0/D0，记录 3 次基线和埋点开销 | 取得板端数据；若板端不可运行，停在“待测”，不继续凭猜测优化 |
| E1 首个瓶颈 | 按 B1 排名选一个热点，写 O/H/唯一变量/双向预测，最多改两份 V1 源码 | 静态审查、同平台 A/B 和功能场景均过 |
| E2 后续瓶颈 | 仅在 E1 闭环后重排热点；必要时检查 `ui_home.c` 健康环或 `dm_glass.h` 壁纸路径 | 新证据显示它是下一项主要成本 |
| V 收尾 | 主控按开发流程做 goldfish 编译与目标操作、`ynm3000` 构建打包校验、板端运行和文档记录 | 分别报告代码、文档、会话日志；无运行数据不标性能完成 |

每个工作包只加载 `AGENTS.md`、`devlog.md`、本文、该包相关函数及直接调用/销毁路径；不把整个 SDK 或历史日志塞进 200K 上下文。交接用五行：`O / H / 唯一改动 / 结果与证据 / 去留和下一步`。执行者完成单包后交主控审查，主控可沿已授权方向继续下一包；不自动提交、推送、刷机或截图。

**B0 已完成（结论见 §6）；当前下一步是 B1：在板端跑 H0/L0/T0/D0 基线。** 埋点代码已就位（`dm_perf.c`，`DM_PERF_ENABLE=0` 即关），板端数据未取得前不指定下一处代码优化。

## 6. B0 结论：量测入口（2026-10-28，只读）

有效配置取板端构建的 `nuttx/.config`（V1 `ynm3000`，`CONFIG_LCD_SUPPORT_BOE_1200X1920=y`、`CONFIG_LUNCHER_DM_APP=y`）：`LV_COLOR_DEPTH_32=y`、`LV_DEF_REFR_PERIOD=16`、`LV_USE_SYSMON=y`、`LV_USE_PERF_MONITOR=y`、`LV_USE_NUTTX_LCD=n`、`LV_USE_NUTTX_CUSTOM_INIT=n`、`FB_UPDATE=y`、`FB_SYNC=y`、`SUNXI_DISP2_FB_HW_ROTATION_SUPPORT=y`、`FB_NUM=1`、`USEC_PER_TICK=1000`。

**启动与刷新实际路径**

1. `luncher_dm.c:667` `lv_nuttx_dsc_init()` 把 `fb_path` 默认成 `/dev/fb0`（`lv_nuttx_entry.c:99`）；`luncher_dm.c` 里 `#ifdef CONFIG_LV_USE_NUTTX_LCD` 的 `/dev/lcd0` 分支因该配置为 n 而不成立。
2. 于是走 `lv_nuttx_fbdev_create()` + `lv_nuttx_fbdev_set_file("/dev/fb0")`（`lv_nuttx_entry.c:150-151`），不是 LCD 的 `LCDDEVIO_PUTAREA`。
3. fbdev 把 fb `mmap` 后直接当绘制缓冲：`lv_display_set_draw_buffers()` + `LV_DISPLAY_RENDER_MODE_DIRECT`（`lv_nuttx_fbdev.c:152,154`）。`FB_NUM=1` → 无双缓冲，flush 里没有 memcpy，也没有 `FBIOPAN_DISPLAY`。
4. 刷新节拍：`display_refr_timer_cb` 先 `poll(fd, POLLOUT)`，通过才跑 `_lv_display_refr_timer`（`lv_nuttx_fbdev.c:176,188`）。
5. flush：`flush_cb` → `FBIO_UPDATE`（`lv_nuttx_fbdev.c:197,229`）→ `sunxifb_updatearea`：按脏区 `hal_dcache_clean_invalidate` + G2D 旋转 + `set_layer_config`（`dev_fb.c:212`）。

**据此修正 §2 的分层判断**：本板的 "flush/传输" 已退化为一次 ioctl（dcache + G2D 旋转），软件侧没有逐区拷贝或搬页成本；面板传输由显示控制器连续扫描完成。真正可测的是 **绘制提交** 与 **flush ioctl** 两段。另：`CONFIG_LV_USE_DRAW_G2D=y` 且 `lv_draw_g2d` 单元没有 `wait_for_finish`，G2D 是异步提交，`RENDER_READY` 不等于 GPU 完成；因此端到端要看 `REFR_START→REFR_READY`。

**已有入口及不足**

| 入口 | 内容 | 不足 |
|---|---|---|
| LVGL sysmon perf 浮窗（`LV_USE_SYSMON`+`LV_USE_PERF_MONITOR`，`deskmate_ui.c:3703` 放大样式） | FPS/CPU/render%/flush%，1 s 平均 | 只有百分比与均值；无 p50/p95、无脏区像素、无堆 |
| 驱动 `[fb] upd avg`（`dev_fb.c:265`，每 300 帧） | clean / g2d+cfg 平均 us | 均值；不含 LVGL 绘制段与脏区面积；需 rotation 支持打开（本板已开） |
| `LV_USE_PROFILER` | 未启用 | 需改 LVGL 配置，非本轮范围 |

**B1 采用的埋点位置**（应用侧，不改 LVGL 与驱动）：在默认 display 上挂 `LV_EVENT_ALL` 回调，复用 LVGL 已有事件（`lv_refr.c:381/407/436/1345/1347`）：

| 指标 | 事件对 | 单位 |
|---|---|---|
| 一轮刷新总耗时 | `REFR_START` → `REFR_READY` | us（p50/p95/max） |
| 绘制提交 | `RENDER_START` → `RENDER_READY` | us（p50/p95） |
| flush ioctl | `FLUSH_START` → `FLUSH_FINISH` | us（p50/p95/max） |
| 脏区像素 | `FLUSH_*` 事件参数 `lv_area_t` | px（窗口累加） |
| flush 次数 / 刷新轮数 / 刷新间隔 | 同上 | 次 / ms |
| LVGL 堆 | `lv_mem_monitor()` | byte（used/peak/total） |

计时用 `clock_gettime(CLOCK_MONOTONIC)`；`USEC_PER_TICK=1000` 意味着实际量化到约 1 ms，亚毫秒样本会读成 0，分位数按 us 报告并注明量化。窗口 60 s 输出一行，关闭开关是 `dm_perf.h` 的 `DM_PERF_ENABLE`。

**未知（不写结论）**

- G2D 单元是否真的接管首页的填充/图层，未在板端统计。
- `FB_SYNC=y` 但 LVGL 未调用 `sunxifb_waitforvsync`，刷新与 VSYNC 的相位关系未知。
- 板端绘制/flush 耗时、脏区面积、堆峰值：本机无串口设备（`/dev/ttyUSB0` 不存在），**未测**。

