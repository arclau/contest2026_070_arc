# DTS → Vela MIPI 屏幕驱动移植技能

> 从 ARM64 Linux 平台 DTS 提取 MIPI 屏幕信息，自动生成 Vela/NuttX (R528 + OpenVela SDK) 平台驱动

---

## 一、工作流总览

```
┌─────────────────────────────────────────────────────────────────────┐
│ ① 解析 Linux DTS → 提取 MIPI 屏幕全部信息                           │
│    (分辨率、时序、初始化序列、DSI 配置、GPIO、背光、电源)            │
├─────────────────────────────────────────────────────────────────────┤
│ ② 分析模板驱动 (t070s140b / ili9341) → 理解 Vela 驱动框架结构        │
│    (面板驱动 .c/.h + SOC 配置 _mipi_config.c + Kconfig + Makefile)   │
├─────────────────────────────────────────────────────────────────────┤
│ ③ 生成屏幕信息 MD 文档 → vela/ 目录下                              │
│    (完整参数表 + 参数来源标注 + 架构差异说明)                       │
├─────────────────────────────────────────────────────────────────────┤
│ ④ 复制模板驱动 → 按屏幕信息命名新驱动文件                          │
│    (面板名取自 compatible / panel 节点名)                           │
├─────────────────────────────────────────────────────────────────────┤
│ ⑤ 数据转换 → 将 Linux DTS 参数精确转换为 Vela 驱动参数             │
│    (时钟 MHz 转换、时序计算、初始化序列翻译、GPIO 映射)             │
├─────────────────────────────────────────────────────────────────────┤
│ ⑥ 注册驱动 → 更新 panels.c/panels.h/Kconfig/Makefile              │
├─────────────────────────────────────────────────────────────────────┤
│ ⑦ 代码审查 → 前瞻性检查 bug 并提前修复                            │
│    (内存泄漏、时序越界、GPIO 冲突、DSI 配置不匹配)                 │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 二、环境与路径

### 2.1 Vela SDK 关键路径

```bash
# 面板驱动目录
PANEL_DIR="vendor/allwinnertech/chips/r528/drivers/rtos-hal/hal/source/disp2/disp/lcd/"
# SOC 配置目录
SOC_DIR="vendor/allwinnertech/chips/r528/drivers/rtos-hal/hal/source/disp2/soc/"
# 面板注册文件
PANELS_C="${PANEL_DIR}panels.c"
PANELS_H="${PANEL_DIR}panels.h"
# 面板 Kconfig
PANEL_KCONFIG="${PANEL_DIR}Kconfig"
# SOC Kconfig
SOC_KCONFIG="${SOC_DIR}Kconfig"
# SOC Makefile
SOC_MAKEFILE="${SOC_DIR}Makefile"
# 项目 defconfig（提交配置；nsh/ 保持上游干净勿动）
DEFCONFIG="vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/deskmate/defconfig"
```

### 2.2 模板文件（参考）

| 模板类型 | 文件 | 说明 |
|---------|------|------|
| **MIPI DSI 面板驱动** | `t070s140b.c` / `.h` | 7寸 1024x600 MIPI DSI, EK79007AD2 驱动IC |
| **MIPI DSI SOC 配置** | `t070s140b_mipi_config.c` | 分辨率、时序、DSI lane、GPIO、PWM |
| **实战案例（本作品）** | `BOE_1200x1920.c` / `.h` + `boe_1200x1920_mipi_config.c` | BOE 1200×1920 MIPI DSI（横屏 1920×1200），取自旧平板的 BOE 屏逆向：MIPI 引脚定义 + init 序列 + GT9271 触摸配置 |
| **SPI 面板驱动** | `ili9341.c` / `.h` | 320x240 SPI, ILI9341 驱动IC |
| **SPI SOC 配置** | `ili9341_lcd_config.c` / `ili9341_lcd_spi.c` | SPI 接口配置 |

> ⚠️ `mipi_config` 二选一：`CONFIG_T070S140B_MIPI` 与 `CONFIG_BOE_1200X1920_MIPI` 必须互斥，
> 否则全局符号 `g_lcd0_config` 重复定义（BOE 开机不加载的根源）。换屏时先关旧屏 Kconfig。

---

## 三、阶段①：解析 Linux DTS → 提取 MIPI 屏幕信息

### 3.1 DTS 中的 MIPI 屏幕节点结构

典型的 RK3568/RK3588 Firefly DTS 中 MIPI 屏幕结构如下：

```dts
/* ===== DTS 节点树 ===== */
&dsi0 {
    status = "okay";
    dsi0_panel: panel@0 {                          // ← 面板节点
        status = "okay";
        compatible = "simple-panel-dsi";            // ← 面板兼容性
        reg = <0>;
        backlight = <&backlight_0>;                  // ← 背光引用
        reset-gpios = <&gpio3 RK_PB5 GPIO_ACTIVE_LOW>;  // ← 复位 GPIO
        reset-delay-ms = <20>;                      // ← 复位延时
        init-delay-ms = <20>;                       // ← 初始化延时
        enable-delay-ms = <20>;                     // ← 使能延时
        prepare-delay-ms = <20>;
        unprepare-delay-ms = <20>;
        disable-delay-ms = <20>;
        
        dsi,flags = <(MIPI_DSI_MODE_VIDEO | ...)>;  // ← DSI 模式标志
        dsi,format = <MIPI_DSI_FMT_RGB888>;         // ← 像素格式
        dsi,lanes  = <4>;                           // ← DSI Lane 数

        panel-init-sequence = [                      // ← 初始化序列（关键！）
            05 14 01 10                              // DCS_LONG_WRITE, 20ms, 1param, SleepOut
            15 00 02 b0 05                           // GEN_SHORT_WRITE_2PARAM, 0ms, 2param, reg=0xb0, val=0x05
            15 64 02 11 00                           // GEN_SHORT_WRITE_2PARAM, 100ms, 2param, SleepIn
            15 0a 02 29 00                           // GEN_SHORT_WRITE_2PARAM, 10ms, 2param, DisplayOn
        ];
        
        panel-exit-sequence = [                      // ← 退出序列
            05 00 01 28                              // DCS_LONG_WRITE, 0ms, 1param, DisplayOff
            05 78 01 10                              // DCS_LONG_WRITE, 120ms, 1param, SleepIn
        ];

        disp_timings0: display-timings {            // ← 时序参数
            native-mode = <&dsi0_timing0>;
            dsi0_timing0: timing0 {
                clock-frequency = <146000000>;        // ← 像素时钟 (Hz)
                hactive = <1200>;                     // ← 水平有效像素
                vactive = <1920>;                     // ← 垂直有效像素
                hfront-porch = <20>;                  // ← 水平前肩
                hback-porch = <30>;                   // ← 水平后肩
                hsync-len = <10>;                     // ← 水平同步脉冲
                vfront-porch = <16>;                  // ← 垂直前肩
                vback-porch = <16>;                   // ← 垂直后肩
                vsync-len = <4>;                      // ← 垂直同步脉冲
                hsync-active = <0>;                   // ← 同步极性
                vsync-active = <0>;
                de-active = <0>;
                pixelclk-active = <0>;
            };
        };
    };
};

