# MiuMiu 施工逻辑 — GT9271修复 + LVGL Demo + 音乐播放

> 本文档记录从"触摸不工作"到"开机显示菜单+播放音乐"的完整施工路径。按烧录次序组织，每次烧录明确：改了什么、验证了什么、结论是什么。

---

## 总览

```
第1-2次: TWI 驱动层诊断 (drv_twi.c / hal_twi.c)
第3次:   GT9271 修复 (gt911_iic_touch.c) ← 关键转折
第4-6次: 硬件上拉验证 (排除法)
第7次:   代码清理 (生产就绪)
第8次:   LVGL 触摸 Demo
第9次:   NXPlayer 音乐播放
```

---

## 第1次烧录: TWI 驱动模式诊断

### 目标
在 TWI 驱动层添加传输前/后寄存器快照，定位失败时的硬件状态。

### 改动

**hal_twi.c:**
- 行 2443: `#if 1` → `#if 0` (切换到驱动模式)
- 新增 `hal_twi_set_drv_mode()` 运行时模式切换函数

**drv_twi.c:**
- `r528_transfer()` 入口: 添加 `PRE_XFER` 诊断 (读 TWI_STAT_REG + TWI_CTL_REG)
- `r528_transfer()` 错误路径: 添加 `XFER FAIL` 诊断 (读 stat/ctrl/clk/srst/lcr/drv_intc/GIC/CCU)
- 每次传输前调用 `hal_twi_set_drv_mode()`

**sunxi_hal_twi.h:**
- 添加 `hal_twi_set_drv_mode()` 声明

### 结果
❌ GT9271 读取依然失败。诊断显示驱动模式有竞态条件 — `twi_read()` 先设 SLA+R 再启动传输，但寄存器地址字节在启动后才加载到 TX FIFO。

### 结论
驱动模式的 combined write-read 有时序问题。但这不是 GT9271 问题的根因 — 因为第一次读取（detect）在驱动模式下也成功了。

---

## 第2次烧录: Engine 模式 + ENG_RES 诊断 ← 关键突破

### 目标
切回 Engine 模式，在 soft_reset 之前捕获 TWI 真实状态。

### 改动

**hal_twi.c:**
- 行 2459: `#if 0` → `#if 1` (切回 Engine 模式)
- 在 `hal_twi_engine_do_xfer()` 中，信号量等待之后、`soft_reset` 之前，添加 `ENG_RES` 诊断:
  ```c
  printf("[ENG_RES] msgs_idx=%d stat=0x%02x\n",
         msgs->msgs_idx, readl(base + TWI_STAT_REG));
  ```

### 结果
✅ **重大突破!** `ENG_RES` 首次捕获到真实失败原因:

```
[ENG_RES] msgs_idx=32, stat=0x20
```

- `msgs_idx=32` = 0x20 = `SLA+W transmitted, NOT ACK received`
- **GT9271 芯片在开机后第一次 I2C 读取时拒绝应答**
- 不是总线问题，不是驱动时序问题，是 **GT9271 芯片行为**

### 结论
根因已定位到芯片层。下一步是在触摸驱动层面解决。

---

## 第3次烧录: GT9271 软件重置+重试 ← 修复

### 目标
在触摸驱动层面解决 GT9271 首次 NACK 问题。

### 改动

**gt911_iic_touch.c (`gt911_worker()`):**
- I2C 读取失败时执行 `gt911_reset_chip()` + 立即重试
- 发现: 第一次读取**必然失败**，重置后重试**必然成功**
- 添加 `first_run` 重置标志（尝试在首次 worker 执行前主动重置）

### 结果
✅ **触摸功能正常工作!**

确认 GT9271 行为模式:
```
上电 → detect (成功) → worker 第一次读 (NACK) → 重置 → worker 重试 (成功) → 之后全部成功
```

### 结论
GT9271 需要两个重置周期才能稳定。`fail → 重置 → 重试` 机制是最简修复方案。

---

## 第4次烧录: RST 上拉电阻测试

### 目标
验证 GT9271 首次 NACK 是否由 RST 引脚浮空导致。

### 改动
- RST 引脚 (PB4) 接 4.7K 上拉电阻到 3.3V

### 结果
❌ 没有改善。行为与无上拉完全一致。

### 结论
GT9271 首次 NACK 不是 RST 引脚浮空问题。根因是芯片内部行为。

---

## 第5次烧录: INT 上拉电阻测试

### 目标
验证 INT 引脚电气环境是否影响 GT9271 I2C 通信。

### 改动
- RST 上拉保留
- INT 引脚 (PB5) 也接 4.7K 上拉电阻到 3.3V

