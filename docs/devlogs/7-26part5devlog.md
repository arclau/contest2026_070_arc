# 7-26 施工逻辑 — LVGL Demo 触摸修复

> 本文档记录从"LVGL Demo 启动但触摸完全不工作"到"坐标上报正常但按钮点击无效"的完整施工路径。按烧录次序组织，每次烧录明确：改了什么、验证了什么、结论是什么。

---

## 总览

```
第1次烧录: 设备路径 + 开机自启
第2次烧录: LCD上电I2C失效 → pinmux恢复
第3次烧录: 坐标mask(xh/yh高4位) + I2C burst读偏移
第4次烧录: maxpoint + circbuf尺寸匹配 + 坐标缩放
第5次烧录: 从芯片寄存器读取真实分辨率缩放
```

---

## 第1次烧录: 设备路径 + 开机自启

### 改动
- `lvgldemo.c`: `info.input_path` → `"/dev/input0"`
- `rcS.nsh`: 注释 `luncher_mini &`，`lvgldemo &` 自启

### 结果
- LVGL 成功打开 `/dev/input0`，`maxpoint=1` ✅
- **但 I2C 通信失败** → 触摸完全无响应 ❌

### 结论
设备路径正确，但 GT9271 I2C 在 LCD 上电后不工作。

---

## 第2次烧录: LCD上电导致I2C失效 + pinmux恢复

### 诊断方法
在 `hal_twi_engine_do_xfer()` 的 `sem_wait()` 之后添加 `[ENG_RES]` 诊断:
```
[ENG_RES] port=0 dev=0x5D msgs_idx=32 stat=0xf8
```
- `msgs_idx=32 = 0x20` = SLA+W NACK → 芯片拒绝应答
- `stat=0xf8` = TWI 控制器正常（空闲状态）

### 根因定位
`lcd_power_on()` 中 `sunxi_lcd_pin_cfg(sel, 1)` 改写了 PB2/PB3/PB4/PB5 引脚功能:
- PB2(SDA)/PB3(SCL) 不再是 TWI 功能 → I2C 信号到不了 GT9271
- PB4(RST)/PB5(INT) 不再是 GPIO 输出 → `gt911_reset_chip()` 无效

### 修复
在 `gt911_worker()` 重试路径中，恢复全部 4 个引脚 pinmux:

```c
hal_gpio_pinmux_set_function(I2C_SDA, I2C_PIN_MUXSEL);
hal_gpio_pinmux_set_function(I2C_SCL, I2C_PIN_MUXSEL);
hal_gpio_pinmux_set_function(RST, GPIO_MUXSEL_OUT);
hal_gpio_set_direction(RST, GPIO_DIRECTION_OUTPUT);
hal_gpio_pinmux_set_function(INT, GPIO_MUXSEL_OUT);
hal_gpio_set_direction(INT, GPIO_DIRECTION_OUTPUT);
gt911_reset_chip(priv);
```

### 验证
```
[GT911] retry SUCCEEDED after re-reset!   ✅
```

### 结论
I2C 通信恢复，但按钮仍无反应——问题不在驱动通信层。

---

## 第3次烧录: 坐标mask + I2C burst读偏移

### 问题1: 坐标超范围
日志显示 `coord=(7427,6146)` 远超屏幕 1200x1920。

**根因:** `TOUCH_POINT_GET_X/Y` 宏未 mask xh/yh 的高4位状态标志位

**修复:** `gt911_iic_touch.h`:
```c
#define TOUCH_POINT_GET_X(t)  ((((t).xh & 0x0F) << 8) | (t).xl)
#define TOUCH_POINT_GET_Y(t)  ((((t).yh & 0x0F) << 8) | (t).yl)
```

### 问题2: 坐标依然异常
mask 后坐标值在 12-bit 范围但依然不对。