/* 背光节点 */
backlight_0: backlight-0 {
    compatible = "pwm-backlight";
    pwms = <&pwm4 0 25000 0>;      // ← PWM4, period=25000ns
    default-brightness-level = <200>;
    brightness-levels = <0 1 2 ... 255>;
};

/* 电源节点 */
vcc1v8_lcd0_power: vcc1v8-lcd0-power-regulator {
    compatible = "regulator-fixed";
    enable-active-high;
    gpio = <&gpio0 RK_PC7 GPIO_ACTIVE_HIGH>;
    regulator-name = "vcc1v8_lcd0_pw_en";
    regulator-always-on;
};
```

### 3.2 提取参数对照表

#### 3.2.1 分辨率与时序

| Linux DTS 参数 | 含义 | Vela 参数名 | 转换规则 |
|---------------|------|------------|---------|
| `clock-frequency` | 像素时钟 (Hz) | `lcd_dclk_freq` | **÷ 1000000 → MHz** (如 146000000 → 146) |
| `hactive` | 水平分辨率 | `lcd_x` | 直接使用 |
| `vactive` | 垂直分辨率 | `lcd_y` | 直接使用 |
| `hsync-len` | 水平同步宽度 | `lcd_hspw` | 直接使用 |
| `hback-porch` | 水平后肩 | `lcd_hbp` | 直接使用 |
| `hfront-porch` | 水平前肩 | `lcd_hfp` | Vela 需要 `lcd_hfp`，DTS 直接提供 |
| `vback-porch` | 垂直后肩 | `lcd_vbp` | 直接使用 |
| `vfront-porch` | 垂直前肩 | `lcd_vfp` | Vela 需要 `lcd_vfp`，DTS 直接提供 |
| `vsync-len` | 垂直同步宽度 | `lcd_vspw` | 直接使用 |
| — | 水平总周期 | `lcd_ht` | **= hactive + hfp + hbp + hsync_len** |
| — | 垂直总周期 | `lcd_vt` | **= vactive + vfp + vbp + vsync_len** |

> **⚠️ 关键差异**：Linux DTS 使用 `hfront-porch`/`hback-porch`/`hsync-len` 独立字段；Vela 使用 `lcd_ht`/`lcd_hbp`/`lcd_hspw`/`lcd_hfp` 组合。**必须计算 `lcd_ht` 和 `lcd_vt`**。

#### 3.2.2 DSI 配置

| Linux DTS 参数 | 含义 | Vela 参数名 | 值映射 |
|---------------|------|------------|--------|
| `dsi,lanes` | Lane 数 | `lcd_dsi_lane` | 4 → `LCD_DSI_LANE_4` (4) |
| `dsi,format` | 像素格式 | `lcd_dsi_format` | `MIPI_DSI_FMT_RGB888` → 0 |
| `dsi,flags` | 模式标志 | `lcd_dsi_if` | 含 `MIPI_DSI_MODE_VIDEO` → `LCD_DSI_IF_VIDEO_MODE` (0) |
| — | DSI 时钟率 | `lcd_dsi_clk_rate` | **需计算**：`lcd_dclk_freq × 8 / lane`，单位 MHz |

**DSI 时钟率计算公式**：
```
DSI_CLK = (lcd_ht × lcd_vt × fps × bits_per_pixel) / (2 × lane_count) / 1000000
其中 bits_per_pixel = 24 (RGB888), fps = 60
简化：DSI_CLK ≈ lcd_dclk_freq × 8 / lane_count
```

#### 3.2.3 初始化序列解析

Linux DTS 的 `panel-init-sequence` 使用字节流格式，每个命令 4+ 字节：

```
┌────────┬────────┬────────┬────────┬──────────────┐
│  type  │ delay  │  npar  │  cmd   │  param(s)    │
│  1byte │  1byte │  1byte │ 1byte  │  npar-1 byte │
└────────┴────────┴────────┴────────┴──────────────┘
```

| type 字节 | 含义 | Vela API |
|----------|------|---------|
| `0x05` | DCS Long Write | `sunxi_lcd_dsi_dcs_write_*para()` |
| `0x15` | Generic Short Write with 2 param | `sunxi_lcd_dsi_gen_write_1para()` |
| `0x39` | Generic Long Write | `sunxi_lcd_dsi_gen_write_*para()` |

**转换规则**：

```
DTS: 05 14 01 10
      │   │  │  └── cmd=0x10 (Sleep Out)
      │   │  └── npar=1 (1 param = cmd only)
      │   └── delay=20ms
      └── type=DCS_LONG_WRITE
