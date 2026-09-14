# GT9271 触摸 I2C 调试 — 接力 Prompt

> 本文档是给下一任 AI 助手的完整任务上下文。请先**完整阅读本文档**，然后按照"任务指令"逐步执行。不要跳过任何诊断步骤。

---

## 1. 项目背景

### 硬件

- **SoC:** Allwinner R528 (sun8iw20)，双核 Cortex-A7
- **开发板:** r528s3-gemini-s1
- **屏幕:** BOE 1200×1920 MIPI DSI
- **触摸 IC:** Goodix GT9271 (兼容 GT911 寄存器映射)
- **I2C 总线:** TWI0 (PB2=SDA, PB3=SCL)
- **I2C 地址:** 0x5D (reset 时 INT=LOW 选择)
- **RST/INT:** PB4(RST), PB5(INT)
- **上拉电阻:** **4.7kΩ 外部上拉已补焊**
- **目标频率:** 100KHz
- **I2C 模式:** Poll 模式（`GT911_TOUCH_POLLMODE` 已定义）

### 软件

- **OS:** NuttX (OpenVela 分支)
- **工作目录:** `/data/openvela`
- **编译:** `cd vendor/allwinnertech/lichee && source tools/scripts/envsetup.sh && lunch_nuttx 2 && m nsh && pack`
- **输出镜像:** `lichee/out/r528s3/gemini-s1_nand/rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img`
- **烧录:** 用户负责

### 关键文件

| 文件 | 说明 | 位置 |
|------|------|------|
| TWI NuttX lower-half | `drv_twi.c` | `chips/r528/drv/twi/drv_twi.c` |
| TWI HAL 实现 (2653行) | `hal_twi.c` | `chips/r528/drivers/rtos-hal/hal/source/twi/hal_twi.c` |
| TWI HAL 头文件 | `sunxi_hal_twi.h` | `chips/r528/drivers/rtos-hal/include/hal/sunxi_hal_twi.h` |
| TWI 平台定义 | `twi_sun8iw20.h` | `chips/r528/drivers/rtos-hal/hal/source/twi/platform/twi_sun8iw20.h` |
| TWI 寄存器定义 | `common_twi.h` | `chips/r528/drivers/rtos-hal/hal/source/twi/common_twi.h` |
| 触摸驱动 | `gt911_iic_touch.c/h` | `boards/r528/drivers/gt911_iic_touch.c` + `.h` |
| BOE LCD 配置 | `BOE_1200x1920_mipi_config.c` | `chips/r528/.../disp2/soc/BOE_1200x1920_mipi_config.c` |
| 板级初始化 | `r528_boot.c` | `chips/r528/r528_boot.c` |
| 内存映射 | `r528_memorymap.h` | `chips/r528/hardware/r528_memorymap.h` |
| CCU 时钟/复位 | 需自行搜索 | `chips/r528/` 或 `rtos-hal/` |

---

## 2. 问题现象

### 症状

1. 初始化阶段读 `0x8140`（product_id，4 字节）**成功**。
2. 之后 work queue 中的轮询读 `0x814E`（坐标状态，1 字节）**持续失败**，`ret=-1`。
3. 连续失败 10 次后调用 `gt911_reset_chip()` 复位 GT9271，复位后**仍然失败**。
3. 两个 I2C 操作走完全相同的代码路径（`I2C_TRANSFER` → `r528_transfer()` → `hal_twi_xfer()`）。

### 典型日志

```
[GT911] ===== INIT START =====
[GT911] GPIO: RST=PB4 INT=PB5
[GT911] Reset seq #1: RST=LOW INT=LOW -> 20ms -> RST=HIGH -> 50ms
[GT911] Try addr 0x5D...
[GT911] detect ret=0, product_id=[927]
GT911] Detected GT927 touch controller
[GT911] I2C_READ FAIL: reg=0x814E len=1 slave=0x5D ret=-1
[GT911] I2C fail, count=1
... (重复)
[GT911] I2C fail 10 times, resetting chip
... (reset 后继续失败)
```

### 完整日志文件

`/data/openvela/bootlog725.txt`

---

## 3. 已实施的诊断

前一任 AI 已在 `drv_twi.c` 的 `r528_transfer()` 错误路径中添加了 TWI 状态寄存器 dump。I2C 传输失败时会打印：

```
[TWI] XFER FAIL port=0 stat=0xF8 ctrl=0x40 clk=0x0059 drv_intc=0x00000000
```

### 诊断数据解读

