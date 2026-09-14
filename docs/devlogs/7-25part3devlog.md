# R528/OpenVela SDK 能力手册 — 给 AI 助手的工具箱

> 本文档列举了在这个 SDK、芯片、驱动架构下可用的**调试能力**和**工具方法**。下一任 AI 在处理 GT9271 I2C 问题前应先掌握这些能力。

---

## 能力 1: 查找 CCU 寄存器定义

### 时钟门控寄存器

TWI0 的时钟门控在 CCU（Clock Control Unit）中。SDK 中 CCU 寄存器的定义通常在：

```
chips/r528/drivers/rtos-hal/hal/source/ccu/
chips/r528/drivers/rtos-hal/include/hal/hal_prcm.h
```

**搜索方法：**
```bash
grep -rn "CLK_BUS_I2C0\|I2C0.*CLK\|twi0.*clk\|i2c0.*clk" --include="*.h" --include="*.c" \
  vendor/allwinnertech/chips/r528/drivers/rtos-hal/
```

**R528 CCU 典型基址：** `0x02001000`

时钟门控寄存器通常命名格式：`CLK_BUS_xxx` 或 `xxx_GATING`。每个 bit 对应一个外设。

### 复位控制寄存器

```bash
grep -rn "RST_BUS_I2C0\|I2C0.*RST\|twi0.*rst\|i2c0.*reset" --include="*.h" --include="*.c" \
  vendor/allwinnertech/chips/r528/drivers/rtos-hal/
```

复位控制寄存器通常命名格式：`RST_BUS_xxx`。写 1 assert 复位，写 0 deassert。

### 寄存器读操作

在 `hal_twi.c` 或 `drv_twi.c` 中，使用 `readl(addr)` 读 32 位寄存器，`writel(val, addr)` 写。

```c
// 示例：读 CCU 寄存器
#define CCU_BASE        0x02001000  // R528 CCU 物理基址（虚拟地址需查映射）
// 或者用已有的映射：
// #include "chip.h"  → 可能有 CCU_VADDR 宏

uint32_t val = readl(CCU_BASE + offset);
```

---

## 能力 2: NuttX 中访问 MMIO 寄存器

### 虚拟地址映射

R528 的外设地址映射在 `r528_memorymap.h` 中定义：

```c
// chips/r528/hardware/r528_memorymap.h
#define R528_TWI0_VADDR     (R528_PERIPH_VSECTION + R528_TWI0_OFFSET)
#define R528_TWI0_PADDR     (R528_SP1_PSECTION + R528_TWI0_OFFSET)
```

`drv_twi.c` 已包含 `"chip.h"`，所以可以直接使用 `R528_TWI0_VADDR`。

### 如果没有虚拟地址宏

可以直接用 `r528_i2c_initialize()` 中建立的映射。或者在 `drv_twi.c` 中通过 `hal_twi_address[port]` 获取（但该数组是 `static` 在 `hal_twi.c` 中，不可直接访问）。

**变通方法：** 从 `twi_sun8iw20.h` 获取物理地址，用 `map_region()` 或 `MmMapIoRegion()`（不同平台 API 不同）。

---

## 能力 3: GPIO 操作

### 在触摸驱动中读 GPIO 电平

```c
#include "hal_gpio.h"
gpio_data_t data;
hal_gpio_get_data(GPIOB(2), &data);  // 读 PB2 (SDA) 电平
printf("SDA level: %d\n", data);
```

### 验证引脚复用功能

```c
gpio_muxsel_t func;
hal_gpio_pinmux_get_function(GPIOB(2), &func);
printf("PB2 mux = %d (expect %d for TWI0)\n", func, 4);
```

### 强制驱动 I2C 总线进行恢复

```c
// 把 PB2/PB3 切到 GPIO 输出，手动产生 9 个时钟脉冲
hal_gpio_pinmux_set_function(GPIOB(3), GPIO_MUXSEL_OUT);  // SCL → GPIO
hal_gpio_pinmux_set_function(GPIOB(2), GPIO_MUXSEL_OUT);  // SDA → GPIO
for (int i = 0; i < 9; i++) {
    hal_gpio_set_data(GPIOB(3), 0); up_udelay(5);
    hal_gpio_set_data(GPIOB(3), 1); up_udelay(5);
}
// 切回 TWI 功能
hal_gpio_pinmux_set_function(GPIOB(3), 4);
hal_gpio_pinmux_set_function(GPIOB(2), 4);
```

