# 7-26 接力 Prompt — LVGL Demo 触摸修复与当前问题

> 本文档是给下一任 AI 助手的完整任务上下文。请先**完整阅读本文档**以及同目录下的 `7-26-devlog.md`、`7-26-construction.md`、`7-26-thinking.md`，然后按照"当前问题"逐步执行。不要跳过任何验证步骤。

---

## 1. 项目背景

### 硬件

- **SoC:** Allwinner R528 (sun8iw20)，双核 Cortex-A7
- **开发板:** r528s3-gemini-s1
- **屏幕:** BOE 1200x1920 MIPI DSI（**显示方向颠倒 180°，需要修复**）
- **触摸 IC:** Goodix GT9271 (兼容 GT911 寄存器映射)
- **I2C 总线:** TWI0 (PB2=SDA, PB3=SCL)
- **I2C 地址:** 0x5D (reset 时 INT=LOW 选择)
- **RST/INT:** PB4(RST), PB5(INT) — **无外部上拉**
- **音频:** Allwinner ACodec, `/dev/audio/pcmC0D0p`
- **存储:** 128MB NAND + NVMe ext4 (`/data`)

### 软件

- **OS:** NuttX (OpenVela 分支)
- **工作目录:** `/data/openvela`
- **编译:** `cd vendor/allwinnertech/lichee && source tools/scripts/envsetup.sh && lunch_nuttx 2 && m nsh c && pack`
- **输出镜像:** `lichee/out/r528s3/gemini-s1_nand/rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img`
- **烧录方式:** USB FEL 烧录

### 开机自启

- `vendor/allwinnertech/boards/r528/r528s3-gemini-s1/src/etc/init.d/rcS.nsh`
- 自启顺序: `lvgldemo &` → `nxplayer /data/laojie.mp3 &`
- `luncher_mini` 已注释掉，由 `lvgldemo` 替代

---

## 2. 已完成的修复

### 2.1 GT9271 触摸 I2C 通信修复 ✅

**问题:** LCD 上电 (`sunxi_lcd_pin_cfg`) 改写了 PB2/PB3/PB4/PB5 的 pinmux，导致 TWI 控制器无法与 GT9271 通信

**修复:** 在 `gt911_worker()` 的 I2C 失败重试路径中，先**恢复全部 4 个引脚的 pinmux**，再重置芯片，再重试:

```c
hal_gpio_pinmux_set_function(I2C_SDA, I2C_PIN_MUXSEL);   // PB2
hal_gpio_pinmux_set_function(I2C_SCL, I2C_PIN_MUXSEL);   // PB3
hal_gpio_pinmux_set_function(RST, GPIO_MUXSEL_OUT);       // PB4
hal_gpio_set_direction(RST, GPIO_DIRECTION_OUTPUT);
hal_gpio_pinmux_set_function(INT, GPIO_MUXSEL_OUT);       // PB5
hal_gpio_set_direction(INT, GPIO_DIRECTION_OUTPUT);
gt911_reset_chip(priv);
```

**关键文件:** `vendor/allwinnertech/boards/r528/drivers/gt911_iic_touch.c`

### 2.2 GT9271 触摸坐标解析修复 ✅

**问题 1:** `TOUCH_POINT_GET_X/Y` 宏缺少 `& 0x0F` mask，xh/yh 的高 4 位状态标志污染了坐标值

**修复:** 在 `gt911_iic_touch.h` 中添加 mask:
```c
#define TOUCH_POINT_GET_X(t)  ((((t).xh & 0x0F) << 8) | (t).xl)
#define TOUCH_POINT_GET_Y(t)  ((((t).yh & 0x0F) << 8) | (t).yl)
```

**问题 2:** I2C 读取触摸数据时，先单独读 0x814E(status)，再读 0x814F(数据)。然后代码用 `priv->touch_buf[0] = status` 覆盖了第一个触摸点数据的首字节，导致整个结构体偏移 1 字节

**修复:** 改为一次 burst 读从 0x814E 开始，包含 status + 触摸数据:
```c
// 之前: gt911_i2c_read(priv, REG_COORD_ADDR + 1, buf, touch_num * 8);
// 之后: gt911_i2c_read(priv, REG_COORD_ADDR, buf, 1 + touch_num * 8);
// 并删除: priv->touch_buf[0] = status;
```

**问题 3:** `lower.maxpoint = 1` 导致 LVGL 读到的触摸点数为 1，但 circbuf 写入不同尺寸

**修复:** `.lower.maxpoint = GT911_MAX_TOUCH_POINTS` (5) 与 `touch_register()` 参数一致

**问题 4:** `sample->npoints = 1` 导致 `touch_event_notify()` 向 circbuf 写入 `SIZEOF_TOUCH_SAMPLE_S(1)`，但 LVGL 读取 `SIZEOF_TOUCH_SAMPLE_S(5)`。尺寸不匹配 → `touchscreen_read_sample()` 永远返回 false → LVGL 丢弃触摸数据

**修复:** `sample->npoints = GT911_MAX_TOUCH_POINTS` (5) 使写入/读取尺寸匹配

**问题 5:** 坐标缩放硬编码 4096(12-bit max)，但实际触摸屏分辨率不同(X 约 1840, Y 约 1910)

**修复:** 在初始化时从寄存器 0x8146-0x8149 读取芯片实际分辨率，用于缩放:
```c
uint32_t div_x = (uint32_t)priv->touch_max_x + 1;
uint32_t div_y = (uint32_t)priv->touch_max_y + 1;
sample->point[i].x = raw_x * LCD_WIDTH / div_x;
sample->point[i].y = raw_y * LCD_HEIGHT / div_y;
```