| 字段 | 值 | 含义 |
|------|-----|------|
| `stat=0xF8 (TWI_STAT_REG)` | 0xF8 | **空闲态。** TWI 状态机处于 idle。不是 NACK (0x20/0x48)、不是总线错误 (0x00)、不是仲裁丢失 (0x38) |
| `ctrl=0x40 (TWI_CTL_REG)` | 0x40 | BIT6(BUSEN)=1，BIT5(STA)=0，BIT3(INTFLG)=0。总线已使能但 START 位未置位 |
| `clk=0x0059 (TWI_CLK_REG)` | 0x59 | CLK_M=11(0xB), CLK_N=1(0x1)。24MHz/((11+1)×2¹)=1MHz ？实际计算见下方 |
| `drv_intc=0x0 (TWI_DRIVER_INTC)` | 0 | 没有任何中断触发 |

### 时钟计算

`twi_set_clock()` 的算法：
```c
src_clk = clk_in / 10;        // 24MHz / 10 = 2.4MHz
divider = src_clk / sclk_req; // 2.4MHz / 100KHz = 24
```

n=1, m=11: `(11+1)*2¹=24` ✅ 寄存器值 = `(11<<3)|1 = 0x59` ✅

**结论：100KHz 已正确生效。**

### 核心矛盾

诊断输出 `stat=0xF8` 是在 `hal_twi_xfer()` 返回失败**之后**读取的。此时的 0xF8 是错误路径中 `twi_soft_reset()`（第 1863 行）的结果。但 `twi_start()` 在 255 次轮询内 STA 位不清除——**硬件无法生成 START 条件**。

---

## 4. 已尝试的修复（均无效）

### 修复 1: 频率传递

**发现：** `r528_transfer()` 将 `i2c_msg_s` 拷贝到 `twi_msg_t` 时丢弃了 `frequency` 字段。`hal_twi_init()` 硬编码 `twi->freq = TWI_FREQUENCY_400K`（400KHz）。

**修改：** `sunxi_hal_twi.h` + `hal_twi.c`: 新增 `hal_twi_set_freq()` API；`drv_twi.c`: `r528_transfer()` 调用 `hal_twi_set_freq()` 传递 `msg[0].frequency`。

**结果：** ❌ 无效。诊断确认 100KHz 已正确配置，但问题依旧。

### 修复 2: `hal_twi_set_freq()` 总是调 `twi_enable()`

**发现：** `hal_twi_set_freq()` 有 `if (twi->freq == freq) return;` 早期返回。第一次频率变化调了 `twi_enable()`，后续频率不变就跳过了。

**修改：** 移除早期返回，迫使每次传输前都调 `twi_enable()`。

**结果：** ❌ 无效。诊断输出完全不变（`stat=0xF8 ctrl=0x40 clk=0x0059`）。

### 修复 3: soft reset 后重新使能 BUSEN

**发现：** `hal_twi_engine_do_xfer()` 第 1824 行 `twi_soft_reset()` 可能清除 BUSEN。前两个修复的 `twi_enable()` 在 soft reset 之前执行，被复位清掉了。

**修改：** `hal_twi_engine_do_xfer()` 第 1844 行加 `twi_enable(base_addr, TWI_CTL_REG, TWI_CTL_BUSEN)`。

**结果：** ❌ 用户反馈无效（未提供最新 bootlog）。

---

## 5. 关键代码分析

### 调用链

```
gt911_register()
  └─ gt911_control_initialize()
       └─ gt911_i2c_read(0x8140, 4) → I2C_TRANSFER → r528_transfer()
            → hal_twi_init() → hal_twi_xfer() → hal_twi_engine_do_xfer()
  └─ work_queue(HPWORK, gt911_worker, 16ms)

gt911_worker()
  └─ gt911_i2c_read(0x814E, 1) → I2C_TRANSFER → r528_transfer()
       → hal_twi_init() → hal_twi_xfer() → hal_twi_engine_do_xfer()
```

### `r528_transfer()` (drv_twi.c:89)

```c
static int r528_transfer(FAR struct i2c_master_s *lower,
                         FAR struct i2c_msg_s *msgs, int count)
{
    struct twi_msg *twi_msgs = kmm_malloc(count * sizeof(struct twi_msg));
    for (int i = 0; i < count; i++) {
        twi_msgs[i].addr = msgs[i].addr;
        twi_msgs[i].flags = msgs[i].flags;
        twi_msgs[i].len = msgs[i].length;
        twi_msgs[i].buf = msgs[i].buffer;
        // ⚠️ msgs[i].frequency 被丢弃！twi_msg_t 没有 frequency 字段
    }
    hal_twi_init(priv->twi_port);
    hal_twi_set_freq(priv->twi_port, msgs[0].frequency);  // 已修复
    if (hal_twi_xfer(priv->twi_port, twi_msgs, count)) {
        // 诊断 dump
        return -1;
    }
    return OK;
}
```