→ Vela: sunxi_lcd_dsi_dcs_write_0para(sel, 0x10);
        sunxi_lcd_delay_ms(20);

DTS: 15 00 02 b0 05
      │   │  │  │  └── param=0x05
      │   │  │  └── cmd=0xb0
      │   │  └── npar=2 (cmd + 1 param)
      │   └── delay=0ms
      └── type=GEN_SHORT_WRITE_2PARAM
→ Vela: sunxi_lcd_dsi_gen_write_1para(sel, 0xb0, 0x05);
        // delay=0, no delay needed

DTS: 15 64 02 11 00
      │   │  │  │  └── param=0x00
      │   │  │  └── cmd=0x11 (Sleep In)
      │   │  └── npar=2
      │   └── delay=100ms (0x64)
      └── type=GEN_SHORT_WRITE_2PARAM
→ Vela: sunxi_lcd_dsi_gen_write_1para(sel, 0x11, 0x00);
        sunxi_lcd_delay_ms(100);
```

> **⚠️ 注意**：`npar` 包含 cmd 本身。所以 `npar=2` 表示 cmd + 1 param，`npar=1` 表示只有 cmd。

#### 3.2.4 退出序列解析

格式与初始化序列相同，但通常只有 Display Off 和 Sleep In。

| DTS 字节 | 含义 | Vela API |
|---------|------|---------|
| `05 00 01 28` | Display Off, 0ms | `sunxi_lcd_dsi_dcs_write_0para(sel, 0x28);` |
| `05 78 01 10` | Sleep In, 120ms | `sunxi_lcd_dsi_dcs_write_0para(sel, 0x10);` `sunxi_lcd_delay_ms(120);` |

#### 3.2.5 GPIO 与电源

| Linux DTS 参数 | Vela 对应 | 说明 |
|---------------|----------|------|
| `reset-gpios` | `lcd_gpio_0` (SOC config) | 复位引脚，`panel_reset()` 调用 |
| 电源 regulator | `sunxi_lcd_power_enable()` | Vela 用 AXP2101 电源 ID |
| `enable-delay-ms` | `sunxi_lcd_delay_ms()` | 在 power_on 序列中 |
| `reset-delay-ms` | `sunxi_lcd_delay_ms()` | 复位脉冲宽度 |

**GPIO 编号转换**（Linux → Vela）：

```c
// Linux: <&gpio3 RK_PB5 GPIO_ACTIVE_LOW>
// 含义: GPIO3, B组第5脚, 低电平有效
// Vela: port=3, port_num=13 (B=8, 8+5=13)
// Vela GPIO macro: GPIOD(19) 例如 PD19

// 转换表:
// 组名     Linux宏     Vela port    Vela port_num 基数
//  PA       RK_PAx     0            x + 0
//  PB       RK_PBx     1            x + 8
//  PC       RK_PCx     2            x + 16
//  PD       RK_PDx     3            x + 24
//  PE       RK_PEx     4            x + 32
//  PF       RK_PFx     5            x + 40
//  PG       RK_PGx     6            x + 48
//  PH       RK_PHx     7            x + 56
//  PI       RK_PIx     8            x + 64
```

#### 3.2.6 背光 PWM

| Linux DTS 参数 | 含义 | Vela 参数 |
|---------------|------|----------|
| `pwms = <&pwm4 0 25000 0>` | PWM4, period=25000ns | `lcd_pwm_ch = 4` |
| period=25000ns | 频率 = 1/25000ns = 40KHz | `lcd_pwm_freq = 40000` |
| `default-brightness-level` | 默认亮度 | `lcd_backlight = 200` |

**PWM 频率转换**：
```
pwm_freq (Hz) = 1000000000 / period_ns
例: period=25000ns → freq=40000Hz=40KHz
```

---

## 四、阶段②：生成屏幕信息 MD 文档

根据阶段①提取的参数，生成格式如下的 MD 文档到 `vela/` 目录下：

```markdown
# MIPI 屏幕移植信息 — {panel_name}

> 来源 DTS: `{dts_filename}`
> 目标平台: Vela/NuttX (R528 + OpenVela SDK)

## 1. 基本信息

| 参数 | 值 | 来源 |
|------|-----|------|
| 面板名称 | `{panel_name}` | DTS panel 节点名 |
| 兼容性 | `{compatible}` | `compatible` 属性 |
| 分辨率 | {x} × {y} | timing 节点 |
| 物理尺寸 | {width}mm × {height}mm | 估算或 datasheet |
| 驱动 IC | {ic_name} | 初始化序列分析 |
| 接口类型 | MIPI DSI {lane}-lane | `dsi,lanes` |
| 像素格式 | RGB888 | `dsi,format` |
| DSI 模式 | Video/Burst | `dsi,flags` |

## 2. 时序参数

| 参数 | DTS 值 | Vela 值 | 单位 |
|------|--------|---------|------|
| 像素时钟 | {clock_frequency} | {dclk_freq} | Hz / MHz |
| 水平有效 | {hactive} | {lcd_x} | pixels |
| 垂直有效 | {vactive} | {lcd_y} | pixels |
| 水平前肩 | {hfp} | {lcd_hfp} | pixels |
| 水平后肩 | {hbp} | {lcd_hbp} | pixels |
| 水平同步 | {hsync_len} | {lcd_hspw} | pixels |
| 水平总周期 | {hact+hfp+hbp+hsync} | {lcd_ht} | pixels |
| 垂直前肩 | {vfp} | {lcd_vfp} | pixels |
| 垂直后肩 | {vbp} | {lcd_vbp} | pixels |
| 垂直同步 | {vsync_len} | {lcd_vspw} | pixels |
| 垂直总周期 | {vact+vfp+vbp+vsync} | {lcd_vt} | pixels |
| 刷新率 | {fps} | {fps} | Hz |