### 2.3 WiFi 日志静音 ✅

**改动:** 在 `customer_rtos_service.c` 中将 `rtw_printf()` 函数体改为空函数；在 `customer_rtos_service.h` 中将 `DBG_INFO` 宏定义为空

### 2.4 设备路径修复 ✅

**改动:** `lvgldemo.c` 中 `info.input_path` 从 `/dev/input/event0` 改为 `/dev/input0` (与 GT911 驱动注册路径一致)

### 2.5 开机自启 ✅

**改动:** `rcS.nsh` 中注释掉 `luncher_mini &`，由 `lvgldemo &` 替代

---

## 3. 当前状态

### ✅ 已完成验证

- GT9271 I2C 通信正常（LCD 上电后可恢复）
- 触摸坐标在屏幕范围内（已确认四角坐标）
- LVGL 成功打开 `/dev/input0`
- WiFi 驱动工作正常（DHCP 获取 IP 10.0.0.2）
- 编译打包流程正常

### ❌ 未解决

| 问题 | 描述 |
|------|------|
| **LVGL 点击无响应** | 坐标上报正常，但按钮 `LV_EVENT_CLICKED` 不触发。怀疑 LVGL 层的事件处理或 indev 初始化有问题 |
| **显示颠倒 180°** | BOE 1200x1920 屏幕内容上下颠倒。怀疑 LCD 初始化参数或 DE 控制器配置有误 |

---

## 4. 关键文件列表

| 文件 | 说明 | 修改状态 |
|------|------|----------|
| `vendor/.../boards/r528/drivers/gt911_iic_touch.c` | GT9271 触摸驱动 | ✅ 已修改 |
| `vendor/.../boards/r528/drivers/gt911_iic_touch.h` | GT9271 头文件（宏定义） | ✅ 已修改 |
| `vendor/.../chips/r528/drivers/rtos-hal/hal/source/twi/hal_twi.c` | TWI HAL (ENG_RES 诊断已移除) | ✅ 已修改 |
| `apps/examples/lvgldemo/lvgldemo.c` | LVGL Demo 主程序 | ✅ 已修改 |
| `vendor/.../drivers/realtek_ieee80211/os/customer_rtos/customer_rtos_service.c` | WiFi rtw_printf 静音 | ✅ 已修改 |
| `vendor/.../drivers/realtek_ieee80211/.../customer_rtos_service.h` | WiFi DBG_INFO 静音 | ✅ 已修改 |
| `vendor/.../boards/r528/r528s3-gemini-s1/src/etc/init.d/rcS.nsh` | 开机启动脚本 | ✅ 已修改 |

---

## 5. 下一步建议

### 5.1 LVGL 点击问题排查方向

1. **确认触摸数据是否到达 LVGL indev**: 在 `lv_nuttx_touchscreen.c` 的 `touchscreen_read()` 中加入 `printf` 或 LVGL LOG 输出，看 `data->state` 是否被设为 `LV_INDEV_STATE_PRESSED`
2. **检查 `conv_touch()` → `process_single_touch()`**: 确认 `sample->point[0].flags` 的 `TOUCH_DOWN`(0x01) 位是否置位
3. **检查 LVGL event dispatch**: `lv_event_send()` 是否被正确调用
4. **检查按钮的 `LV_EVENT_CLICKED` 回调**: 确认 `menu_btn_cb` 是否正确注册
5. **确认 `CONFIG_LV_USE_NUTTX_LIBUV` 编译配置**: 如果用了 libuv，确保 `lv_nuttx_uv_input_init` 的 fd 监控正常

### 5.2 显示颠倒问题排查方向

1. 检查 `BOE_1200x1920.c` 中 `lcd_cfg_panel_info()` 的 `scan` 和 `lcd_rb_swap` 参数
2. 检查 DE (Display Engine) 控制器配置中的输出方向寄存器
3. 可能导致颠倒的原因: TCON 输出方向设置、DE 层翻转标记、DSI 命令 `0x36`(Memory Data Access Control)

---

## 6. 调试技巧

- 添加诊断代码到 `hal_twi_engine_do_xfer()` 的 `sem_wait()` 之后、`soft_reset` 之前，可捕获 TWI 状态寄存器
- 当前诊断 `[GT911] TOUCH touch_num=%d coord=(%d,%d) state=%d` 显示触摸坐标
- 编译: `mnuttx nsh` (增量) / `mnuttx nsh c && mnuttx nsh` (全量)
- 打包: `pack`
- 固件输出: `lichee/out/r528s3/gemini-s1_nand/rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img`

---

## 7. 文档索引

| 文档 | 说明 |
|------|------|
| `7-26-handoff.md` | 本文档 (接力 Prompt) |
| `7-26-devlog.md` | 施工日志 |
| `7-26-construction.md` | 施工详情 (按烧录次序) |
| `7-26-thinking.md` | 调试方法论 |
| `miumiu-handoff.md` | 前一轮工作接力文档 |
| `miumiu.md` | MiuMiu 开发日志 |
| `miumiu-construction.md` | MiuMiu 施工逻辑 |
| `miumiu-thinking.md` | MiuMiu 调试思考方式 |
| `miumiu-devlog.md` | MiuMiu 施工日志 |
| `devlog0725-sdk-capabilities.md` | R528 SDK 能力手册 |
