# 7-26 施工日志 — LVGL Demo 触摸修复全记录

> 本日志记录了 2026-07-26 从"触摸完全不工作"到"坐标上报正常、按钮点击仍需排查"的完整施工过程。

---

## 环境信息

- **SoC:** Allwinner R528 双核 Cortex-A7
- **开发板:** r528s3-gemini-s1
- **屏幕:** BOE 1200x1920 MIPI DSI（显示颠倒 180°）
- **触摸 IC:** Goodix GT9271, TWI0, 地址 0x5D, RST=PB4, INT=PB5
- **音频:** ACodec, `/dev/audio/pcmC0D0p`
- **OS:** NuttX (OpenVela 分支)
- **编译:** `cd vendor/allwinnertech/lichee && source tools/scripts/envsetup.sh && lunch_nuttx 2 && m nsh c && pack`

---

## 施工过程

### Phase 1: 设备路径修复 + 开机自启

**问题:** LVGL 试图打开 `/dev/input/event0`，但 GT911 驱动注册为 `/dev/input0`

**改动:**
- `lvgldemo.c`: `info.input_path = "/dev/input/event0"` → `"/dev/input0"`
- `rcS.nsh`: 注释掉 `luncher_mini &`，`lvgldemo &` 作为唯一开机 APP

**验证:** LVGL 成功打开 `/dev/input0` ✅

---

### Phase 2: LCD 上电导致 I2C 失效

**问题:** LCD 上电后 GT9271 I2C 通信全部 NACK

**排查:**
- `ENG_RES` 诊断显示 `msgs_idx=32 stat=0xf8` (SLA+W NACK)
- 初始化时 detect 成功，LCD 上电后立即失败
- `gt911_reset_chip()` 重置后重试也失败

**根因:** `lcd_power_on()` 中 `sunxi_lcd_pin_cfg(sel, 1)` 改写了 PB2/PB3/PB4/PB5 的引脚功能

**修复:**
- 在 `gt911_worker()` 的 I2C 失败重试路径中，先恢复全部 4 个引脚的 pinmux，再重置芯片
- `hal_twi_soft_reset(0) + hal_twi_init(0)` 实验过但最终未采用（pinmux 恢复+芯片重置已足够）

**验证:** `[GT911] retry SUCCEEDED after re-reset!` ✅

---

### Phase 3: 坐标 mask (xh/yh 高4位)

**问题:** 触摸坐标值巨大（如 7427, 21507）

**根因:** `TOUCH_POINT_GET_X/Y` 宏没有 mask xh/yh 的高4位(状态标志位)

**修复:** `gt911_iic_touch.h` 中添加 `& 0x0F`

**验证:** 坐标从 7427 降到正常 12-bit 范围 ✅

---

### Phase 4: I2C burst 读偏移(关键 bug)

**问题:** 坐标值依然在 12-bit 范围但不正确

**根因:** 代码先读 0x814E(status 1字节)，再读 0x814F(触摸数据 N字节)，然后用 `priv->touch_buf[0] = status` 覆盖数据首字节。导致 `gt911_touch_data_s` 结构体偏移 1 字节

**修复:** 改为从 0x814E 一次 burst 读 `1 + touch_num * 8` 字节，删除 `priv->touch_buf[0] = status` 行

---

### Phase 5: maxpoint 不匹配

**问题:** `lower.maxpoint = 1` 但 `touch_register()` 传入 `GT911_MAX_TOUCH_POINTS=5`

**修复:** `g_gt911_touch.lower.maxpoint = GT911_MAX_TOUCH_POINTS`

---

### Phase 6: circbuf 尺寸不匹配(最终关键 bug)

**问题:** `sample->npoints = 1` 导致 `touch_event_notify()` 向 circbuf 写入 `SIZEOF_TOUCH_SAMPLE_S(1)` 字节，但 LVGL 读取 `SIZEOF_TOUCH_SAMPLE_S(5)` 字节。`read()` 返回的字节数不匹配，`touchscreen_read_sample()` 永远返回 false → LVGL 丢弃所有触摸数据

**修复:** `sample->npoints = GT911_MAX_TOUCH_POINTS` (5)

---

### Phase 7: 坐标缩放公式

**问题:** 硬编码 4096 作为缩放除数，但触摸屏实际分辨率不是 4095

**修复:** 初始化时从 GT9271 寄存器 0x8146-0x8149 读取真实分辨率，用于坐标缩放

---

## 发现的关键事实

### GT9271 芯片行为
- 上电后第一次 I2C 通信可能 NACK，但 `fail → 重置 → 重试` 可恢复
- LCD 上电(`sunxi_lcd_pin_cfg`)会改写 PB 组引脚的 pinmux
- 触摸坐标是 12-bit 值，但高4位含状态标志(xh/yh)
- 寄存器 0x8146-0x8149 存储芯片配置的 X/Y 分辨率

### NuttX 触摸子系统
- `touch_register()` 创建 `/dev/input0` 设备
- `touch_event()` 写入 circular buffer
- `SIZEOF_TOUCH_SAMPLE_S(n)` 依赖于 npoints，写入与读取尺寸必须一致
- `lower.maxpoint` 必须与 `touch_register()` 的 nums 参数一致

### LVGL NuttX 后端
- `lv_nuttx_touchscreen_create()` 打开设备，获取 maxpoint
- `touchscreen_read_sample()` 用 `read(fd, buf, SIZEOF_TOUCH_SAMPLE_S(maxpoint))` 读取
- 尺寸不匹配时返回 false，数据被丢弃

### WiFi 驱动
- `rtw_printf()` 是 Realtek WiFi 驱动的通用输出函数
- `DBG_INFO` 产生 `RTL871X:` 前缀
- 均在 `customer_rtos_service.c/h` 中控制

---

## 未解问题

### LVGL 点击无响应
坐标上报正常（已在日志中确认），但按钮 `LV_EVENT_CLICKED` 不触发。可能原因:
1. LVGL indev 层未正确处理触摸事件
2. `conv_touch()` 或 `process_single_touch()` 未正确设置 `data->state`
3. 按钮的 event callback 注册有问题
4. libuv 输入循环未正确轮询 fd

### 显示颠倒 180°
BOE 1200x1920 屏幕内容颠倒。可能原因:
1. `BOE_1200x1920.c` 中 `lcd_cfg_panel_info()` 的扫描方向参数
2. TCON/DE 控制器的输出方向配置
3. MIPI DSI 命令 `0x36` (Memory Data Access Control)

### WiFi DHCP 失败
日志显示 `ERROR: netlib_obtain_ipv4addr() failed`，虽然最终获得 IP 10.0.0.2，但 DHCP 流程有问题。可能与 `wapi.conf` 配置有关。