## 3. 初始化序列 (panel-init-sequence)

| 步骤 | DTS 字节 | 含义 | Vela API 调用 | 延时 |
|------|---------|------|--------------|------|
| 1 | `05 14 01 10` | Sleep Out | `dcs_write_0para(sel, 0x10)` | 20ms |
| 2 | `15 00 02 b0 05` | Page 0x05 | `gen_write_1para(sel, 0xb0, 0x05)` | 0ms |
| ... | ... | ... | ... | ... |

## 4. 退出序列 (panel-exit-sequence)

| 步骤 | DTS 字节 | 含义 | Vela API 调用 | 延时 |
|------|---------|------|--------------|------|
| 1 | `05 00 01 28` | Display Off | `dcs_write_0para(sel, 0x28)` | 0ms |
| 2 | `05 78 01 10` | Sleep In | `dcs_write_0para(sel, 0x10)` | 120ms |

## 5. GPIO 与电源

| 功能 | Linux DTS 表示 | Vela GPIO 配置 | 说明 |
|------|---------------|---------------|------|
| 复位 | `&gpio3 RK_PB5 GPIO_ACTIVE_LOW` | port=3, port_num=13 | 低电平复位 |
| 电源使能 | `&gpio0 RK_PC7 GPIO_ACTIVE_HIGH` | port=0, port_num=23 | 高电平使能 |

## 6. 背光配置

| 参数 | Linux DTS | Vela 值 |
|------|----------|---------|
| PWM 通道 | pwm4 | ch=4 |
| PWM 周期 | 25000ns | freq=40000Hz |
| 默认亮度 | 200 | 200 |

## 7. 架构差异说明

| 项目 | Linux (RK3568) | Vela (R528/NuttX) | 转换说明 |
|------|---------------|-------------------|---------|
| 操作系统 | Linux 内核 | NuttX RTOS | 无文件系统依赖 |
| 时钟 | CCF 框架 | HAL 时钟 | dclk_freq 直接设 MHz |
| GPIO | pinctrl + gpiod | HAL GPIO | 手动配置 port/port_num |
| 电源 | regulator 框架 | AXP2101 PMIC | 使用 power ID |
| DSI 驱动 | DRM 框架 | disp2 HAL | 使用 sunxi_lcd API |
| 初始化序列 | DTS 字节流 | C 代码硬编码 | 逐条翻译 |

## 8. 注意事项

- {注意点1}
- {注意点2}
```

---

## 五、阶段③：创建面板驱动文件

### 5.1 面板命名规则

```
面板名 = 从 DTS 确定：
  1. 优先使用 compatible 的第二个字段（如 "simple-panel-dsi" → 取 panel 节点名）
  2. 或使用 panel 节点名（如 "dsi0_panel" → "dsi0_panel"）
  3. 或使用分辨率 + 驱动IC（如 "st7701s_1200x1920"）
```

### 5.2 创建面板驱动 .c 文件

从模板 `t070s140b.c` 复制并修改，生成的文件命名 `{panel_name}.c`：

```c
/*
 * {Panel Name} MIPI DSI Panel Driver
 * Auto-generated from Linux DTS: {dts_filename}
 * Target: Vela/NuttX (R528 + OpenVela SDK)
 */

#include "{panel_name}.h"
#include <string.h>
#include <syslog.h>

extern s32 bsp_disp_lcd_set_bright(u32 disp, u32 bright);

/* 面板复位宏 */
#define panel_reset(sel, val) sunxi_lcd_gpio_set_value(sel, 0, val)

/* ===== 前向声明 ===== */
static void lcd_power_on(u32 sel);
static void lcd_power_off(u32 sel);
static void lcd_bl_open(u32 sel);
static void lcd_bl_close(u32 sel);
static void lcd_panel_init(u32 sel);
static void lcd_panel_exit(u32 sel);

/* ===== Gamma 校正表 ===== */
static void lcd_cfg_panel_info(struct panel_extend_para *info)
{
    // gamma 表配置（从原模板复制，或根据 IC 规格调整）
    // ... (与 t070s140b.c 中 lcd_cfg_panel_info 结构相同，可保留模板)
}

/* ===== 开机时序流程 ===== */
static s32 lcd_open_flow(u32 sel)
{
    syslog(LOG_INFO, "{PANEL}: Starting LCD open flow, sel=%lu\n", sel);

    LCD_OPEN_FUNC(sel, lcd_power_on, 10);
    LCD_OPEN_FUNC(sel, lcd_panel_init, {init_delay_ms});  // 从 DTS 提取
    LCD_OPEN_FUNC(sel, sunxi_lcd_tcon_enable, 50);
    LCD_OPEN_FUNC(sel, lcd_bl_open, 0);

    return 0;
}

/* ===== 关机时序流程 ===== */
static s32 lcd_close_flow(u32 sel)
{
    syslog(LOG_INFO, "{PANEL}: Starting LCD close flow, sel=%lu\n", sel);

    LCD_CLOSE_FUNC(sel, lcd_bl_close, 0);
    LCD_CLOSE_FUNC(sel, sunxi_lcd_tcon_disable, 0);
    LCD_CLOSE_FUNC(sel, lcd_panel_exit, 200);
    LCD_CLOSE_FUNC(sel, lcd_power_off, 500);

    return 0;
}