---

## 能力 4: NuttX 日志和调试

### 日志级别

| 宏 | 说明 | 是否默认输出 |
|----|------|------------|
| `printf()` | 标准输出 | ✅ 总是输出 |
| `syslog(LOG_ERR, ...)` | 错误日志 | 取决于配置 |
| `TWI_ERR()` | TWI 错误（内部用 `syslog`） | 可能被过滤 |
| `iinfo()` | I2C 信息 | 需 `CONFIG_DEBUG_I2C_INFO` |
| `TWI_INFO()` | TWI 内部 info | 需对应 DEBUG 配置 |

如果 HAL 内部的 `TWI_ERR("START can't sendout")` 没有在控制台出现，说明 TWI 的日志被过滤了。你可以临时把 `TWI_ERR` 替换为 `printf` 来强制输出。

### 查找 NuttX 配置项

```bash
grep "CONFIG_DEBUG\|CONFIG_I2C\|CONFIG_TWI" \
  vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh/defconfig
```

---

## 能力 5: 查找中断号和中断控制器配置

### IRQ 号

```c
// chips/r528/include/r528_irq.h
#define R528_IRQ_TWI0        41
```

### GIC 配置

```c
// chips/r528/internal_inc/interrupt.h
#define GIC_SRC_TWI0        GIC_SRC_SPI(6)  // SPI #38
```

### 检查中断是否使能

在 `twi_start()` 之前检查 GIC 中 TWI0 的中断使能位：

```c
// GICD_ISENABLER 基址（需确认具体地址）
uint32_t gicd_base = 0x03001000;  // 典型 GICD 基址
int irq = 41;  // TWI0 IRQ
uint32_t reg = readl(gicd_base + 0x100 + (irq / 32) * 4);
printf("GIC ISENABLER[%d] = 0x%08X (bit%d=%d)\n",
       irq / 32, reg, irq % 32, (reg >> (irq % 32)) & 1);
```

---

## 能力 6: 检查 TWI 控制器内部寄存器（TWI_STAT_REG 状态码大全）

TWI 控制器状态机状态码（`TWI_STAT_REG` 偏移 0x10），在 `hal_twi_core_process()` 的 switch 语句中定义：

| 状态码 | 含义 | 处理 |
|--------|------|------|
| `0xF8` | 空闲（Idle） | 无操作 |
| `0x08` | START 已发送 | 发 SLA+W |
| `0x10` | 重复 START 已发送 | 发 SLA+W/R |
| `0x18` | SLA+W 已发，收到 ACK | 发数据字节 |
| `0x20` | SLA+W 已发，收到 NACK | 错误 |
| `0x28` | 数据字节已发，收到 ACK | 发下一字节 |
| `0x30` | 数据字节已发，收到 NACK | 错误 |
| `0x38` | 仲裁丢失 | 错误 |
| `0x40` | SLA+R 已发，收到 ACK | 准备接收数据 |
| `0x48` | SLA+R 已发，收到 NACK | 错误 |
| `0x50` | 数据已接收，ACK 已发 | 收下一字节 |
| `0x58` | 数据已接收，NACK 已发 | 最后一字节 |
| `0x00` | 总线错误 | 错误 |

**诊断中 `stat=0xF8` 的含义：** TWI 状态机处于空闲。正常情况 `twi_start()` 设置 STA 位后，硬件应生成 START 条件并将状态推进到 `0x08`。如果状态一直停留 `0xF8`，说明硬件从未尝试生成 START。

---

## 能力 7: 查看系统初始化顺序

板级初始化在 `r528_boot.c` 中定义。顺序如下：

```c
// chips/r528/r528_boot.c
r528_i2c_initialize("/dev/i2c0", 0);    // TWI0 初始化
// ... 其他外设 ...
gt911_register("/dev/input0", i2c_bus0); // 触摸注册（内含 init + worker 调度）
// ... 更多外设初始化 ...
// LCD 初始化（MIPI DSI + BOE 面板）
// 传感器初始化（SHTC3, sgp30 等）
```

