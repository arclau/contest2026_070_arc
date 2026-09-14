# DevLog 2026-08-03 — R528 BOE 屏 + GT9271 触摸驱动移植与优化

> 工程：openvela (R528) — nsh 配置
> 工作目录：`/data/dm`
> 旧驱动备份：`/data/vela-backup/{screen-driver,touch-driver}`
> 工具链：SDK 自带 `prebuilts/gcc/linux-x86_64/arm-none-eabi` (GCC 13.4.0)

---

## 一、任务背景

原 SDK 使用 7 寸屏（t070s140b, 1024x600）与 GT911 触摸方案。新硬件改为
BOE 1200x1920 MIPI DSI 屏 + GT9271 电容触摸（与 GT911 寄存器兼容但坐标/分辨率不同）。
历史问题：旧方案直接替换 nsh 中的 7 寸屏驱动后，BOE 屏开机不加载。
本次按驱动开发规范进行**代码级重新移植**（不直接复制文件），并对驱动做了专业优化。

---

## 二、BOE 1200x1920 屏幕驱动移植

### 2.1 新增文件（新 SDK disp2 框架）

| 文件 | 说明 |
|------|------|
| `vendor/allwinnertech/chips/r528/drivers/rtos-hal/hal/source/disp2/disp/lcd/BOE_1200x1920.c` | 面板驱动：上下电时序、背光、完整 init 序列（~150 条寄存器写入，逐条保留自 DTS panel-init-sequence） |
| `vendor/allwinnertech/chips/r528/drivers/rtos-hal/hal/source/disp2/disp/lcd/BOE_1200x1920.h` | 面板声明（`struct __lcd_panel`） |
| `vendor/allwinnertech/chips/r528/drivers/rtos-hal/hal/source/disp2/soc/BOE_1200x1920_mipi_config.c` | SOC 配置：时序 HT=1260/VT=1956、DSI 4-lane@292MHz/RGB888、GPIO（PD19 复位、PD20 背光 PWM、PD0~9 DSI）、电源（dcdc1/2/3） |

### 2.2 注册（7 处）

- `disp/lcd/panels.c` / `panels.h`：panel_array 条目 + extern 声明
- `disp/lcd/Kconfig`：`CONFIG_LCD_SUPPORT_BOE_1200X1920`
- `soc/Kconfig` + `soc/Makefile`：板级 `CONFIG_BOE_1200X1920_MIPI`
- `disp/Makefile`：编译目标
- `configs/nsh/defconfig`：`CONFIG_LCD_SUPPORT_BOE_1200X1920=y`、`CONFIG_BOE_1200X1920_MIPI=y`

### 2.3 关键决策

- **mipi_config 二选一**：新 SDK 所有 `*_mipi_config.c` 共享全局符号
  `g_lcd0_config`/`g_disp_config`，`CONFIG_T070S140B_MIPI` 与 `CONFIG_BOE_1200X1920_MIPI`
  必须互斥编译，否则链接重复定义 —— 这是历史"开机不加载 BOE"的根源之一。
- **GPIO 结构差异**：新 SDK 的 `gpio_list` 已不含 `.gpio` 字段，移植时按新模板去掉。
- **面板名匹配**：`lcd_driver_name="BOE_1200x1920"` 与面板 `.name` 精确一致（disp_lcd.c 按名查表）。

---

## 三、GT9271 触摸驱动移植

### 3.1 背景

- 新 SDK 已有 GT911 驱动（简化版：仅识别 "911"、1024x600 减法翻转、maxpoint=1、无失败恢复）。
- 硬件为 GT9271：寄存器与 GT911 同族（product ID 读回 "9271"），但分辨率、坐标缩放、抗干扰逻辑不同。
- 决策：**不与 GT911 重叠**，新建独立 `CONFIG_GT9271_IIC_TOUCH` 项，保留旧备份中的项目定制改动。

### 3.2 新增文件

| 文件 | 说明 |
|------|------|
| `vendor/allwinnertech/boards/r528/drivers/gt9271_iic_touch.c` | 触摸驱动：检测 "911"/"927" 前缀、1200x1920 坐标缩放、I2C 失败重复位、poll 模式自适应轮询 |
| `vendor/allwinnertech/boards/r528/drivers/gt9271_iic_touch.h` | 驱动接口与寄存器定义 |

### 3.3 注册

- `boards/r528/drivers/Kconfig`：`CONFIG_GT9271_IIC_TOUCH`
- `boards/r528/r528s3-gemini-s1/src/Makefile`：CSRCS += gt9271_iic_touch.c
- `chips/r528/r528_boot.c`：`CONFIG_GT9271_IIC_TOUCH` 下注册 `/dev/input0`（i2c_bus0）
- `configs/nsh/defconfig`：`CONFIG_GT911_IIC_TOUCH=y` → `CONFIG_GT9271_IIC_TOUCH=y`