/* ===== 上电时序 ===== */
static void lcd_power_on(u32 sel)
{
    // 从 DTS 提取的电源/复位时序
    // 1. 复位 GPIO 先拉低
    panel_reset(sel, GPIO_DATA_LOW);
    sunxi_lcd_delay_ms({reset_delay_ms});  // 从 DTS reset-delay-ms

    // 2. 使能电源（需要根据实际硬件调整 AXP2101 ID）
    // sunxi_lcd_power_enable(sel, AXP2101_ID_DCDC1);

    // 3. 配置引脚
    sunxi_lcd_pin_cfg(sel, 1);
    sunxi_lcd_delay_ms({enable_delay_ms});  // 从 DTS enable-delay-ms

    // 4. 释放复位（高电平）
    panel_reset(sel, GPIO_DATA_HIGH);
    sunxi_lcd_delay_ms({prepare_delay_ms});  // 从 DTS prepare-delay-ms

    // 5. 使能 DSI 时钟
    sunxi_lcd_dsi_clk_enable(sel);
    syslog(LOG_INFO, "{PANEL}: Power-on sequence completed\n");
}

/* ===== 下电时序（上电的逆序） ===== */
static void lcd_power_off(u32 sel)
{
    syslog(LOG_INFO, "{PANEL}: Power off sequence started\n");
    sunxi_lcd_dsi_clk_disable(sel);
    sunxi_lcd_delay_ms(20);
    panel_reset(sel, GPIO_DATA_LOW);
    sunxi_lcd_delay_ms(20);
    sunxi_lcd_pin_cfg(sel, 0);
    // sunxi_lcd_power_disable(sel, AXP2101_ID_DCDC1);
}

/* ===== 背光控制 ===== */
static void lcd_bl_open(u32 sel)
{
    sunxi_lcd_pwm_enable(sel);
    sunxi_lcd_delay_ms(100);
    sunxi_lcd_backlight_enable(sel);
    sunxi_lcd_delay_ms(100);
}

static void lcd_bl_close(u32 sel)
{
    sunxi_lcd_backlight_disable(sel);
    sunxi_lcd_pwm_disable(sel);
    sunxi_lcd_delay_ms(200);
}

/* ===== 面板初始化序列（从 DTS panel-init-sequence 翻译） ===== */
static void lcd_panel_init(u32 sel)
{
    syslog(LOG_INFO, "{PANEL}: Panel init sequence start\n");

    /* ---- 从 panel-init-sequence 翻译 ---- */
    // 例: 05 14 01 10 → Sleep Out + 20ms
    sunxi_lcd_dsi_dcs_write_0para(sel, DSI_DCS_EXIT_SLEEP_MODE);
    sunxi_lcd_delay_ms(20);

    // 例: 15 00 02 b0 05 → 设置 page 0x05
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB0, 0x05);
    // 无延时

    // 例: 15 00 02 b3 52 → 设置寄存器
    sunxi_lcd_dsi_gen_write_1para(sel, 0xB3, 0x52);

    // ... 更多命令按 DTS 顺序逐条翻译 ...

    // 例: 15 0a 02 29 00 → Display On + 10ms
    sunxi_lcd_dsi_gen_write_1para(sel, 0x29, 0x00);
    sunxi_lcd_delay_ms(10);

    syslog(LOG_INFO, "{PANEL}: Panel init sequence completed\n");
}

/* ===== 面板退出序列 ===== */
static void lcd_panel_exit(u32 sel)
{
    // 从 DTS panel-exit-sequence 翻译
    // 例: 05 00 01 28 → Display Off
    sunxi_lcd_dsi_dcs_write_0para(sel, DSI_DCS_SET_DISPLAY_OFF);
    sunxi_lcd_delay_ms(0);

    // 例: 05 78 01 10 → Sleep In + 120ms
    sunxi_lcd_dsi_dcs_write_0para(sel, DSI_DCS_ENTER_SLEEP_MODE);
    sunxi_lcd_delay_ms(120);
}

/* ===== 用户自定义函数 ===== */
static s32 lcd_user_defined_func(u32 sel, u32 para1, u32 para2, u32 para3)
{
    bsp_disp_lcd_set_bright(sel, 60);
    return 0;
}

/* ===== 面板注册结构体 ===== */
struct __lcd_panel {panel_name}_panel = {
    .name = "{panel_name}",
    .func = {
        .cfg_panel_info =  lcd_cfg_panel_info,
        .cfg_open_flow  =  lcd_open_flow,
        .cfg_close_flow =  lcd_close_flow,
        .lcd_user_defined_func = lcd_user_defined_func,
    },
};
```

### 5.3 创建面板驱动 .h 文件

```c
#ifndef _{PANEL_NAME_UPPER}_H
#define _{PANEL_NAME_UPPER}_H

#include "panels.h"

extern struct __lcd_panel {panel_name}_panel;
extern s32 bsp_disp_get_panel_info(u32 screen_id, struct disp_panel_para *info);

#endif
```

### 5.4 创建 SOC 配置 _mipi_config.c 文件

从模板 `t070s140b_mipi_config.c` 复制并修改，生成 `{panel_name}_mipi_config.c`：

```c
#include <stdint.h>
#include <hal_clk.h>
#include <hal_gpio.h>
#include "../disp/disp_sys_intf.h"

typedef uint32_t u32;
typedef int32_t  s32;

#include "disp_board_config.h"

struct property_t g_lcd0_config[] = {
    /* 基本配置 */
    { .name = "lcd_used",  .type = PROPERTY_INTGER, .v.value = 1 },
    { .name = "lcd_driver_name", .type = PROPERTY_STRING, .v.str = "{panel_name}" },
    { .name = "lcd_if", .type = PROPERTY_INTGER, .v.value = 4 },  // 4=DSI

    /* 分辨率 */
    { .name = "lcd_x", .type = PROPERTY_INTGER, .v.value = {hactive} },
    { .name = "lcd_y", .type = PROPERTY_INTGER, .v.value = {vactive} },
    { .name = "lcd_width", .type = PROPERTY_INTGER, .v.value = {物理宽度mm} },
    { .name = "lcd_height", .type = PROPERTY_INTGER, .v.value = {物理高度mm} },