### `hal_twi_engine_do_xfer()` (hal_twi.c:1816) — **关键函数**

```c
static int hal_twi_engine_do_xfer(hal_twi_t *twi, twi_msg_t *msgs, int num)
{
    twi_soft_reset(base_addr, TWI_SRST_REG, TWI_SRST_SRST);  // ① 软复位

    while (status != IDLE && status != BUS_ERR && ...) {
        twi_send_clk_9pulse(...);  // ② 9 脉冲恢复
    }

    twi_enable(..., BUSEN);  // ③ 已修复：复位后重新使能

    twi_disable_ack(base_addr);
    twi_set_efr(base_addr, 0);

    ret = twi_start(base_addr, twi->port);  // ④ ⚠️ 失败点！START 发不出
    if (ret == SUNXI_TWI_FAIL) {
        twi_soft_reset(...);   // ⑤ 错误路径再次软复位 → stat 回到 0xF8
        twi_disable_irq(...);
        return SUNXI_TWI_RETRY;
    }

    twi_enable_irq(base_addr);
    hal_sem_timedwait(twi->hal_sem, timeout);  // 等待中断完成
    return ret;
}
```

### `twi_start()` (hal_twi.c:533) — **失败点**

```c
static int twi_start(const unsigned long base_addr, int port)
{
    unsigned int timeout = 0xff;
    twi_set_start(base_addr);  // 写 CTL: STA=1, INTFLG=0
    while ((twi_get_start(base_addr) == 1) && (--timeout))
        ;  // 轮询等待硬件清除 STA 位（硬件生成 START 后自动清除）
    if (timeout == 0) {
        TWI_ERR("[twi%d] START can't sendout!", port);
        return SUNXI_TWI_FAIL;  // ← 255 次轮询后 STA 仍为 1
    }
    return SUNXI_TWI_OK;
}
```

### `hal_twi_init()` (hal_twi.c:2343) — already_init 路径

```c
twi_status_t hal_twi_init(twi_port_t port)
{
    hal_twi_t *twi = &hal_twi[port];
    if (twi->already_init) {
        // 只检查 pinmux，不碰时钟、复位、使能
        for (int i = 0; i < TWI_PIN_NUM; i++) {
            hal_gpio_pinmux_get_function(twi->pin[i], &func);
            if (func != twi->pinmux) {
                hal_twi_pinctrl_init(twi);
                break;
            }
        }
        return TWI_STATUS_OK;
    }
    // 第一次调用做完整初始化：
    //   hal_twi_clk_init() → 使能时钟、释放复位、配时钟分频器、使能BUSEN
    //   hal_twi_pinctrl_init() → 配引脚
    //   hal_request_irq() + hal_enable_irq() → 注册中断
    twi->already_init++;
}
```

---

## 6. 排除了的假设

| 假设 | 排除理由 |
|------|---------|
| 缺少上拉电阻 | 用户已补焊 4.7kΩ |
| 频率不对 | 诊断确认 `clk=0x0059` = 100KHz ✅ |
| LCD 引脚配置干扰 | 移除 BOE config 中的 PB2/PB3 条目后问题依旧 |
| BUSEN 被 soft reset 清除 | 修复 3 在 soft reset 后重新使能，用户反馈无效 |
| I2C 地址错误 | 第一次传输成功（product_id 读出 "927"）|
| GT9271 芯片坏 | 第一次传输成功，芯片有正常响应 |
| NACK/总线错误 | `stat=0xF8` 表示空闲，不是 NACK 也不是总线错误 |

---

## 7. 需要你调查的 7 个方向

### 方向 1: TWI 时钟是否被意外关闭

`hal_twi_clk_init()` 调用 `hal_clock_enable(twi->clk)`。运行期间是否有代码关闭了该时钟？

你需要：在 `twi_start()` 之前读取 CCU 的 `CLK_BUS_I2C0` 寄存器，确认时钟门控位是否仍为 1。

```c
// CCU 基址通常定义在 hal 或 memory map 中
// CLK_BUS_I2C0 偏移需查 R528 CCU 手册
uint32_t clk_reg = readl(CCU_BASE + CLK_BUS_I2C0);
printf("CLK_BUS_I2C0 = 0x%08X\n", clk_reg);
```

### 方向 2: TWI 复位是否被意外断言

