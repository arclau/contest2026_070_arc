# r528/vendor_drivers — R528 板级驱动源码镜像

本目录是 **R528（allwinnertech）板级驱动源码镜像**，按 `vendor_allwinnertech` 仓的**相对路径**逐文件存放。
它通过仓库根目录 `contest2026_070_arc.xml` 中的 `<linkfile>` / `<copyfile>` 映射回编译树
`vendor/allwinnertech/`，使作品专属仓在**不携带整份上游 vendor 仓**的前提下，也能把本作品
新增/改动的驱动源码随仓提供给评审。

> 作品技术亮点（逆向点亮 BOE 大屏、GT9271 触摸、ltr553 环境光）的驱动 C 源码此前只存在于
> 编译树、从未随专属仓提交；本目录即为其补齐的镜像源。

## 映射机制（为什么分 linkfile / copyfile）

`repo` 在映射时的行为决定了必须分两类：

| 类型 | 适用对象 | repo 行为 | 结果 |
|---|---|---|---|
| `<linkfile>` | **上游不存在的新路径** | 创建符号链接 | 正常，不 dirty |
| `<copyfile>` | **上游已跟踪、被我们改动的文件** | remove + copy 覆盖 | 覆盖成功，但工作树变 **dirty** |

- `<linkfile>` 的 dest 若指向**上游 git 已跟踪的文件会失败**：repo 的 removedirs 会保留普通文件，
  随后创建 symlink 报 `File exists`。因此 **linkfile 只能用于上游不存在的新路径**。
- `<copyfile>` 会 remove + copy，**可以覆盖**上游已跟踪文件（代价：工作树 dirty，增量重 sync
  需 `repo sync --force-sync`）。
- dest 一律带 `vendor/allwinnertech/` 前缀，与仓内既有 3 条 linkfile 写法一致。

## 一、新增 9 文件（linkfile 映射）

| # | 路径（相对本目录） | 作用 |
|---|---|---|
| 1 | `apps/gt9271_touch_test/Kconfig` | GT9271 触摸自检 APP 的 Kconfig 开关 |
| 2 | `apps/gt9271_touch_test/Make.defs` | 触摸自检 APP 构建定义 |
| 3 | `apps/gt9271_touch_test/Makefile` | 触摸自检 APP 构建入口 |
| 4 | `apps/gt9271_touch_test/gt9271_touch_test_main.c` | 触摸自检 APP 主程序（I2C 读点、坐标上报自检） |
| 5 | `boards/r528/drivers/gt9271_iic_touch.c` | **GT9271 I2C 触摸驱动**（地址选择时序、零长度写、返回值判据、坐标 mask/burst/maxpoint/circbuf） |
| 6 | `boards/r528/drivers/gt9271_iic_touch.h` | GT9271 触摸驱动头文件 |
| 7 | `chips/r528/drivers/rtos-hal/hal/source/disp2/disp/lcd/BOE_1200x1920.c` | **BOE 1200×1920 MIPI DSI 面板驱动**（由安卓 DTB 逆向翻译的 init 序列/时序） |
| 8 | `chips/r528/drivers/rtos-hal/hal/source/disp2/disp/lcd/BOE_1200x1920.h` | BOE 面板驱动头文件 |
| 9 | `chips/r528/drivers/rtos-hal/hal/source/disp2/soc/BOE_1200x1920_mipi_config.c` | BOE 面板 MIPI 配置（`g_lcd0_config` 等 SoC 级参数） |

> 其中 `apps/gt9271_touch_test` 为**整目录**，manifest 用一条目录 linkfile 映射。

## 二、改动 17 文件（copyfile 覆盖上游）

| # | 路径（相对本目录） | 作用 |
|---|---|---|
| 1 | `boards/r528/drivers/Kconfig` | 板级驱动注册：GT9271 触摸开关 |
| 2 | `boards/r528/r528s3-gemini-s1/src/Makefile` | 板级构建注册：触摸源文件纳入编译 |
| 3 | `boards/r528/drivers/realtek_ieee80211/api/wifi/wifi_conf.c` | Realtek WiFi 配置：**冷启动重试** |
| 4 | `boards/r528/drivers/realtek_ieee80211/api/wifi/wifi_util.c` | Realtek WiFi 工具：**冷启动重试** |
| 5 | `boards/r528/drivers/realtek_ieee80211/os/customer_rtos/customer_rtos_service.c` | Realtek WiFi 服务层：**冷启动重试** |
| 6 | `chips/r528/r528_boot.c` | 启动初始化适配（引脚/时钟等） |
| 7 | `chips/r528/drivers/rtos-hal/hal/source/disp2/Make.defs` | 显示模块构建开关：BOE 面板纳入编译（修 T070 无条件编译致 `g_lcd0_config` 符号冲突） |
| 8 | `chips/r528/drivers/rtos-hal/hal/source/disp2/disp/Makefile` | disp 子模块构建注册 |
| 9 | `chips/r528/drivers/rtos-hal/hal/source/disp2/disp/de/lowlevel_v2x/de_dsi.c` | **DSI 链路加固**：`dsi_gen_wr()` 加超时，根治卡 LOGO 死等 |
| 10 | `chips/r528/drivers/rtos-hal/hal/source/disp2/disp/dev_fb.c` | framebuffer 驱动适配（横屏驱动层旋转/分辨率） |
| 11 | `chips/r528/drivers/rtos-hal/hal/source/disp2/disp/lcd/Kconfig` | LCD 面板 Kconfig：BOE 面板开关 |
| 12 | `chips/r528/drivers/rtos-hal/hal/source/disp2/disp/lcd/panels.c` | 面板注册表：挂载 BOE 面板 |
| 13 | `chips/r528/drivers/rtos-hal/hal/source/disp2/disp/lcd/panels.h` | 面板声明 |
| 14 | `chips/r528/drivers/rtos-hal/hal/source/disp2/soc/Kconfig` | SoC 级 Kconfig：BOE MIPI config 开关 |
| 15 | `chips/r528/drivers/rtos-hal/hal/source/disp2/soc/Makefile` | SoC 级构建注册：BOE MIPI config 纳入编译 |
| 16 | `chips/r528/drivers/rtos-hal/hal/source/sensor/als/ltr553.c` | **LTR553 ALS 环境光驱动**（积分时间 × 增益校正） |
| 17 | `chips/r528/drivers/rtos-hal/hal/source/sound/codecs/sun8iw20-codec.c` | **sun8iw20-codec**：麦克风关断 POP |

## 三、使用与注意事项

- **一次性全新 sync + 编译不受影响**：首次 `repo init` / `repo sync` 会正常建立 linkfile 并按
  copyfile 覆盖目标文件，直接 `./build.sh ...` 即可。
- **增量重 sync 需 `--force-sync`**：copyfile 覆盖后的上游文件在工作树中处于 dirty 状态，
  常规 `repo sync` 会因本地改动而拒绝更新这些路径；需
  `repo sync --force-sync`（或先 `git -C vendor/allwinnertech checkout -- <路径>` 还原）。
- **本目录是源码镜像的“真源”**：修改驱动请改本目录文件并同步更新对应映射；不要把 `.o`/`.su`
  等编译产物放入本目录。