    /* 像素时钟 (MHz) */
    { .name = "lcd_dclk_freq", .type = PROPERTY_INTGER, .v.value = {dclk_freq} },

    /* DSI 配置 */
    { .name = "lcd_dsi_if", .type = PROPERTY_INTGER, .v.value = LCD_DSI_IF_VIDEO_MODE },
    { .name = "lcd_dsi_lane", .type = PROPERTY_INTGER, .v.value = {lane} },
    { .name = "lcd_dsi_format", .type = PROPERTY_INTGER, .v.value = 0 },  // 0=RGB888
    { .name = "lcd_dsi_te", .type = PROPERTY_INTGER, .v.value = 0 },
    { .name = "lcd_dsi_port_num", .type = PROPERTY_INTGER, .v.value = 3 },
    { .name = "lcd_dsi_clk_rate", .type = PROPERTY_INTGER, .v.value = {dsi_clk_rate} },

    /* 水平时序 */
    { .name = "lcd_ht", .type = PROPERTY_INTGER, .v.value = {lcd_ht} },
    { .name = "lcd_hbp", .type = PROPERTY_INTGER, .v.value = {hbp} },
    { .name = "lcd_hfp", .type = PROPERTY_INTGER, .v.value = {hfp} },
    { .name = "lcd_hspw", .type = PROPERTY_INTGER, .v.value = {hsync_len} },

    /* 垂直时序 */
    { .name = "lcd_vt", .type = PROPERTY_INTGER, .v.value = {lcd_vt} },
    { .name = "lcd_vbp", .type = PROPERTY_INTGER, .v.value = {vbp} },
    { .name = "lcd_vfp", .type = PROPERTY_INTGER, .v.value = {vfp} },
    { .name = "lcd_vspw", .type = PROPERTY_INTGER, .v.value = {vsync_len} },

    { .name = "lcd_frm", .type = PROPERTY_INTGER, .v.value = 0 },

    /* 背光 PWM */
    { .name = "lcd_pwm_used", .type = PROPERTY_INTGER, .v.value = 1 },
    { .name = "lcd_pwm_ch", .type = PROPERTY_INTGER, .v.value = {pwm_ch} },
    { .name = "lcd_pwm_freq", .type = PROPERTY_INTGER, .v.value = {pwm_freq} },
    { .name = "lcd_pwm_pol", .type = PROPERTY_INTGER, .v.value = 0 },
    { .name = "lcd_pwm_max_limit", .type = PROPERTY_INTGER, .v.value = 255 },
    { .name = "lcd_backlight", .type = PROPERTY_INTGER, .v.value = {default_brightness} },
    { .name = "lcd_backlight_curve", .type = PROPERTY_INTGER, .v.value = 0 },
    { .name = "lcd_bl_en_power", .type = PROPERTY_INTGER, .v.value = 1 },

    /* GPIO: 复位引脚 (从 DTS reset-gpios 转换) */
    {
        .name = "lcd_gpio_0",
        .type = PROPERTY_GPIO,
        .v.gpio_list = {
            .gpio_name = "P{port_letter}{port_num}",
            .port = {port},          // 端口号
            .port_num = {port_num},  // 引脚号
            .mul_sel = GPIO_MUXSEL_OUT,
            .pull = GPIO_PULL_DOWN_DISABLED,
            .drv_level = GPIO_DRIVING_LEVEL3,
            .data = GPIO_DATA_LOW,
            .gpio = GPIO{port_letter}({port_num}),
        },
    },
};

// ... 后续数组长度定义等，同模板结构 ...
```

### 5.5 更新注册文件

#### panels.c — 添加面板声明

```c
// 在 panel_array[] 中添加:
#ifdef CONFIG_LCD_SUPPORT_{PANEL_NAME_UPPER}
    &{panel_name}_panel,
#endif
```

#### panels.h — 添加 extern 声明

```c
#ifdef CONFIG_LCD_SUPPORT_{PANEL_NAME_UPPER}
extern struct __lcd_panel {panel_name}_panel;
#endif
```

#### lcd/Kconfig — 添加配置项

```kconfig
config LCD_SUPPORT_{PANEL_NAME_UPPER}
    bool "LCD support {panel_name} panel"
    select FB_UPDATE
    default n
    ---help---
        If you want to support {panel_name} MIPI DSI {x}x{y} panel for display driver, select it.
```

#### soc/Kconfig — 添加板级配置

```kconfig
config {PANEL_NAME_UPPER}_MIPI
    bool "board, disp with {panel_name} MIPI"
    default n
    ---help---
        board, disp with {panel_name} MIPI {x}x{y} panel
```

#### soc/Makefile — 添加编译目标

```makefile
obj-$(CONFIG_{PANEL_NAME_UPPER}_MIPI) += {panel_name}_mipi_config.o
```

---

## 六、阶段④：代码审查（前瞻性 Bug 检查）

### 6.1 自动检查清单

每生成一个驱动文件后，必须逐项检查：

#### 6.1.1 时序一致性检查

```c
// 检查 HT 计算是否正确
// HT = hactive + hfp + hbp + hsync_len
// 例: 1200 + 20 + 30 + 10 = 1260
// VT = vactive + vfp + vbp + vsync_len
// 例: 1920 + 16 + 16 + 4 = 1956