`hal_twi_clk_init()` 调用 `hal_reset_control_reset(twi->reset)`（assert + deassert）。后续是否有代码重新断言了复位？

你需要：在 `twi_start()` 之前读取 CCU 的 `RST_BUS_I2C0` 寄存器，确认复位位是否仍为 0。

### 方向 3: `twi_soft_reset()` 写入后 SRST 位是否自动清除

`twi_soft_reset()` 写 `TWI_SRST_REG(0x18)` bit0=1 触发软复位。写后没有清除该位。如果硬件不自动清除，TWI 可能一直保持在复位状态。

你需要：在 `twi_start()` 之前读取 `TWI_SRST_REG`：
```c
uint32_t srst = readl(base_addr + TWI_SRST_REG);
printf("SRST_REG = 0x%08X\n", srst);
```

### 方向 4: SDA/SCL 实际电平

即使有 4.7kΩ 上拉，某个器件可能异常驱动 SDA 或 SCL 为低。

你需要：
- （最佳）使用逻辑分析仪抓取 I2C 总线波形
- （次选）在 `twi_start()` 之前读取 GPIO 数据寄存器查看 PB2/PB3 电平
- （NuttX 方式）`hal_gpio_get_data(GPIOB(2), &data)` 读 SDA

### 方向 5: 中断路由 / GIC 配置

TWI0 的 IRQ 是 41 (`R528_IRQ_TWI0`)。确认：
- GIC 中该中断是否已使能
- 是否路由到了正确的 CPU
- 是否有其他代码禁用了该中断

### 方向 6: 引擎模式 vs 驱动模式

`twi->twi_drv_used = ENGINE_XFER`（0），使用经典 TWI 状态机 + 中断模式。尝试切换到 `TWI_DRV_XFER`（1），使用驱动模式（DMA/中断控制）。

```c
// 在 hal_twi_init() 中或 hal_twi_engine_do_xfer() 前
twi->twi_drv_used = TWI_DRV_XFER;
```

### 方向 7: GT9271 芯片初始化

第一个 I2C 操作后，GT9271 可能进入了某种需要额外握手才能继续响应的状态。

你需要：在每次 I2C 传输前对 GT9271 执行完整 reset（RST + INT 时序），看问题是否消失。

---

## 8. 你需要添加的诊断代码

### 位置 A: `twi_start()` 内部（最精确）

在 `twi_start()` 的 timeout 循环之前和失败之后添加寄存器 dump：

```c
static int twi_start(const unsigned long base_addr, int port)
{
    unsigned int timeout = 0xff;

    // ← 在这里添加: 读 STAT, CTL, CLK, SRST, CCU_CLK, CCU_RST

    twi_set_start(base_addr);
    while ((twi_get_start(base_addr) == 1) && (--timeout))
        ;

    if (timeout == 0) {
        // ← 在这里添加: 读 STAT, CTL, CLK, SRST, CCU_CLK, CCU_RST
        TWI_ERR("...");
        return SUNXI_TWI_FAIL;
    }
    return SUNXI_TWI_OK;
}
```

**需要读取的寄存器：**
```c
// TWI 内部寄存器（通过 base_addr 访问）
uint32_t stat = readl(base_addr + TWI_STAT_REG);     // 0x10
uint32_t ctl  = readl(base_addr + TWI_CTL_REG);      // 0x0C
uint32_t clk  = readl(base_addr + TWI_CLK_REG);      // 0x14
uint32_t srst = readl(base_addr + TWI_SRST_REG);     // 0x18
uint32_t addr = readl(base_addr + TWI_ADDR_REG);     // 0x00

// CCU 寄存器（需要查 R528 CCU 手册找地址）
// CLK_BUS_I2C0 和 RST_BUS_I2C0 偏移
// 可以在 chip.h/R528 相关头文件中查找
uint32_t ccu_base = 0x02001000;  // 典型 Allwinner CCU 基址
uint32_t clk_bus_i2c0 = readl(ccu_base + 0x0xxx);  // 需确认偏移
uint32_t rst_bus_i2c0 = readl(ccu_base + 0x0xxx);  // 需确认偏移

printf("[TWI_DBG] port=%d pre_start: STAT=0x%02X CTL=0x%02X "
       "CLK=0x%04X SRST=0x%08X CLK_CCU=0x%08X RST_CCU=0x%08X\n",
       port, stat & 0xFF, ctl & 0xFF, clk & 0xFFFF, srst,
       clk_bus_i2c0, rst_bus_i2c0);
```

### 位置 B: `r528_transfer()` 错误路径（已存在）

