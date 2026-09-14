# 7-26 思考方式 — LVGL Demo 触摸调试方法论

> 本文档定义了本阶段调试中采用的**思维方式**和**推理纪律**。

---

## 核心信条

- **"我不在猜，我是在排除。"** 每一步都有一个可验证的预测。
- **"不要治疗症状，要找到病原体。"** 如果一个补丁没解决根因，就不要提交。
- **"驱动问题、数据流问题、UI 问题是三层独立的问题。"** 先分清是哪一类。
- **"每一层的数据格式必须上下游一致。"** 写入 circbuf 的尺寸必须等于读取的尺寸。

---

## 思维模型：四层定位法

触摸问题从物理点击到 UI 响应，经过四层：

### 第一层：硬件/I2C 通信层

```
物理触摸 → GT9271 芯片 → I2C 总线 → TWI 控制器
```

**关键判断：** I2C 读返回值是否正常？
- `ret=0` → 通信正常
- `ret<0` → 通信失败（NACK、超时、总线错误）

**诊断方法：** 在 `hal_twi_engine_do_xfer()` 的 `sem_wait()` 之后读取 TWI_STAT_REG
- `stat=0xf8` → 控制器空闲（已完成操作）
- `msgs_idx=1` → 传输成功
- `msgs_idx=32 (0x20)` → SLA+W NACK（芯片不应答）

**常见根因：**
- pinmux 被其他初始化改写（如 `sunxi_lcd_pin_cfg`）
- TWI 控制器内部状态被破坏
- 芯片电源或复位时序问题

### 第二层：触摸驱动层

```
I2C 数据 → gt911_worker() → gt911_touch_process_data()
         → gt911_touch_process_event_one() → touch_event()
```

**关键检查点：**

1. **Raw 数据读取**: `gt911_i2c_read()` 的寄存器地址和长度
   - status 必须从 0x814E 读
   - 触摸数据必须从 0x814E 开始 burst 读（含 status）
   - 读长度 = `1 + touch_num * GT911_POINT_SIZE`

2. **坐标解析宏**: `TOUCH_POINT_GET_X/Y` 必须有 `& 0x0F` mask

3. **数据缓冲区对齐**: `priv->touch_buf[0]` 必须是 status 字节，之后紧接 `touch_point_s` 数组

4. **lower.maxpoint**: 必须与 `touch_register()` 的 `nums` 参数一致

5. **sample->npoints**: 必须是 `GT911_MAX_TOUCH_POINTS`(5)，确保 `SIZEOF_TOUCH_SAMPLE_S(npoints)` 与 LVGL 读取尺寸匹配

### 第三层：NuttX 触摸子系统层

```
touch_event() → circbuf_overwrite() → touch_read()
```

**关键检查点：**

1. `touch_event_notify()` 写入 circbuf 的尺寸: `SIZEOF_TOUCH_SAMPLE_S(sample->npoints)`
2. LVGL `touchscreen_read_sample()` 读取的尺寸: `SIZEOF_TOUCH_SAMPLE_S(maxpoint)`
3. **这两个尺寸必须严格相等！** 否则 `read()` 返回的字节数 != 期望值 → 数据被丢弃

**`SIZEOF_TOUCH_SAMPLE_S(n)` 公式:**
```c
sizeof(struct touch_sample_s) + ((n) - 1) * sizeof(struct touch_point_s)
```

### 第四层：LVGL 应用层

```
touchscreen_read() → conv_touch() → process_single_touch()
→ indev 处理 → lv_event_send() → widget 回调
```

**关键检查点：**
1. `data->state` 是否被设为 `LV_INDEV_STATE_PRESSED`?
2. `data->point.x/y` 是否在屏幕范围内？
3. LVGL event dispatch 是否正确？
4. libuv 输入循环是否正常轮询 fd？

---

## 心态纪律

### 规则 1: 一次只改一个变量

同时改多个东西，你不知道哪个有效。
- ❌ 同时改 pinmux + npoints + maxpoint + 缩放
- ✅ 先修复 pinmux → 验证 I2C 通信恢复 → 再修坐标解析 → 再修尺寸匹配

### 规则 2: 诊断代码精准投放