### 3.4 保留的项目定制（来自旧备份）

- product ID 检测接受 "911"/"927"（兼容 GT911/GT9271/GT9272）
- 原始坐标按芯片分辨率读取（0x8146~0x8149）缩放至 LCD 像素分辨率（1200x1920），用 max+1 取整
- I2C 失败计数 + 引脚 pinmux 恢复 + 芯片重复位（应对 LCD 上电 pinmux 竞争）
- poll 模式自适应间隔（16ms 起，失败退避至 100ms）

---

## 四、工具链与构建问题修复

### 4.1 libxx 编译失败（C++17）

- 症状：`libstdc++/libsupc++/del_opa.cc: 'std::align_val_t' has not been declared`
- 根因：直接 `make -C nuttx` 绕过了 envsetup，PATH 命中系统
  `/usr/bin/arm-none-eabi-gcc`（10.3.1），其 newlib C++ 头文件过旧。
- 修复：使用 `./build.sh`（自动 `source build/envsetup.sh`，注入 SDK 自带
  GCC 13.4.0 prebuilt 工具链），libxx 编译通过。

### 4.2 链接失败（undefined reference to BOE_1200x1920_panel）

- 根因：新 SDK 的 disp2 实际编译入口是 NuttX 风格 `disp2/Make.defs`
  （`CHIP_CSRCS`），kbuild 风格 `obj-$(CONFIG_...)` 行不参与该链路。
- 修复：在 `disp2/Make.defs` 补注册 BOE 面板驱动与 mipi_config（soc 段二选一）。

---

## 五、驱动代码优化（本次）

### 5.1 触摸驱动 GPIO 重复初始化消除

排查结论：
- BOE mipi_config 中**已不含**触摸 GPIO（PB2~PB5），触摸引脚完全由触摸驱动管理，
  屏幕/触摸两侧无跨驱动冲突。
- 但 GT9271 驱动内部存在重复：`gt9271_control_initialize()` 与
  `gt9271_worker()` I2C 失败恢复路径各写了一遍相同的 pinmux/direction 配置。

优化：抽取 `gt9271_pins_config()` 公共函数，集中配置
PB2/PB3（TWI mux）、PB4（RST 输出）、PB5（INT 输出），三处调用点复用，
消除 ~15 行重复代码，保证初始化与故障恢复路径行为一致。

### 5.2 r528_boot.c PWM 宏残留 bug 修复

- 症状：`#ifdef LCD_SUPPORT_T070S140B` 缺 `CONFIG_` 前缀，条件永不成立，
  背光 PWM 始终走 `pwm_initialize("/dev/pwm0", 0)`（通道 0）。
- 影响：BOE 屏 mipi_config 的 `lcd_pwm_ch=4`（PD20），PWM 通道不匹配，
  背光无法正常工作。
- 修复：改为 `#ifdef CONFIG_LCD_SUPPORT_BOE_1200X1920` → 通道 4。

---

## 六、编译验证结果

| 项目 | 结果 |
|------|------|
| configure（Kconfig 语法） | ✅ nsh 配置生成成功，BOE/GT9271 生效，T070S140B/GT911 关闭 |
| 语法级编译（-fsyntax-only） | ✅ BOE_1200x1920.c / mipi_config.c / gt9271_iic_touch.c 均通过 |
| 全量构建 `./build.sh` | ✅ LD 链接通过，`nuttx/vela.bin`（~5.4MB）生成 |
| 目标文件归档 | ✅ BOE_1200x1920.o / BOE_1200x1920_mipi_config.o 已进 libarch.a |
| 固件镜像 | ✅ vela.bin 已转换并拷贝至 lichee board 目录 |

---

## 七、遗留事项 / 下一步

1. **上板验证**：烧录 `vela.bin`，确认 BOE 屏点亮、触摸坐标方向与缩放正确
   （当前按 1200x1920 缩放；若触摸方向异常，检查 `TOUCH_POINT_GET_X/Y` 与旋转配置）。
2. **背光 PWM 验证**：确认 `CONFIG_LCD_SUPPORT_BOE_1200X1920` 下 PWM4 (PD20) 输出正常。
3. 触摸坐标与旋转：若启用 `CONFIG_SUNXI_DISP2_FB_HW_ROTATION_SUPPORT`，
   触摸轴可能需要同步旋转映射（当前 GT9271 按原生方向输出）。
4. 如需回退 7 寸屏方案，defconfig 中恢复 `CONFIG_LCD_SUPPORT_T070S140B` +
   `CONFIG_T070S140B_MIPI` + `CONFIG_GT911_IIC_TOUCH`（与 BOE/GT9271 互斥）。

---

*DevLog by AtomCode (deepseek-v4-flash)*