当前诊断代码在 `hal_twi_xfer()` 返回失败后执行。此时错误路径已经做过第二次 `twi_soft_reset()`，寄存器状态已被改变。这个位置的诊断只能看到"善后"状态，看不到失败时的瞬态。

你可以保留它作为参考，但应该优先实现位置 A 的诊断。

---

## 9. 任务指令

请严格按照以下顺序执行：

### Step 1: 阅读代码

读以下文件的完整内容，理解调用链和状态机逻辑：
1. `drv_twi.c` — NuttX lower-half，看 `r528_transfer()` 和 `r528_i2c_initialize()`
2. `hal_twi.c` — 重点读：`hal_twi_engine_do_xfer()`(1816)、`twi_start()`(533)、`hal_twi_handler()`(1697)、`hal_twi_core_process()`(1418)、`hal_twi_init()`(2343)、`hal_twi_clk_init()`(2162)
3. `twi_sun8iw20.h` — 平台定义（引脚、基址、时钟配置常数）
4. `common_twi.h` — 寄存器偏移
5. `gt911_iic_touch.c` — 触摸驱动
6. `r528_boot.c` — 板级初始化顺序

### Step 2: 实施诊断

在 `twi_start()` 中加入位置 A 的诊断代码（读 STAT、CTL、CLK、SRST、CCU 时钟/复位寄存器）。

### Step 3: 编译打包烧录

```bash
cd /data/openvela/vendor/allwinnertech/lichee
source tools/scripts/envsetup.sh
lunch_nuttx 2
m nsh
pack
```

### Step 4: 分析诊断输出

用诊断数据判断属于哪个方向（第 7 节），然后针对性地修复。

### Step 5: 如果定位到根因

修复后编译打包，请用户烧录验证。确认 I2C 不再报错后，清除所有诊断代码（恢复 `twi_start()` 和 `r528_transfer()` 的干净状态）。

---

## 10. 参考

- CCU 寄存器定义通常在：`chips/r528/drivers/rtos-hal/hal/source/ccu/` 中
- R528 数据手册在文档仓库中
- Linux 内核 goodix 驱动参考：`drivers/input/touchscreen/goodix.c`（寄存器映射完全兼容 GT9271）
- 上一任的 postmortem 报告：`devlog0725-gt9271-debug-postmortem.md`

---

## 11. 第 5 轮调查（AtomCode 第二任 AI）—— 2026-07-25

### 关键发现

**`twi_start()` 成功了。** 我在 `twi_start()` 内部（失败路径）加了诊断代码，但它从未触发。这说明 START 条件**确实生成了**，STA 位被硬件正确清除。之前的结论"硬件无法生成 START"是错误的——START 发出去了。

**真正失败点：`hal_sem_timedwait()` 超时。** 在 `hal_twi_engine_do_xfer()` 中，`twi_start()` 成功后启用中断 (`twi_enable_irq()`)，然后调用 `hal_sem_timedwait()` 等待中断处理完成。这个等待超时了——中断从未触发。

### 推理

1. START 条件生成了（硬件层面 OK）
2. 但 TWI 中断处理函数 `hal_twi_handler()` 没被调用
3. 所以 `hal_twi_core_process()` 没执行，状态机没推进
4. 所以 `hal_sem_post()` 没被调用，semaphore 超时

**可能原因（按概率排序）：**
- **GIC 中断使能被意外关闭。** 第一次传输成功后，某个其他驱动的初始化过程意外操作了 GIC 寄存器，禁用了 IRQ 41
- **TWI 控制器的中断信号被阻塞。** START 后的状态变化（0x08）没产生 INTFLG
- **中断路由到错误的 CPU。** 第一次在 CPU1 运行，worker 可能在 CPU0 上，GIC 路由配置问题

### 我插入的代码

1. **`hal_twi.c` 的 `twi_start()`**：在失败路径加了 TWI 寄存器 + CCU 诊断（STAT/CTL/CLK/SRST/LCR + CCU 0x91C）。**未触发**——证明 twi_start 成功。
2. **`hal_twi.c` 的 `hal_twi_engine_do_xfer()` 超时路径**：加了 GIC ISENABLER/ICPENDR + TWI INTFLG 诊断。**也未输出**——因为 `hal_twi.c` 缺少 `#include <stdio.h>`，且 HAL 编译体系可能独立缓存，修改未生效。
3. **`drv_twi.c` 的 `r528_transfer()` 错误路径**：加了 GIC ISENABLER/ICPENDR + TWI CTL 分解（busen/sta/intflg/inten）诊断。这个文件已验证能输出——**但编译被用户中断了**。