**根因:** I2C 读触摸数据的方式导致结构体偏移 1 字节:
```
之前: 读0x814F → touch_buf[0]=TrackID
      touch_buf[0] = status  ← 覆盖TrackID!
      touch[0].xh = 实际是Y低字节 → 坐标完全错误

之后: 读0x814E(含status) → touch_buf[0]=status ✅
      touch[0].xh = 真实X高字节 ✅
```

**修复:**
```c
// 之前: gt911_i2c_read(priv, REG_COORD_ADDR + 1, buf, touch_num * 8);
// 之后: gt911_i2c_read(priv, REG_COORD_ADDR, buf, 1 + touch_num * 8);
// 删除: priv->touch_buf[0] = status;
```

### 验证
坐标值在合理范围(36,15)~(537,557)，覆盖屏幕四角 ✅

### 结论
坐标正确了，但按钮依然不响应——问题在上层。

---

## 第4次烧录: maxpoint + circbuf尺寸 + 坐标缩放

### 问题1: maxpoint 不匹配
```c
.lower.maxpoint = 1;                    // 告诉LVGL只支持1个点
// touch_register(..., GT911_MAX_TOUCH_POINTS)  // 注册为5个点
```

**修复:** `.lower.maxpoint = GT911_MAX_TOUCH_POINTS` (5)

### 问题2: circbuf 尺寸不匹配 ← 关键bug！
`gt911_touch_process_event_one()` 中:
```c
sample->npoints = 1;  // 导致 touch_event() 写入 SIZEOF_TOUCH_SAMPLE_S(1)
```
LVGL 读取 `SIZEOF_TOUCH_SAMPLE_S(5)`。`1 ≠ 5` → `touchscreen_read_sample()` 永远返回 false → LVGL 丢弃触摸数据。

**修复:** `sample->npoints = GT911_MAX_TOUCH_POINTS` (5)

### 问题3: 坐标缩放硬编码
硬编码 4096 作为除数，但实际触摸屏 X/Y 分辨率不是 4095。

**修复:** 从 GT9271 寄存器 0x8146-0x8149 读取实际分辨率。

### 验证
四角坐标覆盖范围为 (36,15)~(537,557)，但按钮仍无响应 ❌

---

## 第5次烧录: 分辨率寄存器读取(当前)

### 改动
在 `gt911_control_initialize()` 中 + `gt911_touch_process_data()` 中:
```c
// 初始化时读分辨率:
gt911_i2c_read(priv, REG_COORD_RESOLUTION, res_buf, 4);
priv->touch_max_x = res_buf[0] | (res_buf[1] << 8);
priv->touch_max_y = res_buf[2] | (res_buf[3] << 8);

// 缩放时用芯片实际分辨率:
uint32_t div_x = priv->touch_max_x + 1;
uint32_t div_y = priv->touch_max_y + 1;
sample->point[i].x = raw_x * LCD_WIDTH / div_x;
sample->point[i].y = raw_y * LCD_HEIGHT / div_y;
```

### 待验证
见 `7-26-handoff.md` 第5节"下一步建议"。

---

## 最终文件状态

| 文件 | 改动 | 说明 |
|------|------|------|
| `gt911_iic_touch.c` | 多处修复 | pinmux恢复、burst读、maxpoint、npoints、分辨率读取、坐标缩放 |
| `gt911_iic_touch.h` | mask修复 | `xh & 0x0F`, `yh & 0x0F` |
| `lvgldemo.c` | 路径修复 | `/dev/input0` |
| `rcS.nsh` | 自启修改 | lvgldemo替代luncher_mini |
| `customer_rtos_service.c` | WiFi静音 | rtw_printf空函数 |
| `customer_rtos_service.h` | WiFi静音 | DBG_INFO空宏 |
| `hal_twi.c` | 诊断已移除 | ENG_RES已删 |

## 烧录清单

```
1. m nsh c      # clean build
2. m nsh        # build
3. pack         # 打包固件
4. 固件: lichee/out/r528s3/gemini-s1_nand/rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img
```
