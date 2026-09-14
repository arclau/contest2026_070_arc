# tfdevlog727.md — TF 卡挂载问题全记录

> 日期: 2026-07-27
> 目标: Allwinner R528 (gemini-s1) + OpenVela/NuttX
> 问题: TF 卡无法挂载，SMHC0 初始化卡死

---

## 一、问题现象

启动串口 log 关键段:

```
[PWR] step1: before clk_enable
[PWR] step2: after clk_enable, will dump regs
[PWR] step3: dump done
[PWR] step4: HW reset SKI...    ← 卡死在这行之后
```

- SMHC1 (WiFi SDIO) 初始化正常
- SMHC0 (TF 卡) 在 `rom_HAL_SDC_PowerOn` 中卡死
- 插卡/不插卡都一样
- `[CLK]` debug printf 从未出现 → `__mci_clk_prepare_enable` 未执行或更早就卡了

---

## 二、涉及的源码文件

| 文件 | 路径 | 说明 |
|------|------|------|
| TF卡驱动 | `/data/openvela/vendor/allwinnertech/boards/r528/drivers/micro_sd/micro_sd_driver.c` | 当前已修改: cd_mode=3 + 同步mmcsd_slotinitialize+mount |
| SMHC主机驱动 | `/data/openvela/vendor/allwinnertech/chips/r528/drivers/rtos-hal/hal/source/sdmmc/hal_sdhost.c` | 多处修改: debug printf, 跳HW reset, 直接写时钟寄存器 |
| SDIO多卡版 | `/data/openvela/vendor/allwinnertech/chips/r528/drivers/rtos-hal/hal/source/sdmmc/nuttx_sdio_mult.c` | 加了create/init debug printf |
| 板级boot | `/data/openvela/vendor/allwinnertech/chips/r528/r528_boot.c:966` | 调用 `micro_sd_initialize()` |
| 平台定义 | `.../hal/source/sdmmc/platform/mmc_sun20iw1p1.h` | R528 平台寄存器地址 |
| 时钟宏 | `hal_sdhost.c:122-152` | SDC0 CCM 操作宏 (两个ifdef分支) |
| pinmux | `.../hal/source/sdmmc/hal_sdpin.c:127` | `sdmmc_pinctrl_init()` |

---

## 三、已做的尝试和结果

### 尝试 1: 修复 cd_mode

**做法:** `micro_sd_driver.c` 将 `set_sdio_param(0, 2, ...)` → `set_sdio_param(0, CARD_ALWAYS_PRESENT, ...)`

**结果:** ✅ `host->present=1` 但 `cd_cb` 不触发 → `mmcsd_slotinitialize` 不在回调中执行 → `/dev/mmcsd1` 不出现

**教训:** CARD_ALWAYS_PRESENT 模式下 SMHC 驱动不会调用 card_detected 回调，需要同步执行 mmcsd_slotinitialize

### 尝试 2: 加同步 mmcsd_slotinitialize + mount

**做法:** 在 `micro_sd_initialize()` 中 `sdio_initialize()` 之后直接调用 `mmcsd_slotinitialize(1, mrcio_sd_host)` 和 `mount()`

**结果:** ❌ 系统在 `sdio_initialize(0)` → `hal_sdc_init` → `rom_HAL_SDC_PowerOn` 中卡死

### 尝试 3: 跳过 HW reset（第一次）

**做法:** 在 `rom_HAL_SDC_PowerOn` 中 `__mci_check_bit_clear` 超时后手动清除 HWReset bit

**结果:** ❌ 写 HWReset 后 SMHC0 彻底锁死，读任何寄存器都卡住

### 尝试 4: 直接跳过整个 HW reset 段

**做法:** 删除 HW reset 三段代码（读GCTRL→写HWReset→等clear），只留清中断+配置

**结果:** ❌ 依然卡在同一个位置（`[PWR] step4: HW reset SKIPPED` 打印到一半）