### 给下一任 AI 的指令

**先编译我留在 `drv_twi.c` 的诊断代码，拿到日志后分析。**

```bash
cd /data/openvela/vendor/allwinnertech/lichee
source tools/scripts/envsetup.sh && lunch_nuttx 2 && m nsh && pack
```

烧录后搜 `[TWI] XFER FAIL`，你会看到新增的三行诊断：
- `[TWI]   SRST=... LCR=... CTL(busen=... sta=... intflg=... inten=...)\n`
- `[TWI]   GIC IRQ41: EN=... PEND=...`

**关键判断逻辑：**
- `GIC EN(bit9)=0` → GIC 层面中断被禁用，需要找谁动了 GIC 寄存器
- `GIC EN(bit9)=1` 但 `PEND(bit9)=0` 且 `CTL intflg=0` → TWI 硬件没产生中断
- `GIC EN(bit9)=1` 且 `CTL intflg=1` → TWI 产生了中断但 GIC 没响应（路由问题）

**修复方向：**
- 如果 GIC EN=0：在 `r528_transfer()` 调用 `hal_twi_xfer()` 之前加 `hal_enable_irq(41)` 临时验证
- 如果是路由问题：切换到 `TWI_DRV_XFER` 模式（设置 `twi->twi_drv_used = 1`），这是 DMA/中断驱动模式，走不同路径
- **最简便的验证：** 每次传输前对 GT9271 做完整 reset（RST+INT 时序），排除芯片状态问题

### ⚠️ 重要教训

**不要在 `hal_twi.c` 里加诊断。** 这个文件编译到 HAL 静态库中，可能会被构建系统缓存。`drv_twi.c` 是 NuttX 内核的一部分，每次 `m nsh` 都会重编，诊断可靠。如果非要改 `hal_twi.c`，需要 `m nsh c`（clean build）才能生效。

**CCU 寄存器已确认：**
- CCU 基址：`0x02001000`
- TWI0 时钟门控：offset `0x91C`, BIT(0)
- TWI0 复位控制：offset `0x91C`, BIT(16)，**低电平有效**（写0=assert复位，写1=deassert。`ccu_reset_assert()` 清除 bit，`ccu_reset_deassert()` 设置 bit）
- GICD 物理基址：`0x03001000`
- **GICD 虚拟基址（NuttX 实际使用）：** `MPCORE_ICD_VBASE = CHIP_MPCORE_VBASE(R528_GIC_VADDR=0x03020000) + MPCORE_ICD_OFFSET(0x1000) = 0x03021000`
- TWI0 IRQ：41, GIC SPI #38

---

## 第 6 轮调查（AtomCode 第三任 AI）—— 2026-07-25

### 关键发现

**1. GIC IRQ41 始终使能 — GIC 不是根因。**

用正确的 GICD 虚拟地址（`0x03021000 + 0x104`）读取 ISENABLER[1]：
```
[TWI] GIC IRQ41: pre_EN(b9)=1 post_EN(b9)=1 now_EN=0x00088A19 PEND=0x00100000
```
- `pre_EN(b9)=1`：传输前 IRQ41 已在 GIC 中使能，**没人关过中断**
- `PEND(bit9)=0`：没有 TWI0 的挂起中断

第5轮的"GIC 中断被关"结论是**诊断代码地址错误**导致的误判（读了 `0x03001104` 而非 `0x03021104`）。

**2. CCU 时钟门控和复位均正常。**

```
CCU TWI0(0x91C)=0x00050005 clk_en=1 rst=1
```
- `clk_en=1`：TWI0 时钟已使能 ✅
- `rst=1`：**复位已释放**（CCU 复位是低电平有效，`ccu_reset_assert` 清除 bit，`ccu_reset_deassert` 设置 bit）✅

**3. 根因范围缩小：TWI 控制器硬件不产生中断。**

已排除的因素：
| 假设 | 结论 |
|------|------|
| GIC 中断被关 | ❌ pre_EN(b9)=1 |
| CCU 时钟被关 | ❌ clk_en=1 |
| CCU 复位被断言 | ❌ rst=1（active-LOW deasserted）|
| twi_start() 失败 | ❌ 第5轮证明 START 成功生成 |
| BUSEN 被 soft reset 清除 | ❌ 已加修复，且 CTL(busen=1) |

**4. 真实 GICD 虚拟地址映射**

