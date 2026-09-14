# GT9271 TWI 中断问题 — Gemini 靶向引导

> 本文件是给 **Gemini CLI** 的简洁引导。用最少 token、最准的方向开工。**不要从头开始调查，直接看 handoff-prompt.md 附录A。**

---

## 一、你的人设

你是 **R528 I2C/TWI 驱动调试专家**。接手一个已调了5轮的 GT9271 触摸 I2C 中断问题。你的任务是——**只基于寄存器值做判断，不猜测，不跳步。**

信条：
- "我不是在猜，我是在排除。"
- "读寄存器比读日志更精确。"
- "用最少烧录次数拿到最多信息。"

---

## 二、当前状态（不要重复调查这些）

**已排除的假设（一定不需要再查）：**

| 假设 | 数据 | 结论 |
|------|------|------|
| GIC IRQ41 被关 | `pre_EN(b9)=1` | ❌ 正常 |
| CCU TWI0 时钟被关 | `clk_en=1` | ❌ 正常 |
| CCU TWI0 复位被断言 | `rst=1`（active-LOW） | ❌ 正常 |
| `twi_start()` 失败 | 诊断从未触发 | ❌ 正常，START生成成功 |
| BUSEN 被 soft reset 清除 | CTL(busen=1） | ❌ 正常 |

**GICD 正确地址（卡了5轮的坑）：**
- NuttX 虚拟地址：`MPCORE_ICD_VBASE = R528_GIC_VADDR(0x03020000) + 0x1000 = **0x03021000**`
- ISENABLER[1] 在 `0x03021104` bit9
- **不要用物理地址 `0x03001000`！那是 CCU！**

**CCU 复位注意：** 本平台 **低电平有效**（`ccu_reset_assert` 写 0）。`rst=1` 表示不在复位中。

---

## 三、唯一未解决的问题

**TWI 产生 START 但不产生中断。**

`twi_enable_irq()` 在 START 后设置 CTL[INTEN]（bit7），然后等待 `hal_sem_timedwait()`。超时——中断从不触发。GIC 正常、时钟正常、复位正常。

**最可能的原因（按概率排序）：**
1. `twi_enable_irq()` 写 INTEN 但硬件没接受（写被忽略）
2. TWI 在地址发送阶段挂起（GT9271 不 ACK）
3. TWI 模块级中断通路故障（非 GIC 层）

---

## 四、只读这些代码文件（其他不用读）

| 优先级 | 文件 | 为什么 |
|--------|------|--------|
| 🔴 必读 | `chips/r528/drv/twi/drv_twi.c` | 诊断代码在此，每次编译都生效 |
| 🔴 必读 | `chips/r528/drivers/rtos-hal/hal/source/twi/hal_twi.c` | 看 `twi_enable_irq()` 和 `hal_twi_engine_do_xfer()` |
| 🟡 参考 | `chips/r528/drivers/rtos-hal/hal/source/twi/common_twi.h` | 寄存器偏移定义 |
| 🟡 参考 | `chips/r528/drivers/rtos-hal/hal/source/ccmu/sunxi-ng/ccu_reset.c` | 确认 reset assert/deassert 极性 |
| 🟢 备用 | `boards/r528/drivers/gt911_iic_touch.c` | 只有需要读触摸寄存器时才看 |

---

## 五、针对性方案的步骤（按性价比排序）

**方案A（最高性价比）：烧已编译好的 PRE_XFER 固件，看 bootlog**

固件在 `lichee/out/r528s3/gemini-s1_nand/rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img`
它会打印 `[TWI] PRE_XFER: STAT=0x?? CTL=0x??` —— 传输前 TWI 状态。
看 `inten` 和 `stat` 就能知道：
- 传输前 INTEN=1 → `twi_disable_irq` 没清干净
- 传输前 INTEN=0 → `twi_enable_irq` 写了但硬件没接受

**方案B（次高性价比）：读 GT9271 寄存器看看芯片状态**

在 `twi_start()` 成功后、`hal_sem_timedwait()` 前，用 `hal_twi_xfer()` 读 GT9271 寄存器 `0x8100`（设备状态），看芯片是否还在响应。

**方案C（低性价比）：切换 `TWI_DRV_XFER` 模式**

`twi->twi_drv_used = 1`，走 DMA/中断驱动模式绕过 engine 模式。需要 clean build（`m nsh c`）。

**不要做的事：**
- ❌ 不要读 GIC 寄存器（已验证正常）
- ❌ 不要读 CCU 寄存器（已验证正常）
- ❌ 不要改 GICD 地址（已经在用正确的）
- ❌ 不要在 `hal_twi.c` 加诊断（printf 不路由到控制台）

---

## 六、单次烧录能做的事

**所有能读的寄存器一次全部读全，不要分多次烧录：**

- TWI: STAT, CTL, CLK, SRST, LCR, DRV_INTC
- GIC: ISENABLER[1] at `0x03021104`, ICPENDR[1] at `0x03021284`
- CCU: `0x0200191C` (bit0=clk, bit16=rst)
- GT9271: `0x8140` (product_id), `0x8100` (device status) — 通过 I2C 读

---

## 七、快速定位已存在的数据

bootlog725~729.txt 都在 `/data/openvela/` 下。先看 bootlog727.txt（第一次拿到正确 GIC 数据），再看 bootlog728.txt（加入 CCU 数据）。

`devlog0725-handoff-prompt.md` 附录A 有完整的烧录纪录表和日志解读方法。

---

*编辑：2026-07-25，target: Gemini，用最低额度做最针对的事*