### 尝试 5: 直接写时钟寄存器 + 回读验证

**做法:** 在 `__mci_clk_prepare_enable` 中不用宏而是直接 `hal_writel` 写 0x200184c 的 bit0 和 bit16，并加 `[CLK]` printf 回读

**结果:** ❌ `[CLK]` 日志从未出现，SMHC0 的 `__mci_clk_prepare_enable` 没执行到

### 尝试 6: distclean 全量重编

**做法:** `./build.sh ... distclean -j12` + 全量编译 + pack

**结果:** ❌ 同样卡死

---

## 四、关键发现

### 4.1 寄存器 dump 对比

从 crash 时 dump 的寄存器值:

| 寄存器 | 地址 | 值 | 含义 |
|--------|------|-----|------|
| BUS_GATE_RESET | 0x200184c | `0x20002` | **SDC0 时钟未开!** bit0=0, bit16=0 |
| SDC1 SCLK_CTRL | 0x2001834 | `0x8000010e` | SDC1 模块时钟已使能 ✅ |

对比之前能工作的固件:

| 寄存器 | 之前(能工作) | 现在(挂了) |
|--------|:---------:|:-------:|
| 0x200184c | `0x30003` | `0x20002` |
| bit 0 (SDC0 bus clock) | **1** ✅ | **0** ❌ |
| bit 16 (SDC0 bus reset) | **1** ✅ | **0** ❌ |
| bit 1 (SDC1 bus clock) | 1 | 1 ✅ |
| bit 17 (SDC1 bus reset) | 1 | 1 ✅ |

**SDC0 的时钟门控和总线复位从未被使能。** bootloader 只配了 SDC1(WiFi)，没配 SDC0(TF卡)。

### 4.2 `hal_writel` 写 0x200184c 不生效

宏 `SDC0_CCM_BusEnableClock()` 和直接 `hal_writel()` 对 `SDC_CCM_SDC_BUS_GATE_RESET` (0x200184c) 的 bit0/bit16 写入均不生效。

`hal_writel` 定义:
```c
#define hal_writel(value,reg)  ({isb(); *(volatile uint32_t*)(long)(reg) = (value); dsb();})
```

就是普通内存映射写，无特殊解锁机制。但同一个地址的 bit1/bit17（SDC1）是 1（bootloader 设的）。

**可能原因:**
1. R528 的 CCU 模块需要特殊解锁序列才能写（如先写 0x16AA0000 到某寄存器）
2. 该寄存器有写保护位（bit 28）
3. `hal_writel` 使用的内存映射在 NuttX 中未正确建立（MMU 页表不允许写该地址）
4. SMHC0 物理上不存在或未连接（但读返回了非全F值）

### 4.3 之前的固件为什么能工作

之前能工作的固件不是从当前代码编译的。当前代码是 repo sync 恢复的 SDK 原始版，而之前的固件有大量修改（包括 hal_sdhost.c 的 DMA 错误处理、r528_bringup.c 的初始化等）。

**没有备份**被 repo sync 冲掉的 hal_sdhost.c 修改版。`/data/vela-backup/` 中有 screen-driver/touch-driver/audio，但 **没有** hal_sdhost.c 的备份。

---

## 五、当前代码改动汇总

### micro_sd_driver.c

```c
// 改: cd_mode 2→3
set_sdio_param(0, 3, card_detected);

// 增: 同步 mmcsd_slotinitialize + mount
ret = mmcsd_slotinitialize(1, mrcio_sd_host);
ret = mount("/dev/mmcsd1", "/sdcard/", "vfat", ...);
```

### hal_sdhost.c

多处改动，当前状态可用 `git diff` 查看:
- `__mci_clk_prepare_enable`: SDC0 直接写 BUS_GATE_RESET + 回读 `[CLK]` printf
- `rom_HAL_SDC_PowerOn`: 跳过 HW reset + `[PWR]` step printf
- `SDC_BUSY_WAIT_LOOP`: 0xffffffff → 1000000