// ⚠️ 常见错误: HT/VT 计算错误导致画面偏移或撕裂
// ✅ 修复: 逐项加总验证
```

#### 6.1.2 初始化序列完整性检查

```c
// ⚠️ 常见错误1: 遗漏 DCS 标准命令
// 初始化序列必须包含以下命令（MIPI DCS 规范）:
//   - 0x01: Software Reset (可选，但推荐)
//   - 0x11: Exit Sleep Mode (必须!)
//   - 0x29: Display On (必须!)
// 
// ⚠️ 常见错误2: 延时不足
//   - Sleep Out 后至少需要 120ms 延时
//   - Display On 后至少需要 50ms 延时
//   - 参考 DTS 中的 delay 值，不要降低
//
// ✅ 修复: 检查序列首尾，确保有 Sleep Out 和 Display On
```

#### 6.1.3 退出序列检查

```c
// ⚠️ 常见错误: 退出时序错误导致关机闪烁
// 正确顺序: Display Off (0x28) → Sleep In (0x10)
// Sleep In 后至少需要 120ms 延时
// ✅ 修复: 确保 exit 序列包含 Display Off 和 Sleep In
```

#### 6.1.4 DSI 配置一致性检查

```c
// ⚠️ 常见错误: lane 数不匹配
// 检查:
//   1. dsi,lanes 与 lcd_dsi_lane 值一致
//   2. DSI 时钟率与 lane 数匹配（4-lane 需要更高时钟）
//   3. 格式与数据位宽匹配（RGB888 → 24bpp）
//
// ⚠️ 常见错误: DSI 时钟率计算错误
// DSI_CLK ≈ dclk_freq × 8 / lane_count
// 例: 146MHz × 8 / 4 = 292MHz
// 这个值范围应在 150-500 之间
```

#### 6.1.5 GPIO 配置检查

```c
// ⚠️ 常见错误: GPIO 端口号转换错误
// Linux RK_PB5 → Vela: port=1, port_num=13 (8+5)
// 验证转换表:
//   RK_PA0 = port=0, num=0
//   RK_PD19 = port=3, num=43 (24+19)
//
// ⚠️ 常见错误: GPIO 极性不匹配
// GPIO_ACTIVE_LOW → 复位时为低
// GPIO_ACTIVE_HIGH → 使能时为高
```

#### 6.1.6 内存安全检查

```c
// ⚠️ 常见错误: 未释放 malloc 的内存
// 在 LCD_panel_init 中如果使用 disp_sys_malloc，必须在退出前 disp_sys_free
// 模板 t070s140b.c 未使用 malloc，推荐保持一致
//
// ✅ 修复: 确认所有函数中 malloc/free 成对出现
```

#### 6.1.7 注册完整性检查

```c
// ⚠️ 常见错误: 驱动注册不完整
// 必须检查以下 6 处是否都已添加:
//  [✅] panels.c panel_array[] 中
//  [✅] panels.h extern 声明中
//  [✅] lcd/Kconfig 配置项
//  [✅] soc/Kconfig 板级配置
//  [✅] soc/Makefile 编译目标
//  [✅] defconfig 中使能
```

#### 6.1.8 命名一致性检查

```c
// ⚠️ 常见错误: 名称不一致
// 检查以下名称在所有文件中完全一致:
//   1. {panel_name} — 文件名、面板名、驱动名
//   2. {PANEL_NAME_UPPER} — Kconfig 宏、条件编译宏
//   3. {panel_name}_panel — 全局结构体变量名
//   4. {panel_name}_mipi_config — SOC 配置文件名
```

### 6.2 编译验证

```bash
# 1. 检查 Kconfig 语法
python3 -c "import re; open('/dev/null')"  # 简单语法检查
# 实际应在 menuconfig 中验证

# 2. 检查 C 语法
# 暂不编译，但可做静态分析
gcc -fsyntax-only -I${PANEL_DIR} -I${SOC_DIR} ${PANEL_DIR}/{panel_name}.c 2>&1 || true

# 3. 检查所有引用是否一致
grep -r "{panel_name}" vendor/allwinnertech/chips/r528/drivers/rtos-hal/hal/source/disp2/
```

---

## 七、完整工作流示例（以 rk3568-firefly-roc-pc-se-8fhd.dts 为例）

### 步骤 1：解析 DTS 提取参数

```
输入文件: rk3568-firefly-roc-pc-se-8fhd.dts

提取结果:
  hactive=1200, vactive=1920
  clock-frequency=146000000 → dclk_freq=146MHz
  hfp=20, hbp=30, hsync=10 → ht=1260
  vfp=16, vbp=16, vsync=4 → vt=1956
  dsi_lanes=4, dsi_format=RGB888
  dsi_clk_rate = 146*8/4 = 292MHz
  reset-gpio: <&gpio3 RK_PB5 GPIO_ACTIVE_LOW> → port=3, port_num=13
  PWM: pwm4, period=25000ns → freq=40000Hz
  panel-init-sequence: 16 条命令（从 0x05/0x15 解析）
  panel-exit-sequence: 2 条命令（Display Off + Sleep In）
  面板名: "rk3568_firefly_8fhd" (从 panel 节点名 "dsi0_panel" 或 compatible 提取)
```

### 步骤 2：生成 MD 文档

```
生成文件: vela/panel_info_rk3568_firefly_8fhd.md
```

### 步骤 3：创建驱动文件

```
生成文件列表:
  disp/lcd/rk3568_firefly_8fhd.c
  disp/lcd/rk3568_firefly_8fhd.h
  soc/rk3568_firefly_8fhd_mipi_config.c
```

### 步骤 4：更新注册文件

```
修改文件列表:
  disp/lcd/panels.c        → 添加 panel_array 条目
  disp/lcd/panels.h        → 添加 extern 声明
  disp/lcd/Kconfig         → 添加 LCD_SUPPORT_RK3568_FIREFLY_8FHD
  soc/Kconfig              → 添加 RK3568_FIREFLY_8FHD_MIPI
  soc/Makefile             → 添加编译目标
