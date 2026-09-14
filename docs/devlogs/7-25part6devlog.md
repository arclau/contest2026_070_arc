# MiuMiu 施工日志 — 2026-07-25

> 本日志记录了 2026-07-25 从"触摸不工作"到"开机播放音乐"的完整施工过程。

---

## 角色设定

本日志由 AI 驱动开发工程师记录。定位：精通 NuttX/Linux 嵌入式系统、I2C 协议、GT9271 触摸控制器、LVGL 图形框架的全栈 BSP 工程师。

---

## 环境信息

- **SoC:** Allwinner R528 双核 Cortex-A7
- **开发板:** r528s3-gemini-s1
- **屏幕:** BOE 1200x1920 MIPI DSI
- **触摸 IC:** Goodix GT9271 (GT911 寄存器映射)
- **I2C:** TWI0, 地址 0x5D, RST=PB4, INT=PB5
- **音频:** ACodec, `/dev/audio/pcmC0D0p`
- **存储:** 128MB NAND + NVMe ext4 (`/data`)
- **OS:** NuttX (OpenVela 分支)
- **编译:** `cd vendor/allwinnertech/lichee && source tools/scripts/envsetup.sh && lunch_nuttx 2 && m nsh c && pack`

---

## 已完成工作

### 1. GT9271 触摸修复 ✅

**问题:** GT9271 开机后第一次 I2C 读取必然 NACK (SLA+W 被拒绝)

**根因:** GT9271 芯片内部行为。不是总线问题、不是驱动问题、不是上拉问题。芯片需要两个重置周期才能稳定。

**修复:** `gt911_worker()` 中 I2C 失败时执行 `gt911_reset_chip()` + 立即重试。

**验证:**
- 第一次读取: msgs_idx=32 (0x20 = SLA+NACK) ← 芯片拒绝
- 重置后重试: msgs_idx=1 (成功) ← 芯片就绪
- 之后所有读取: 全部成功, 零失败

**硬件验证:**
- RST 上拉 (4.7K to 3.3V): 无效
- INT 上拉 (4.7K to 3.3V): 有害 (两个地址都 NACK)
- 结论: 保持原始电路, 不加任何上拉

### 2. LVGL 触摸 Demo ✅

**改动:** `lvgldemo.c` 完全重写

**功能:**
- 主菜单: Touch Color Test / Widgets Demo / Music Demo
- Touch Color Test: 点击屏幕切换颜色 (10种), 显示触摸坐标
- Widgets / Music: 调用 LVGL 内置 `lv_demos_create()` API

**开机自启:** `rcS.nsh` 添加 `lvgldemo &`

### 3. NXPlayer 音乐播放 ✅

**改动:** `nxplayer_main.c` 添加命令行参数支持

**功能:**
- `nxplayer /data/laojie.mp3` → 播放文件后退出
- 原交互模式不受影响 (无参数时行为不变)

**数据:** `laojie.mp3` (5.3MB) 打包进 `usrdata.fex` → 设备 `/data/laojie.mp3`

**开机自启:** `rcS.nsh` 添加 `nxplayer /data/laojie.mp3 &`

### 4. 诊断代码清理 ✅

**已移除:**
- `hal_twi.c` 中所有 `[ENG_PRE]`, `[ENG_RES]`, `[TWI] PRE_XFER`, `[TWI] XFER FAIL` 诊断块
- `drv_twi.c` 中所有诊断打印
- `drv_twi.c` 中 `hal_twi_set_drv_mode()` 调用和 BUSEN 检查

**已保留:**
- `hal_twi.c` 中 `#if 1` (Engine 模式)
- `gt911_iic_touch.c` 中 `fail → 重置 → 重试` 修复

---

## 发现的关键事实

### GT9271 芯片行为
- 开机后第一次 I2C 通信: 必然 NACK (SLA+W 被拒绝)
- 第一次重置后: 立即恢复正常, 之后 100% 成功
- 不受 RST 上拉影响, 不受 500ms 延迟影响
- INT 上拉会干扰 I2C 通信 (两个地址都 NACK)

### msgs_idx 含义
- `msgs_idx=32` = 0x20 = TWI 状态 `SLA+W transmitted, NOT ACK received`
- `msgs_idx=1` = 传输成功

### 音频设备路径
- `/dev/audio/pcmC0D0p` — 默认播放设备
- nxplayer 自动搜索 `/dev/audio/` 目录

### LVGL 注意事项
- `lv_color_make()` 不是 C 常量表达式, 不能用于 static 数组初始化
- `lv_nuttx_uv_loop()` 是本地 helper 函数, 不是 LVGL API
- `int32_t` 在 printf 中需 `%ld` + `(long)` 强转

---

## 关键文件

| 文件 | 说明 |
|------|------|
| `boards/r528/drivers/gt911_iic_touch.c` | GT9271 触摸驱动 (修复在这里) |
| `chips/r528/drivers/rtos-hal/hal/source/twi/hal_twi.c` | TWI HAL (诊断已移除) |
| `chips/r528/drv/twi/drv_twi.c` | TWI lower-half (诊断已移除) |
| `apps/examples/lvgldemo/lvgldemo.c` | LVGL Demo (重写) |
| `apps/system/nxplayer/nxplayer_main.c` | NXPlayer (命令行参数支持) |
| `boards/r528/r528s3-gemini-s1/src/etc/init.d/rcS.nsh` | 启动脚本 |

---

## 未解问题

### GT9271 首次 NACK 的物理原因
为什么 GT9271 在开机后第一次 I2C 通信时 NACK？可能的原因:
1. 芯片内部上电复位需要额外时间
2. 第一次通信前需要特定的初始化序列
3. 芯片在 detect 期间进入了特殊状态

**现状:** 不影响使用。`fail → 重置 → 重试` 机制完美解决。

### NXPlayer 交互模式
NXPlayer 是交互式 shell。当前修改支持 `nxplayer <file>` 播放后退出。如需更复杂的播放控制 (暂停/下一首/音量)，需要进一步开发。