### 结果
❌ **更糟!** 两个 I2C 地址 (0x5D, 0x14) 都 NACK。

### 结论
**INT 上拉有害。** GT9271 的 INT 引脚在 I2C 通信中有复用功能，改变其默认电平会干扰芯片行为。

---

## 第6次烧录: 移除 INT 上拉 + 清理 first_run

### 目标
回到已验证可用的状态，移除无效的实验性改动。

### 改动
- 移除 INT 上拉 (硬件)
- 移除 `first_run` 重置块和 500ms 延迟（实验无效）
- 保留 `fail → 重置 → 重试` 核心机制

### 结果
✅ 行为与第3次烧录一致。触摸正常。

### 结论
最终硬件状态: **不要给 RST/INT 加上拉。** 保持原始电路。

---

## 第7次烧录: 清理所有诊断代码

### 目标
移除所有调试诊断，代码达到生产状态。

### 改动

**hal_twi.c:**
- 移除 `[ENG_PRE]` 诊断块
- 移除 `[ENG_RES]` 诊断块
- 移除 `[TWI] PRE_XFER` 诊断块
- 移除 `[TWI] XFER FAIL` 诊断块
- 保留 `#if 1` (Engine 模式)

**drv_twi.c:**
- 移除所有 `PRE_XFER` 和 `XFER FAIL` 诊断打印
- 移除 `hal_twi_set_drv_mode()` 调用
- 移除 BUSEN 检查

### 结果
✅ 代码干净。触摸正常。日志中只有正常的触摸事件。

### 结论
这是最终的驱动代码状态。不再有诊断代码。

---

## 第8次烧录: LVGL 触摸 Demo

### 目标
创建带菜单的触摸验证 Demo，开机自启。

### 改动

**lvgldemo.c (完全重写):**
- 主菜单: 3 个按钮 (Touch Color Test / Widgets Demo / Music Demo)
- Touch Color Test: 全屏矩形 + 触摸切换 10 种颜色 + 显示坐标
- Widgets / Music: 调用 LVGL 内置 `lv_demos_create()` API
- 保留 `lv_nuttx_uv_loop()` helper 函数 (libuv 支持)

**rcS.nsh:**
- 在 `luncher_mini &` 之后添加 `lvgldemo &`

### 构建问题
1. `lv_color_make()` 不是常量表达式 → 用 `lv_color_hex()` + 运行时赋值
2. `lv_nuttx_uv_loop()` 未定义 → 需要从原代码恢复 helper 函数
3. `int32_t` 格式化 → 用 `%ld` + `(long)` 强转

### 结果
✅ 开机显示菜单。触摸按钮切换 Demo。Touch Color Test 颜色切换正常。

---

## 第9次烧录: NXPlayer 音乐播放

### 目标
开机自动播放 laojie.mp3，通过 LVGL 菜单可选进入音乐 Demo。

### 改动

**nxplayer_main.c:**
- 在 `main()` 开头添加命令行参数支持:
  ```c
  if (argc > 1) {
      nxplayer_playfile(pplayer, argv[1], AUDIO_FMT_UNDEF, AUDIO_FMT_UNDEF);
      while (pplayer->state != 0) usleep(500 * 1000);
      nxplayer_release(pplayer);
      return 0;
  }
  ```

**rcS.nsh:**
- 在 `lvgldemo &` 之后添加 `nxplayer /data/laojie.mp3 &`

**UDISK/laojie.mp3:**
- MP3 文件 (5.3MB) 放入 `board/common/data/UDISK/`
- 打包进 `usrdata.fex` 数据分区
- 设备上路径: `/data/laojie.mp3`

### 结果
✅ 开机后同时显示 LVGL 菜幕 + 播放 laojie.mp3。

---

## 最终文件状态

| 文件 | 改动类型 | 说明 |
|------|----------|------|
| `gt911_iic_touch.c` | 修复 | I2C 失败时重置+重试 |
| `hal_twi.c` | 恢复 | 所有诊断已移除, 保留 Engine 模式 |
| `drv_twi.c` | 恢复 | 所有诊断已移除 |
| `lvgldemo.c` | 重写 | 菜单 + Touch Color Test + 内置 Demo |
| `nxplayer_main.c` | 修改 | 支持命令行参数播放文件 |
| `rcS.nsh` | 修改 | 添加 lvgldemo + nxplayer 自启 |
| `UDISK/laojie.mp3` | 新增 | MP3 音乐文件 |

## 烧录清单

```
1. m nsh c     # clean build
2. m nsh       # build
3. pack        # 打包固件
4. 固件位置: vendor/allwinnertech/lichee/out/r528s3/gemini-s1_nand/rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img
```