**重要：** 触摸驱动在 LCD 初始化之前注册。触摸 worker 在 HPWORK 上调度运行（16ms 后首次执行），可能与 LCD 初始化并发。

---

## 能力 8: 用逻辑分析仪抓 I2C 波形（如果可用）

如果用户有逻辑分析仪，这是最高效的定位方法。

请用户抓取以下场景：
1. 上电后 GT9271 reset 时序（RST、INT 引脚电平变化）
2. 第一次 I2C 传输（读 0x8140 product_id）的完整波形
3. 后续 I2C 传输（读 0x814E）的完整波形——尤其是 **START 条件是否出现**

**关键观察点：**
- SDA/SCL 是否都有上拉到 3.3V（空闲时 HIGH）
- 第一次传输是否有完整的 START → Address+W → ACK → Data → STOP
- 第二次传输是否根本没有 START（如果一直空闲则验证了 `twi_start()` 的诊断结果）
- 第二次传输如果 START 了但被 NACK，GT9271 可能有问题

---

## 能力 9: 参考 Linux 内核的 GT9271 驱动

Linux 主线内核 `drivers/input/touchscreen/goodix.c` 是最权威的参考实现。关键差异可以对照：

### Linux probe 流程

```
goodix_probe()
  ├── goodix_get_gpio_config()
  ├── goodix_power_on() + goodix_reset()
  ├── goodix_i2c_test()          // 读 0x8140 验证通信
  ├── goodix_read_version()      // 读 0x8144 获取版本
  ├── goodix_get_chip_data()     // 根据 product_id 选择参数
  ├── goodix_configure_dev()
  │    ├── goodix_read_config()  // 读 0x8047 (186字节)
  │    ├── input_set_abs_params()
  │    └── input_setup_polling() // 轮询或 IRQ
  └── goodix_ts_input_dev_setup()
```

### 坐标读取流程

```c
// Linux 读 0x814E 坐标数据
goodix_i2c_read(client, 0x814E, data, 10);  // 1 header + 8 contact + 1 keycode
if (data[0] & 0x80) {  // buffer_ready
    touch_num = data[0] & 0x0F;
    if (touch_num > 1)
        goodix_i2c_read(client, 0x814E + 10, data + 10, 8 * (touch_num - 1));
    goodix_i2c_write_u8(client, 0x814E, 0);  // 清除状态
}
```

---

## 能力 10: 在不烧录固件的情况下验证假设（如果可以）

### 通过 NSH 命令行测试 I2C

如果系统启动后能进入 NSH shell，可以用 i2c 工具直接操作：

```bash
# 列出 I2C 总线
i2c bus
# 读 TWI0 从设备 0x5D，偏移 0x8140，读 4 字节
i2c read /dev/i2c0 0x5D 0x8140 4
# 写 TWI0 从设备 0x5D，偏移 0x8040，写 1 字节 0x00
i2c write /dev/i2c0 0x5D 0x8040 1 0x00
```

### NuttX 的 i2c 工具

```bash
# 如果 i2c 工具不在默认编译中，可以在 defconfig 中启用：
# CONFIG_I2C_TOOL=y
```

---

## 能力速查表

| 你想做什么 | 怎么做 |
|-----------|--------|
| 读 CCU 时钟门控寄存器 | 在 CCU 头文件中搜 `CLK_BUS_I2C0` |
| 读 CCU 复位控制寄存器 | 在 CCU 头文件中搜 `RST_BUS_I2C0` |
| 读 TWI 状态寄存器 | `readl(base_addr + TWI_STAT_REG)` (0x10) |
| 读 TWI 控制寄存器 | `readl(base_addr + TWI_CTL_REG)` (0x0C) |
| 读 TWI 软复位寄存器 | `readl(base_addr + TWI_SRST_REG)` (0x18) |
| 读 GPIO 电平 | `hal_gpio_get_data(GPIOB(n), &data)` |
| 检查 GPIO 复用 | `hal_gpio_pinmux_get_function(GPIOB(n), &func)` |
| 强制输出日志 | 用 `printf()` 替换 `TWI_ERR()` / `syslog()` |
| 检查中断是否使能 | 读 GICD_ISENABLER 寄存器 |
| 查阅 CCU 定义 | 搜 `chips/r528/` 下所有 `*ccu*` `*clk*` `*prcm*` 文件 |
| 查阅 I2C 工具 | `i2c read/write` NSH 命令 |
| 查阅 Linux 参考 | `drivers/input/touchscreen/goodix.c` |

