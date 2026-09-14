# MiuMiu 开发日志
# 机型: R528s3-gemini-s1 (Allwinner R528 双核 Cortex-A7, NuttX/OpenVela)

## 环境

```
SSH: sshpass -p 'lala891209' ssh -6 arc@2409:8a55:9a3e:5170:eaff:1eff:feda:3736
编译: cd vendor/allwinnertech/lichee && source tools/scripts/envsetup.sh && lunch_nuttx 2 && m nsh c && pack
固件: vendor/allwinnertech/lichee/out/r528s3/gemini-s1_nand/rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img
```

## 硬件信息

- **SoC**: Allwinner R528 双核 Cortex-A7
- **显示**: BOE 1200x1920, framebuffer `/dev/lcd0`, LVGL 32-bit色深, NEON加速, G2D GPU
- **触摸**: GT9271 I2C触摸屏, TWI0总线, 地址0x5D, RST=PB4, INT=PB5
- **音频**: Allwinner ACodec, `/dev/audio/pcmC0D0p`, nxplayer支持MP3解码
- **存储**: 128MB NAND, NVMe `/dev/nvme1n1p1` 挂载到 `/data` (ext4)

---

## 第1次烧录: TWI驱动模式诊断

**改动**:
- `hal_twi.c`: 切换到驱动模式 (`#if 0`), 添加 `hal_twi_set_drv_mode()` 运行时模式切换
- `drv_twi.c`: 添加 `PRE_XFER` (传输前) 和 `XFER FAIL` (传输后) 诊断打印
- `sunxi_hal_twi.h`: 添加 `hal_twi_set_drv_mode()` 声明

**结果**: GT9271读取依然失败。诊断显示驱动模式有竞态条件 — `twi_read()` 先设SLA+R再启动传输，但寄存器地址字节在启动后才加载到TX FIFO。

---

## 第2次烧录: 切换到Engine模式 + 关键诊断

**改动**:
- `hal_twi.c`: 切回Engine模式 (`#if 1`), 添加 `ENG_RES` 诊断 — 在信号量等待**之后**、`soft_reset` **之前**读取TWI状态

**结果**: **重大突破!** `ENG_RES` 首次捕获到真实失败原因:
```
[ENG_RES] msgs_idx=32, stat=0x20
```
- `msgs_idx=32` = 0x20 = `SLA+W transmitted, NOT ACK received`
- GT9271芯片本身在开机后第一次I2C读取时**拒绝应答**
- 不是总线问题，不是驱动时序问题，是**GT9271芯片行为**

---

## 第3次烧录: 软件重置+重试机制 (修复)

**改动**:
- `gt911_iic_touch.c` (`gt911_worker`): I2C失败时执行 `gt911_reset_chip()` + 立即重试
- 发现: 第一次读取**必然失败**，重置后重试**必然成功**，之后所有读取都正常
- 添加 `first_run` 重置标志和500ms延迟尝试

**结果**: 触摸功能正常工作! 确认GT9271需要两个重置周期才能稳定 — 第一个周期总是NACK，第二个周期总是OK。

---

## 第4次烧录: 硬件上拉电阻测试

**改动**:
- RST引脚(PB4)接4.7K上拉电阻到3.3V

**结果**: 没有改善。RST上拉对GT9271首次NACK问题无效。根因是芯片内部行为，不是引脚浮空。

---

## 第5次烧录: INT上拉测试

**改动**:
- INT引脚(PB5)也接4.7K上拉电阻到3.3V (RST上拉保留)

**结果**: **更糟!** 两个I2C地址(0x5D, 0x14)都NACK。INT上拉干扰了GT9271的I2C通信。**结论: 不要给INT加上拉。**

---

## 第6次烧录: 确认RST上拉无用 + 清理

**改动**:
- 仅保留RST上拉 (移除INT上拉)
- 移除 `first_run` 重置块和500ms延迟

**结果**: 行为与无上拉完全一致。确认: RST上拉无用，500ms延迟无用。保留 `fail → 重置 → 重试` 机制即可。

---

## 第7次烧录: 清理所有诊断代码

**改动**:
- `hal_twi.c`: 移除所有 `[ENG_PRE]`, `[ENG_RES]`, `[TWI] PRE_XFER`, `[TWI] XFER FAIL` 诊断打印
- `drv_twi.c`: 移除所有诊断打印块
- 保留Engine模式 (`#if 1`) 和GT911的fail→重置→重试机制

**结果**: 代码干净，触摸正常。这是最终的驱动代码状态。

---

## 第8次烧录: LVGL触摸Demo + 开机自启

**改动**:
- `lvgldemo.c`: 重写为菜单界面
  - 主菜单: Touch Color Test / Widgets Demo / Music Demo 三个按钮
  - Touch Color Test: 点击屏幕切换颜色 (10种颜色循环), 显示触摸坐标
  - Widgets / Music: 调用LVGL内置Demo
- `rcS.nsh`: 添加 `lvgldemo &` 开机自启

**结果**: 开机显示菜单, 触摸正常, 颜色切换正常。

---

## 第9次烧录: nxplayer + MP3播放 (当前)

**改动**:
- `nxplayer_main.c`: 添加命令行参数支持, `nxplayer <file>` 直接播放文件
- `rcS.nsh`: 添加 `nxplayer /data/laojie.mp3 &` 开机自启
- `laojie.mp3` (5.3MB) 打包进 `usrdata.fex` 数据分区

**结果**: 开机后同时显示LVGL菜单 + 播放laojie.mp3。

---

## 最终文件状态

### 已修改的文件
| 文件 | 改动 |
|------|------|
| `gt911_iic_touch.c` | `gt911_worker()`: I2C失败时重置+重试 |
| `hal_twi.c` | `#if 1` engine模式, 所有诊断打印已移除 |
| `drv_twi.c` | 所有诊断打印已移除 |
| `lvgldemo.c` | 重写: 菜单 + Touch Color Test |
| `nxplayer_main.c` | 支持命令行参数播放文件 |
| `rcS.nsh` | 添加 `lvgldemo &` 和 `nxplayer /data/laojie.mp3 &` |
| `UDISK/laojie.mp3` | MP3文件, 打包进usrdata |

### GT9271触摸修复总结
```
问题: GT9271开机后第一次I2C读取必然NACK (SLA+W被拒绝)
根因: GT9271芯片内部行为, 不是总线/驱动/上拉问题
修复: gt911_worker() 中 fail → gt911_reset_chip() → 重试
效果: 第一次失败自动恢复, 之后100%稳定, 零失败
硬件: RST上拉无用, INT上拉有害, 保持原始电路即可
```