诊断插入位置的铁律：
- 必须在**故障发生的精确时刻**读取寄存器
- 不要在错误处理之后读（`soft_reset` 已经改变了状态）
- 不要在函数入口读（那时故障还没发生）

**本项目的关键诊断位置：**
```
hal_twi_engine_do_xfer():
  sem_wait()      ← 等中断
  [ENG_RES]       ← 在这里读！故障刚发生，状态未被篡改
  soft_reset      ← 这之后状态就变了
```

### 规则 3: 修复要在最简单的层面

- pinmux 问题 → 在驱动层恢复 pinmux（不要改 TWI HAL）
- 坐标解析问题 → 在宏定义中修复（不改上层）
- 尺寸匹配问题 → 在驱动层设置正确的 npoints（不改 LVGL 后端）

### 规则 4: 诊断代码不是永久代码

诊断代码的使命是**回答问题**。一旦问题回答完毕，立刻清除。

### 规则 5: 上下游尺寸一致性

这是本项目最重要的教训——NuttX 触摸子系统的 circbuf 写入和读取尺寸必须完全一致。如果写入 `SIZEOF_TOUCH_SAMPLE_S(1)` 而读取 `SIZEOF_TOUCH_SAMPLE_S(5)`，数据永远无法通过 `read()` 传递到用户态。

---

## 本阶段特有陷阱

### 陷阱 1: LCD 初始化干扰 pinmux

`lcd_power_on()` 中的 `sunxi_lcd_pin_cfg(sel, 1)` 会改写 PB 组引脚的 pinmux 寄存器。这不仅影响 I2C 引脚(PB2/PB3)，也会影响 RST(PB4)/INT(PB5)。

**教训:** 恢复 pinmux 时，必须恢复所有被影响的引脚，不能只恢复 I2C 那 2 个。

### 陷阱 2: I2C 读触摸数据的方式

GT9271 的触摸数据在寄存器 0x814E(address) + 0x814F+(data)。必须先读 status(0x814E)，然后在一次 burst 中读 status + 数据(0x814E 开始)。

**教训:** 不能分成两次 I2C read(先读 status 再读数据)然后在驱动层拼装，因为 `priv->touch_buf[0] = status` 的覆盖会破坏结构体对齐。

### 陷阱 3: 12-bit ≠ 4095

GT9271 虽然是 12-bit ADC，但触摸屏的实际传感器分辨率通常不等于 4095。必须从芯片寄存器 0x8146-0x8149 读取真实分辨率。

**教训:** 不硬编码缩放除数。

### 陷阱 4: maxpoint/npoints 传递链

```
gt911驱动:  .lower.maxpoint = GT911_MAX_TOUCH_POINTS
       → touch_register(..., GT911_MAX_TOUCH_POINTS)
       → ioctl(TSIOC_GETMAXPOINTS) 返回 upper->maxpoint = nums
       → LVGL: touchscreen->maxpoint = upper->maxpoint
       → touchscreen_read_sample() 读 SIZEOF_TOUCH_SAMPLE_S(maxpoint)
       
gt911驱动:  gt911_touch_process_event_one()
       → sample->npoints = GT911_MAX_TOUCH_POINTS
       → touch_event_notify() 写 SIZEOF_TOUCH_SAMPLE_S(sample->npoints)
       
条件: sample->npoints == touchscreen->maxpoint (必须相等!)
```

---

## 推荐的后续调试步骤

### 步骤 1: 确认触摸数据到达 LVGL indev

在 `lv_nuttx_touchscreen.c` 的 `touchscreen_read()` 或 `process_single_touch()` 中添加 LVGL LOG：
```c
LV_LOG_USER("touch: flags=0x%02x xy=(%d,%d) state=%d",
            touch_flags, data->point.x, data->point.y,
            touchscreen->last_state);
```

### 步骤 2: 确认 indev 注册

在 `lv_nuttx_touchscreen_create()` 中，确认 `lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER)` 被调用，且 indev 的 `read_cb` 指向 `touchscreen_read`。

### 步骤 3: 确认 libuv 输入轮询

在 `lv_nuttx_uv_input_init()` 中确认 `uv_poll_start()` 正确监控了 touch fd。

### 步骤 4: 检查按钮回调

在 `menu_btn_cb()` 中添加 `printf`，确认回调是否被调用。
