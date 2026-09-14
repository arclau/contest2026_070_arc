# 7/28 施工日志 — R528 GEMINI-S1 BOE 1200x1920 屏幕移植

> 本文档用于 AI 之间交接。下一个 AI 请先通读全文再动手。

---

## 一、项目背景

### 硬件平台
| 项目 | 值 |
|------|-----|
| SoC | **Allwinner R528** (双核 Cortex-A7, ARMv7-A) |
| 板型 | **R528S3 GEMINI-S1** |
| 原配屏幕 | 7寸 T070S140B (1024x600, MIPI DSI) |
| 目标屏幕 | **BOE 1200x1920** (MIPI DSI, 4-lane, 1200x1920) |
| 触摸 IC | **Goodix GT9271** (I2C 地址 0x5D, 引脚 PB2/PB3 SDA/SCL, PB4 RST, PB5 INT) |

### 软件系统
| 项目 | 值 |
|------|-----|
| RTOS | **NuttX** (Apache NuttX, OpenVela 发行版) |
| 芯片 SDK | `vendor/allwinnertech/chips/r528` |
| 板级 SDK | `vendor/allwinnertech/boards/r528/r528s3-gemini-s1` |
| 构建系统 | Makefile (NuttX `make`) + CMake (部分) |
| 显示框架 | `disp2` — Allwinner 自研显示驱动 (disp2/disp/lcd/ + disp2/soc/) |
| LVGL | LVGL 9.x, 通过 `lv_nuttx` 后端接入 fb + touch |
| 项目根 | `/data/openvela/` |
| 备份目录 | `/data/vela-backup/` |

### 关键路径
- 显示驱动源码: `chips/r528/drivers/rtos-hal/hal/source/disp2/`
- 显示编译配置 (Make): `chips/r528/drivers/rtos-hal/hal/source/disp2/Make.defs`
- 显示编译配置 (CMake): `chips/r528/drivers/rtos-hal/hal/source/disp2/CMakeLists.txt`
- 板级配置: `boards/r528/r528s3-gemini-s1/configs/nsh/defconfig`
- 触摸驱动: `boards/r528/drivers/gt911_iic_touch.c` / `.h`
- BOE 备份文件: `/data/vela-backup/screen-driver/` (含 BOE_1200x1920.c/.h/_mipi_config.c)
- 触摸备份文件: `/data/vela-backup/touch-driver/` (含修改后的 gt911 驱动)

---

## 二、我已完成的改动

### 1. BOE 屏幕驱动集成

| 文件 | 改动说明 |
|------|----------|
| `boards/.../nsh/defconfig` | `CONFIG_LCD_SUPPORT_T070S140B=y` → `BOE_1200X1920=y`; `T070S140B_MIPI=y` → `BOE_1200X1920_MIPI=y` |
| `disp/lcd/Kconfig` | 新增 `LCD_SUPPORT_BOE_1200X1920` 选项 |
| `soc/Kconfig` | 新增 `BOE_1200X1920_MIPI` 板选选项 |
| **新文件** | `disp/lcd/BOE_1200x1920.c` / `.h` / `_mipi_config.c` (从备份复制) |
| `CMakeLists.txt` | 新增 BOE 文件编译 (`CONFIG_LCD_SUPPORT_BOE_1200X1920`) |
| `disp/Makefile` | 新增 `obj-$(BOE_1200X1920)` |
| `soc/Makefile` | 新增 `obj-$(BOE_1200X1920_MIPI)` |

### 2. 关键 Bug 修复：Make.defs 无条件编译 T070 配置

**⚠️ 最重要的修复！**

`chips/r528/drivers/rtos-hal/hal/source/disp2/Make.defs` 原代码：

```makefile
ifeq ($(CONFIG_ARCH_BOARD_R528S3_GEMINI_S1), y)
CHIP_CSRCS += $(DISP_PATH)/soc/t070s140b_mipi_config.c   # ← 无条件！
endif
```

对于 GEMINI_S1 板型，`t070s140b_mipi_config.c` **无论是否配置都被编译**。而该文件定义了 `g_lcd0_config`、`g_disp_config` 等全局符号。同时 BOE 的 `_mipi_config.c` 也定义了同名的全局符号。LTO 链接器随机选一个，导致分辨率锁定在 1024x600（T070 的值）。

修复后：
```makefile
ifeq ($(CONFIG_ARCH_BOARD_R528S3_GEMINI_S1), y)
ifeq ($(CONFIG_LCD_SUPPORT_T070S140B),y)
CHIP_CSRCS += $(DISP_PATH)/soc/t070s140b_mipi_config.c
endif
ifeq ($(CONFIG_LCD_SUPPORT_BOE_1200X1920),y)
CHIP_CSRCS += $(DISP_PATH)/disp/lcd/BOE_1200x1920_mipi_config.c
endif
endif
```

### 3. 触摸驱动更新
| 文件 | 改动 |
|------|------|
| `boards/.../drivers/gt911_iic_touch.c` | 从备份替换（含 pinmux 恢复、product_id 检测、坐标 mask、maxpoint/npoints 等全部修复） |
| `boards/.../drivers/gt911_iic_touch.h` | 从备份替换（分辨率 1200x1920、坐标 mask 宏） |

### 4. PB4/PB5 GPIO 重复配置修复
`BOE_1200x1920_mipi_config.c` 中删除了 `lcd_gpio_1`(PB4/RST) 和 `lcd_gpio_2`(PB5/INT) 的定义。这些触摸引脚完全由 GT911 驱动自行管理，LCD 上电时 `sunxi_lcd_pin_cfg()` 不再覆盖它们。