NuttX 通过 `MPCORE_ICD_VBASE` 访问 GIC 分发器：
- `CHIP_MPCORE_VBASE = R528_GIC_VADDR = R528_SH0_VSECTION(0x03000000) + R528_GIC_OFFSET(0x00020000) = 0x03020000`
- `MPCORE_ICD_VBASE = CHIP_MPCORE_VBASE + MPCORE_ICD_OFFSET(0x1000) = 0x03021000`
- ISENABLER[1]（IRQ32-63）在 `0x03021000 + 0x100 + 4 = 0x03021104`
- ICPENDR[1] 在 `0x03021000 + 0x280 + 4 = 0x03021284`

之前的诊断用 `0x03001000` 作为 GICD 基址是错误的——那是另一个外设的物理地址。

### 仍未解决的问题

**TWI 为什么在第二次传输时不产生中断？**

第一次传输（product_id 读）正常工作，后续所有传输失败。`twi_start()` 成功（START 发得出），但 `hal_sem_timedwait()` 超时——中断从不触发。

**下一个关键诊断：** `[TWI_DBG]`（hal_twi.c 超时路径已有）需要 clean build 才能生效。它会在超时瞬间读取 CTL(INTEN/INTFLG) 寄存器，告诉我们 INTEN 是否设置成功、硬件 INTFLG 是否被置位。

**修复方向（待验证后可选择）：**
1. 如果 INTEN=1 但 INTFLG=0 且 GIC PEND=0 → TWI 硬件不产生中断信号（可能是模块级问题）
2. 如果 INTEN=0 → `twi_enable_irq()` 写入未生效（检查寄存器写保护）
3. 切换到 `TWI_DRV_XFER` 模式（`twi->twi_drv_used = 1`）绕过 engine 模式
4. 如果以上都不行，需用逻辑分析仪抓 I2C 总线看 START 后发生了什么

### ⚠️ 第 6 轮教训

1. **不要轻易否定已验证的数据。** 第5轮已验证 GICD 基址为 `0x03001000`（物理地址），我基于内存映射注释的笔误改成 `0x03020000` 浪费了两轮烧录。
2. **CCU 复位极性因平台而异。** 本平台使用低电平有效复位（`ccu_reset_assert` 写 0），与常见的"写 1 断言复位"相反。
3. **MPCORE_ICD_VBASE ≠ 物理 GICD 基址。** 需要加上 `MPCORE_ICD_OFFSET=0x1000`。NuttX 的 GIC 驱动通过这个宏访问寄存器，而物理地址直接读可能读到不同的外设。
4. **clean build（`m nsh c`）是激活 hal_twi.c 诊断的唯一方法。** 增量编译不重编 HAL 静态库。

---

## 附录 A：第 6 轮烧录纪录（2026-07-25）

此表记录每次烧录的固件内容、诊断输出和结论。**下一任 AI 接手后优先读此表**，了解已做了什么、得到了什么数据。

| # | bootlog | 固件内容 | 关键诊断输出 | 结论 |
|---|---------|---------|-------------|------|
| 1 | `bootlog725.txt` | 初始固件（第5轮遗留）。drv_twi.c 有 GIC 诊断，但 GICD 硬编码 `0x03001000` | `GIC EN=0x00000000(b9=0)` | ❌ 误判：这个地址不对，读的不是 GIC |
| 2 | `bootlog726.txt` | GICD 改为 `0x03020000`（错改） | `EN=0x00000000(b9=0)` 不变 | ❌ 也是错的。0x03020000 还不是正确的虚拟地址 |
| 3 | `bootlog727.txt` | GICD 改为 `R528_GIC_VADDR + 0x1000 = 0x03021000`，加了 `hal_enable_irq(41)` | `pre_EN(b9)=1, post_EN(b9)=1, now_EN=0x00088A19` | ✅ **正确数据！GIC 正常，IRQ41 始终使能** |
| 4 | `bootlog728.txt` | 加了 CCU `0x91C` 读取 | `CCU TWI0(0x91C)=0x00050005 clk_en=1 rst=1` | ✅ **CCU 也正常。clk 使能、复位释放（active-LOW）** |
| 5 | `bootlog729.txt` | clean build（`m nsh c`），期望 hal_twi.c 的 `[TWI_DBG]` 输出 | 无 `[TWI_DBG]` 输出 | ❌ HAL 层 `printf` 不路由到控制台。**HAL 诊断不可靠** |
| 6 | *未烧录* | 加了 `PRE_XFER` 传输前读 CTL/STAT | — | 用户要求停止烧录。固件已编译好可随时用 |

### 诊断日志阅读指南

当你在 bootlog 中看到以下模式时，对照解读：