---

> 下一任 AI：在修改代码前先掌握以上能力。特别是**能力 6**（STAT 状态码）和**能力 1**（CCU 寄存器定位），你需要在 `twi_start()` 中同时读取 TWI 内部寄存器和 CCU 时钟/复位寄存器，才能真正定位 `twi_start()` 失败的原因。

---

## 能力 11: 已验证的 CCU/GIC 寄存器地址（第 5 轮确认 + 第 6 轮修正）

| 寄存器 | 物理地址 | 说明 |
|--------|---------|------|
| CCU 基址 | `0x02001000` | `ccu-sun8iw20.h`：`SUNXI_CCU_BASE` |
| TWI0 时钟门控 | `0x0200191C` | offset `0x91C`, BIT(0)。1=使能 |
| TWI0 复位控制 | `0x0200191C` | offset `0x91C`, BIT(16)。**低电平有效**（`ccu_reset_assert` 写 0=assert，`ccu_reset_deassert` 写 1=deassert） |
| GICD 物理地址 | `0x03001000` | ARM Cortex-A7 GIC 分发器物理基址 |
| GICD 虚拟地址（NuttX 实际使用） | `0x03021000` | `MPCORE_ICD_VBASE = CHIP_MPCORE_VBASE(R528_GIC_VADDR=0x03020000) + MPCORE_ICD_OFFSET(0x1000)`。**诊断必须用此地址而非物理地址！** |
| ISENABLER[1] 地址 | `0x03021104` | IRQ32-63 使能位。TWI0 IRQ41 在 bit9 |
| ICPENDR[1] 地址 | `0x03021284` | IRQ32-63 挂起位 |
| TWI0 IRQ | 41 | GIC SPI #38。ISENABLER 在 `GICD_VADDR+0x104` BIT(9) |
| TWI0 基址 | `0x02502000` | `twi_sun8iw20.h`：`SUNXI_TWI0_PBASE` |

### ⚠️ 诊断地址陷阱

**永远不要直接用 `0x03001000` 作为 GICD 基址来读 ISENABLER！** 这个地址是 CCU（`SUNXI_CCU_BASE`）！GIC 分发器在 NuttX 中通过虚拟地址 `0x03021000` 访问：

```
MPCORE_ICD_VBASE = R528_GIC_VADDR + MPCORE_ICD_OFFSET
                 = 0x03020000 + 0x1000
                 = 0x03021000
```

ISENABLER[1] 在 `0x03021000 + 0x100 + 4 = 0x03021104`。

## 能力 12: 诊断代码位置注意事项（重要！）

**`hal_twi.c` 修改可能不生效。** 这个文件编译到 `librtos-hal.a` 静态库中，可能被构建系统缓存。即使源文件已修改，`m nsh` 增量编译可能不会重编这个库。

**可靠的做法：**
- ✅ 在 `drv_twi.c`（`chips/r528/drv/twi/drv_twi.c`）中加诊断——NuttX 内核文件，每次 `m nsh` 必然重编
- ✅ 如果必须改 `hal_twi.c`，用 `m nsh c` clean build 再 `m nsh`
- ✅ 验证诊断是否生效：编译后 `strings 镜像文件 | grep -c '你的字符串'`

**已验证能输出的诊断位置：** `drv_twi.c` 的 `r528_transfer()` 错误路径（`hal_twi_xfer()` 返回失败后）。此处已有 `printf("[TWI] XFER FAIL ...")` 输出，可扩展现有诊断块。

## 能力 13: 强制中断使能（如果 GIC 诊断显示中断被禁）

```c
// 在调用 hal_twi_xfer 之前
#include "interrupt.h"  // hal_enable_irq
hal_enable_irq(41);  // 强制重新使能 TWI0 中断
```