### nuttx_sdio_mult.c

- `sdio_initialize`: 加了 `[TF]` create/init debug printf

---

## 六、下一任 AI 交接清单

### 优先级 1: 排查 `hal_writel` 为什么写不进 0x200184c

这是核心阻塞点。建议排查方向:

**方向 A: CCU 寄存器写保护**
查阅 R528 技术参考手册 (TRM) 中 CCU 章节，确认 BUS_GATE_RESET 寄存器是否有写使能位或解锁序列。Allwinner H6/H616 等芯片的 CCU 可能需要先写特定 key。

**方向 B: MMU 页表权限**
检查 NuttX 启动时 CCU 地址范围 `0x2001000-0x2001xxx` 的 MMU 映射属性。如果页表标记为只读，写入会静默失败。

**方向 C: 对比能工作的 commit**
能用 `git log` 和 `git bisect` 找到之前能工作的 commit，对比差异。之前能工作的固件对应的代码状态已丢失，但 git 历史中可能有记录。

**方向 D: 使用 writel 替代 hal_writel**
SDC0 的 `#if` 分支(124-141行)中使用了 `writel()` 而不是 `hal_writel()`，看看是否 `writel` 能写进去。

### 优先级 2: 确认 SMHC0 硬件是否存在

在 NuttX 启动早期 dump `0x200184c` 原始值和 `0x04020000` (SMHC0基地址) 的值，确认 bootloader 是否初始化了 SMHC0。

### 优先级 3: 改用 SMHC1 或 SMHC2

如果 SMHC0 确认有问题，考虑将 TF 卡改到 SMHC1 或 SMHC2。但 SMHC1 已用于 WiFi SDIO，可能需要确认硬件连接。

### 优先级 4: 修 DMA 超时 Data Abort

即使挂载成功，原 SDK 的 DMA 超时错误处理器中有 bug:
```c
for (i = 0; i < sg_len; i++) {
    struct scatterlist *sg = data->sg;  // data 可能为 NULL
    if (HAL_PT_TO_U(sg[i].buffer) & 0x03) {  // → Data Abort
```

需要加 `if (data && data->sg)` 保护或恢复之前删除的 `SDC_BUG_ON` 回避。

### 优先级 5: 清理 debug printf

所有 `[PWR]`, `[CLK]`, `[TF]` 调试 printf 在问题解决后应移除。

---

## 七、参考文件

| 文件 | 说明 |
|------|------|
| `/data/vela/devlog.md` | 开发日志(v4.0布局重构) |
| `/data/openvela/velafix-0726.md` | 7/26 上午修复记录(音频/触摸) |
| `/data/openvela/velafix-0726t2.md` | 7/26 下午第二阶段修复(TF卡/播放器) |
| `/data/openvela/tfdevlog.txt` | 7/26 TF卡调试全记录 |
| `/data/openvela/tfdevlog727.md` | **本文** - 7/27 TF卡问题交接 |
| `/data/vela-backup/` | SDK 恢复前的文件备份(无 hal_sdhost.c) |

### 关键硬件寄存器地址 (R528 sun20iw1p1)

```
SMHC0_BASE = 0x04020000
SMHC1_BASE = 0x04021000
CCM_BASE   = 0x2001000
BUS_GATE_RESET  = 0x2001000 + 0x84c = 0x200184c
SDC0_SCLK_CTRL  = 0x2001000 + 0x830 = 0x2001830
SDC1_SCLK_CTRL  = 0x2001000 + 0x834 = 0x2001834
```

### SDC0 引脚 (gemini-s1)

```
SDC0_CLK = GPIO_PF2
SDC0_CMD = GPIO_PF3
SDC0_D0  = GPIO_PF1
SDC0_D1  = GPIO_PF0
SDC0_D2  = ? (未在 mmc_sun20iw1p1.h 中定义)
SDC0_D3  = ? (未在 mmc_sun20iw1p1.h 中定义)
```