```

### 步骤 5：代码审查

```
检查项:
  ✅ 时序 HT=1260, VT=1956 计算正确
  ✅ 初始化序列包含 Sleep Out(0x11) 和 Display On(0x29)
  ✅ 退出序列包含 Display Off(0x28) 和 Sleep In(0x10)
  ✅ DSI 4-lane, 时钟 292MHz 在合理范围
  ✅ GPIO 转换正确: RK_PB5 → port=1, port_num=13
  ✅ 所有 6 处注册完整
  ✅ 命名一致
```

---

## 八、常见问题与解决方案

### 8.1 屏幕不亮

| 可能原因 | 检查方法 | 修复 |
|---------|---------|------|
| 初始化序列错误 | 确认 DTS 字节流解析正确 | 重新解析 panel-init-sequence |
| 延时不足 | Sleep Out 后至少 120ms | 增加延时 |
| GPIO 极性反 | 确认 ACTIVE_LOW/HIGH | 反转 GPIO 值 |
| 电源未使能 | 检查 AXP2101 电源 ID | 配置正确的电源 |
| DSI 时钟不对 | 检查 lcd_dsi_clk_rate | 重新计算 |

### 8.2 画面偏移/撕裂

| 可能原因 | 检查方法 | 修复 |
|---------|---------|------|
| HT/VT 计算错误 | 重新加总 | 确认 ht = hactive + hfp + hbp + hsync |
| 同步极性错误 | 检查 hsync-active/vsync-active | 设置 lcd_hv_sync_polarity |
| 像素时钟不对 | 检查 dclk_freq | 确认 MHz 值正确 |

### 8.3 编译错误

| 可能原因 | 检查方法 | 修复 |
|---------|---------|------|
| 缺少头文件 | 检查 include | 添加 `#include "panels.h"` |
| 未定义符号 | 检查 extern | 确认 panels.h 中有 extern 声明 |
| 重复定义 | 检查名称 | 确认面板名唯一 |

---

## 九、附录

### 9.1 DTS 字节流指令速查表

| Type 字节 | 名称 | 含义 | Vela API |
|----------|------|------|---------|
| 0x05 | MIPI_DSI_DCS_LONG_WRITE | DCS 长写（1+ param） | `sunxi_lcd_dsi_dcs_write_*para()` |
| 0x15 | MIPI_DSI_GENERIC_SHORT_WRITE_2_PARAM | 通用短写（2 param） | `sunxi_lcd_dsi_gen_write_1para()` |
| 0x39 | MIPI_DSI_GENERIC_LONG_WRITE | 通用长写 | `sunxi_lcd_dsi_gen_write_*para()` |

### 9.2 Vela 显示接口枚举

```c
// lcd_if (显示接口类型)
#define LCD_IF_CPU      0
#define LCD_IF_LVDS     3
#define LCD_IF_DSI      4
#define LCD_IF_RGB      7

// lcd_dsi_if (DSI 模式)
#define LCD_DSI_IF_VIDEO_MODE  0
#define LCD_DSI_IF_CMD_MODE    1
#define LCD_DSI_IF_BURST_MODE  2

// lcd_dsi_lane (DSI Lane 数)
#define LCD_DSI_LANE_1  1
#define LCD_DSI_LANE_2  2
#define LCD_DSI_LANE_4  4

// lcd_dsi_format (像素格式)
#define LCD_DSI_FORMAT_RGB888  0
#define LCD_DSI_FORMAT_RGB666  1
```

### 9.3 AXP2101 电源 ID（R528 平台）

```c
#define AXP2101_ID_DCDC1   1
#define AXP2101_ID_DCDC2   2
#define AXP2101_ID_DCDC3   3
#define AXP2101_ID_DCDC4   4
#define AXP2101_ID_ALDO1   5
#define AXP2101_ID_ALDO2   6
#define AXP2101_ID_ALDO3   7
#define AXP2101_ID_BLDO1   8
#define AXP2101_ID_BLDO2   9
#define AXP2101_ID_BLDO3   10
#define AXP2101_ID_BLDO4   11
```

### 9.4 Vela GPIO 端口宏

```c
// 端口定义
#define GPIOA(n)  ((n) + 0)    // 端口 0
#define GPIOB(n)  ((n) + 8)    // 端口 1
#define GPIOC(n)  ((n) + 16)   // 端口 2
#define GPIOD(n)  ((n) + 24)   // 端口 3
#define GPIOE(n)  ((n) + 32)   // 端口 4
#define GPIOF(n)  ((n) + 40)   // 端口 5
#define GPIOG(n)  ((n) + 48)   // 端口 6
#define GPIOH(n)  ((n) + 56)   // 端口 7
#define GPIOI(n)  ((n) + 64)   // 端口 8
```

---

## 十、自检清单

每次移植完成后，按以下清单逐项确认：

```
□ ① DTS 解析完整（分辨率、时序、初始化序列、DSI、GPIO、PWM）
□ ② MD 文档生成（包含所有参数和转换说明）
□ ③ 面板驱动 .c 文件创建（初始化序列正确翻译）
□ ④ 面板驱动 .h 文件创建
□ ⑤ SOC 配置 _mipi_config.c 创建（时序、DSI、GPIO、PWM 正确）
□ ⑥ panels.c 更新（添加 panel_array 条目）
□ ⑦ panels.h 更新（添加 extern 声明）
□ ⑧ lcd/Kconfig 更新（添加配置项）
□ ⑨ soc/Kconfig 更新（添加板级配置）
□ ⑩ soc/Makefile 更新（添加编译目标）
□ ⑪ 时序计算验证（HT = hactive + hfp + hbp + hsync）
□ ⑫ 初始化序列完整性检查（包含 Sleep Out + Display On）
□ ⑬ GPIO 转换正确性检查
□ ⑭ DSI 时钟率合理性检查
□ ⑮ 命名一致性检查（文件名、结构体、Kconfig 宏）
□ ⑯ 无内存泄漏检查
```