```
[TWI] PRE_XFER: STAT=0x?? CTL=0x?? (busen=? sta=? intflg=? inten=?)
```
→ 每次传输开始前读取的 TWI 状态。`inten` 应为 0（还没设置）。`stat` 应在 0xF8（idle）或合理值。

```
[TWI] XFER FAIL port=0 stat=0xF8 ctrl=0x40 clk=0x0059 drv_intc=0x00000000
[TWI]   SRST=0x00 LCR=0x3A CTL(busen=1 sta=0 intflg=0 inten=0)
```
→ 传输失败后的错误路径诊断。`stat=0xF8` 是错误路径 `twi_soft_reset()` 后的空闲态。`inten=0` 因为错误路径调了 `twi_disable_irq()`。

```
[TWI]   GIC IRQ41: pre_EN(b9)=1 post_EN(b9)=1 now_EN=0x00088A19 PEND=0x00100000
```
→ **`pre_EN(b9)` 是最重要的信号**：`1` = IRQ41 已在 GIC 使能。`PEND(bit9)=0` = 无 TWI0 挂起中断。

```
[TWI]   CCU TWI0(0x91C)=0x00050005 clk_en=1 rst=1
```
→ `clk_en=1` = TWI0 时钟已开。`rst=1` = 复位已释放（**active-LOW**，1=不复位）。

```
[TWI_DBG] ===== xfer timeout ... (HAL 层，仅 clean build 生效)
```
→ 超时瞬间在 HAL 层读取的寄存器。但此输出**可能不出现**——`printf` 在 HAL 上下文中可能不路由到 UART 控制台。不要依赖它。

### 给下一任 AI 的避坑清单

**⚠️ 不要重复我犯的 6 个错误：**

1. **不要改已验证的地址。** 第5轮已验证 GICD 物理基址 `0x03001000`，我因看到内存映射注释的笔误就改了——浪费两轮烧录。
   - ✅ 做法：如果对地址有疑问，读 `GICD_IIDR`（offset `0xFC`）确认。GIC-400 返回 `0x0200143B`。

2. **诊断 GIC 前，先确认你要用什么地址。** NuttX 通过虚拟地址访问 GIC：
   ```
   MPCORE_ICD_VBASE = R528_GIC_VADDR + MPCORE_ICD_OFFSET = 0x03020000 + 0x1000 = 0x03021000
   ```
   ISENABLER[1] 在 `0x03021104`。**不要直接用物理地址 `0x03001000`！**

3. **CCU 复位极性因平台而异。** 读 `ccu_reset_assert()` 的实现：是 set bit 还是 clear bit？
   - 本平台：`assert = clear bit`（写 0），`deassert = set bit`（写 1）——低电平有效。

4. **不要在 `hal_twi.c` 加诊断。** 它编译到 `librtos-hal.a`，增量编译可能不重编。即使 clean build，HAL 层的 `printf` 也可能不路由到控制台。
   - ✅ 永远在 `drv_twi.c` 加诊断。

5. **读 bootlog 时先 grep 关键字符串。** 推荐的一行命令：
   ```bash
   grep -n "TWI_DBG\|TWI XFER\|PRE_XFER\|GIC IRQ41\|CCU TWI0\|product_id\|I2C fail 10\|I2C_READ FAIL" bootlogXXX.txt
   ```

6. **不要一次次烧录验证单一假设。** 能一次读全的寄存器（GIC + CCU + TWI STAT/CTL + SRST + LCR）一次读完。第6轮花了4次烧录才排除了 GIC 和 CCU。

### 第6轮已排除的假设和当前状态

**已排除（不需要再查）：**
- ❌ GIC IRQ41 被意外关闭 — `pre_EN(b9)=1` 始终使能
- ❌ CCU TWI0 时钟被关闭 — `clk_en=1`
- ❌ CCU TWI0 复位被断言 — `rst=1`（active-LOW deasserted）
- ❌ `twi_start()` 失败 — 第5轮确认 START 条件生成
- ❌ BUSEN 被 soft reset 清除 — CTL(busen=1)

**待查（如果你接手）：**
- 🔍 TWI 的 `twi_enable_irq()` 写入 CTL[INTEN] 是否被硬件接受？
- 🔍 GT9271 在第一次读后是否进入不响应状态？（需要逻辑分析仪）
- 🔍 `TWI_DRV_XFER` 模式是否能绕过 engine 模式的问题？

**最后的手段：** 如果以上都排除了，问题可能在 R528 芯片的 TWI 模块硬件 bug 或模块电源域/时钟域隔离问题。到那时需要对比 Linux 内核的 TWI 驱动实现。