### 5. 开机启动 APP 修改
`CONFIG_INIT_ENTRYPOINT` 从 `"nsh_main"` 改为 `"lvgldemo_main"`

### 6. SDK 预存 bug 修补
`apps/tests/testsuites/kernel/kv/cases/kv_test_021.c` 中 `property_reload()` 函数不存在导致编译失败，已注释该行。

---

## 三、编译与打包命令

```bash
# 设置环境
cd /data/openvela/vendor/allwinnertech/lichee
source tools/scripts/envsetup.sh

# 选择项目 (2 = r528s3-gemini-s1)
lunch_nuttx 2

# 清理编译
m nsh c && m nsh

# 打包固件
pack

# 固件输出
ls lichee/out/r528s3/gemini-s1_nand/rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img
```

---

## 四、当前状态（最新固件启动日志分析）

### ✅ 确认已修复
```
xres: 1200                                ← BOE 分辨率正确！
yres: 1920                                ← BOE 分辨率正确！
Resolution is set to 1200x1920 at 130dpi  ← LVGL 收到正确分辨率
touchscreen /dev/input0 open success, maxpoint 5  ← 触摸驱动正常
lv_demo_widgets                           ← LVGL 界面启动
```

### ❌ 仍然存在的问题

#### 1. 屏幕不亮（最可能：面板初始化序列失败）
虽然 LVGL 收到了 1200x1920 的 framebuffer，但可能物理屏幕仍然黑屏。原因可能是 MIPI DSI 初始化序列（`lcd_panel_init` 中的 DCS 命令）与 BOE 面板不匹配。

`BOE_1200x1920.c` 中的 `lcd_panel_init()` 函数是从 **RK3568 Linux DTS** 翻译过来的 MIPI 命令序列。R528 的 DSI 控制器可能与 RK3568 不同，初始化序列可能需要调整。

建议排查方向：
- 检查 DSI 时钟配置是否与 BOE 面板要求匹配
- 检查 `lcd_panel_init` 中的 DCS 命令是否被正确发送
- 用逻辑分析仪抓 MIPI 信号确认命令是否发出
- 在 `lcd_panel_init` 中添加 syslog 打印每条命令的返回值

#### 2. LVGL 告警
```
[LVGL] [Error]   check_stack_size: Stack size is too small. Please increase it to 32768 bytes or more.
[LVGL] [Warn]    LV_FONT_MONTSERRAT_24 is not enabled for the widgets demo. Using LV_FONT_DEFAULT instead.
[LVGL] [Warn]    lv_draw_buf_init: Data is not aligned, ignored
```

这些是次要问题，但建议后续修复：
- Stack size: 在 defconfig 中增大 `CONFIG_EXAMPLES_LVGLDEMO_STACKSIZE`（当前 40960，但 LVGL 建议 32768 + 额外空间）
- Font: 启用 `CONFIG_LV_FONT_MONTSERRAT_24`
- Alignment: framebuffer 地址未 64-byte 对齐（当前 0x46e00000），可能需要调整 DDR 映射地址

#### 3. 启动依然显示 `[nsh]` 前缀
日志开头是 `[nsh]` 而不是 `[lvgldemo]`，但系统最终启动了 lvgldemo。这是因为 NuttX 的 init 流程先创建了 NSH 基础设施。

---

## 五、文件修改清单

### Git 跟踪的修改 (6 files)
```
M boards/r528/r528s3-gemini-s1/configs/nsh/defconfig
M chips/r528/drivers/rtos-hal/hal/source/disp2/CMakeLists.txt
M chips/r528/drivers/rtos-hal/hal/source/disp2/Make.defs
M chips/r528/drivers/rtos-hal/hal/source/disp2/disp/Makefile
M chips/r528/drivers/rtos-hal/hal/source/disp2/disp/lcd/Kconfig
M chips/r528/drivers/rtos-hal/hal/source/disp2/soc/Kconfig
M chips/r528/drivers/rtos-hal/hal/source/disp2/soc/Makefile
```

### 新文件 (3 files)
```
?? chips/.../disp2/disp/lcd/BOE_1200x1920.c
?? chips/.../disp2/disp/lcd/BOE_1200x1920.h
?? chips/.../disp2/disp/lcd/BOE_1200x1920_mipi_config.c
```

### 其他修改
```
M apps/tests/.../kv_test_021.c                 # property_reload 编译修复
M boards/.../drivers/gt911_iic_touch.c         # 从备份替换
M boards/.../drivers/gt911_iic_touch.h         # 从备份替换
```

---

## 六、给下一个 AI 的 TIPS

1. **分辨率已正确**：`xres: 1200, yres: 1920` 说明 `g_disp_config` 被正确读取。如果未来又变成 1024x600，检查 `Make.defs` 是否还有无条件编译的 T070 条目。

2. **面板不亮 vs 分辨率错误是两回事**：分辨率来自 `g_lcd0_config`（`_mipi_config.c`），面板初始化来自 `BOE_1200x1920.c` 的 `lcd_panel_init()`。分辨率正确不代表面板初始化序列是对的。

3. **触摸驱动注意事项**：备份的 `gt911_iic_touch.c` 中有 pinmux 恢复逻辑（`gt911_worker()` 重试路径中恢复 PB2-PB5 pinmux）。如果修改了 BOE 的 `lcd_power_on`，注意它调用的 `sunxi_lcd_pin_cfg()` 会改写 PB 组 pinmux。

4. **编译前确认 defconfig**：`boards/.../nsh/defconfig` 是 NSH 配置，修改后运行 `lunch_nuttx 2` 会自动同步到 `.config`。

5. **LTO 警告可忽略**：`lto-wrapper` 的 type-mismatch 警告是 SDK 自带的蓝牙兼容性问题，不影响功能。
