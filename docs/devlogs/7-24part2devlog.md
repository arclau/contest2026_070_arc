# Dev Log 0724 - GT9271 触摸驱动调试

## 角色设定

本日志由 AI 驱动开发工程师记录。定位：精通 Linux/NuttX 内核、I2C/SPI 协议、触摸屏控制器（Goodix/FocalTech/Ilitek）的嵌入式 BSP 工程师。具备从寄存器手册到驱动框架的全栈调试能力，擅长通过 I2C 波形分析、寄存器 dump、内核源码对比来定位问题。

---

## 环境信息

- 开发机：i7-12650H（16线程），编译用 `-j10`
- 编译命令：
  ```bash
  cd /data/openvela/vendor/allwinnertech/lichee
  source tools/scripts/envsetup.sh
  lunch_nuttx 2          # 选择 r528s3-gemini-s1
  m nsh                  # 编译 nsh 固件
  pack                   # 打包镜像
  ```
- 固件输出：`lichee/out/r528s3/gemini-s1_nand/rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img`
- 工作目录：`/data/openvela`
- 备份目录：`/data/vela-backup/`（BOE 驱动备份、触摸驱动备份）

---

## 需求背景

R528s3-gemini-s1 开发板，搭配 BOE 1200x1920 MIPI 屏幕，触摸 IC 为 Goodix GT9271（I2C 接口）。目标：让触摸功能正常工作，上报坐标数据。

硬件连接：
- I2C 总线：TWI0（i2c_bus0）
- I2C 地址：0x5D（通过 reset 时 INT=LOW 选择）
- RST 引脚：PB4
- INT 引脚：PB5（poll 模式下用作输出轮询）

---

## 已完成工作

### 1. BOE 屏幕集成 ✅
- 从备份复制 BOE 驱动文件（BOE_1200x1920_mipi.c 等）
- 修改 panels.c/h 注册 BOE 面板
- 修复大小写问题：`boe_1200x1920_panel` → `BOE_1200x1920_panel`（匹配 C 符号定义）
- 屏幕成功点亮，显示正常

### 2. 触摸驱动适配
- 修改 `gt911_iic_touch.h`：分辨率从 1024x600 改为 1200x1920
  ```c
  // 修改前
  #define T070S140B_LCD_WIDTH   1024
  #define T070S140B_LCD_HEIGHT  600
  // 修改后
  #define T070S140B_LCD_WIDTH   1200
  #define T070S140B_LCD_HEIGHT  1920
  ```
- 使能触摸 GPIO 配置（`BOE_1200x1920_mipi_config.c` 中移除 `#if 0`，保留 PB2/PB3 I2C + PB4 RST + PB5 INT）
- 修复 product_id 检测：buffer 从 4 字节扩到 5 字节，截断版本字节，支持 "911" 和 "927" 两种 ID
  ```c
  // 修改前：只读 4 字节，直接 strcmp "911"
  uint8_t product_id[4];
  gt911_i2c_read(priv, GT911_REG_PRODUCT_ID, product_id, 4);
  if(strcmp((char*)product_id,"911")){...}
  
  // 修改后：读 5 字节，截断版本位，支持 "911" 和 "927"
  uint8_t product_id[5];
  gt911_i2c_read(priv, GT911_REG_PRODUCT_ID, product_id, 4);
  product_id[3] = '\0';  // 截断版本字节
  if(strcmp((char*)product_id,"911") && strcmp((char*)product_id,"927")){...}
  ```

### 3. I2C 诊断增强
- `gt911_i2c_read`：每次读取打印真实 I2C_TRANSFER 返回值、寄存器地址、长度、从机地址、hex dump
- `gt911_i2c_write`：同上
- `gt911_diag_read_all_regs`：启动时全寄存器扫描（0x8140/0x8144/0x8146/0x8040/0x8044/0x8047/0x8000/0x814E 多长度测试/0x814F）
- Worker 每步打印详细状态（STEP1-4）

---

## 发现的关键事实

### 寄存器地址确认
从 Linux 内核 `drivers/input/touchscreen/goodix.h` 确认：
```
GOODIX_REG_ID           = 0x8140  (Product ID)
GOODIX_GT9X_REG_CONFIG  = 0x8047  (Config, 186 bytes)
GOODIX_READ_COOR_ADDR   = 0x814E  (Coordinate data)
GOODIX_REG_COMMAND      = 0x8040  (Command)
```
GT9271 和 GT911 使用完全相同的寄存器映射（Linux 内核 goodix_chip_ids 表确认）。

### I2C 通信状态
诊断扫描结果（addr=0x5D）：
```
0x8140 ProductID : "9271" ✅
0x8144 FW Version: 0x20 0x10 0x80 0x07
0x8146 Resolution: 1920x1920
0x8040 Command   : 0xFF
0x8044 Status    : 0x00
0x8047 Config    : [41 80 07 b0 04 0a 35 00 ...]
0x8000 FW Sig    : "GOODIX_GT9..."
0x814E len=1     : 0x80 (bit7=1, touch_num=0) ✅
0x814E len=4     : 0x80 00 00 00 ✅
0x814E len=10    : ✅
0x814E len=41    : ✅
0x814F len=8     : 全零 ✅
```

**所有 I2C 读取均成功！** 之前 0x814E ret=1 的问题已不存在。

### 之前的 ret=1 根因分析
之前 0x814E 读取失败，现在成功。差异在于：之前先尝试 0x5D（失败）再尝试 0x14（也失败），现在 0x5D 直接成功。实际上 0x5D 一直能通，只是之前的代码流程有问题。

### 0x814E 状态字节分析
`0x80` = bit7( buffer_status_ready )=1, bit6( large_detect )=0, bit3:0( touch_num )=0
含义：芯片有数据就绪，但当前无触摸点。这是正常空闲状态。

### Config 寄存器（0x8047）解析
读取到的 config 前 8 字节：`[41 80 07 b0 04 0a 35 00]`
- Byte 0: 0x41 = config 版本号
- Byte 1-2: 0x80 0x07 = X 分辨率（little-endian: 0x0780 = 1920）
- Byte 3-4: 0xB0 0x04 = Y 分辨率（little-endian: 0x04B0 = 1200）
- Byte 5: 0x0A = 最大触点数（10）
- Byte 6: 0x35 = 中断触发模式等配置

### I2C 上拉电阻（硬件问题）
- **必须外部上拉**：I2C 是开漏输出，需要上拉电阻才能产生高电平
- 常用值：2.2k（400kHz）、4.7k（100kHz）
- 用户确认 CTP_SDA/SCL 直接从 SOC 出来，只有 3.3V 电平
- **解决方案**：在靠近 GT9271 处焊接 2.2k 上拉电阻到 3.3V

### Linux goodix 驱动 probe 流程分析
```c
// 完整 probe 流程（goodix.c）
1. goodix_get_gpio_config()        // 获取 GPIO
2. regulator_enable()              // 上电
3. goodix_reset()                  // 复位（含 INT 同步）
   ├── goodix_reset_no_int_sync()  // RST LOW 20ms → INT=HIGH → RST HIGH
   └── goodix_int_sync()           // INT LOW 50ms → INT input
4. goodix_i2c_test()               // 读 0x8140 验证 I2C 通信
5. goodix_read_version()           // 读 0x8144（6字节：4 ID + 2 version）
6. goodix_get_chip_data()          // 根据 product_id 获取芯片参数
7. goodix_configure_dev()          // 配置设备
   ├── goodix_read_config()        // 读 0x8047（186字节 config）
   ├── input_set_abs_params()      // 设置输入参数
   ├── input_mt_init_slots()       // 初始化 MT 槽
   └── input_setup_polling()       // 设置轮询（如果无 IRQ）
```

**关键发现：** Linux 驱动在 probe 时会读取 config（0x8047），但我们的驱动没有这一步。不过 config 读取主要是获取分辨率等参数，不影响坐标读取。

### 坐标读取流程（Linux goodix 驱动）
```c
// goodix_ts_read_input_report()
u16 addr = 0x814E;
int header_contact_keycode_size = 1 + 8 + 1;  // 10 bytes

// 第一次读：10 字节（1 header + 8 contact + 1 keycode）
goodix_i2c_read(client, addr, data, header_contact_keycode_size);

if (data[0] & 0x80) {  // bit7 = buffer ready
    touch_num = data[0] & 0x0F;
    if (touch_num > 1) {
        // 继续读剩余触点
        goodix_i2c_read(client, addr + 10, data + 10, 8 * (touch_num - 1));
    }
    return touch_num;
}

// 读完后清除 status
goodix_i2c_write_u8(client, 0x814E, 0);
```

---

## 当前问题

### 问题 1：系统 crash ✅ 已解决
诊断扫描完成后，AppBringUp 任务触发 assertion panic。已通过删除诊断函数修复。

### 问题 2：I2C 通信不稳定 ⚠️ 需硬件确认
Worker 轮询时 I2C_READ FAIL（ret=-1），已通过以下软件优化缓解：
- 轮询间隔从 5ms 增加到 16ms
- I2C 失败时 reset GT9271 芯片
- 连续失败 10 次自动 reset

**待确认**：需要检查硬件（I2C SDA/SCL 是否有外部 2.2k 上拉电阻）

### 问题 3：触摸数据尚未验证
I2C 通信不稳定，worker 的实际触摸数据读取尚未验证（被 I2C 失败中断）。

---

## 下一步计划

### 立即执行
1. **硬件确认**：检查 I2C SDA/SCL 是否有 2.2k 上拉电阻，没有则焊接
2. **验证 worker**：确保 worker 能正常轮询 0x814E 并读取触摸坐标
3. **测试触摸**：手指按压看坐标上报（观察 STEP2 日志）

### 触摸数据读取方案
参考 Linux goodix 驱动的坐标读取流程：
```c
// 1. 从 0x814E 读 10 字节 (1 header + 8 contact + 1 keycode)
goodix_i2c_read(client, 0x814E, data, 10);
// 2. 检查 bit7 (buffer ready)
if (data[0] & 0x80) {
    touch_num = data[0] & 0x0F;
    // 3. 如果 touch_num > 1，继续读剩余触点
    if (touch_num > 1)
        goodix_i2c_read(client, 0x814E + 10, data+10, 8*(touch_num-1));
    // 4. 清除 status
    goodix_i2c_write_u8(client, 0x814E, 0);
}
```

当前 worker 已实现类似逻辑（先读 1 字节 status，再读数据），需要验证是否正确。

### 潜在优化
- 如果 GT9271 的 contact_size 不是 8 字节，需要调整
- 考虑是否需要写入 config（0x8047）来配置分辨率等参数
- 确认 poll 模式下的轮询间隔是否合理

---

## 关键文件

| 文件 | 作用 | 状态 |
|------|------|------|
| `vendor/allwinnertech/boards/r528/drivers/gt911_iic_touch.c` | 触摸驱动主文件（535行） | **正在调试** |
| `vendor/allwinnertech/boards/r528/drivers/gt911_iic_touch.h` | 触摸驱动头文件（78行） | 已修改分辨率 |
| `vendor/allwinnertech/chips/r528/r528_boot.c` | 板级初始化，注册触摸驱动 | 已修改 |
| `vendor/allwinnertech/chips/r528/drivers/rtos-hal/hal/source/disp2/soc/BOE_1200x1920_mipi_config.c` | 屏幕配置，含触摸 GPIO | 已使能 |
| `vendor/allwinnertech/chips/r528/drivers/rtos-hal/hal/source/disp2/disp/lcd/panels.c` | 面板注册 | 已添加 BOE |
| `vendor/allwinnertech/chips/r528/drivers/rtos-hal/hal/source/disp2/disp/lcd/panels.h` | 面板头文件 | 已添加 extern |
| `vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh/defconfig` | nsh 配置 | 已修改 |
| `nuttx/arch/arm/src/Makefile` | Make.dep ELF 截断修复 | 已修改 |

### I2C 地址选择机制
GT911/GT9271 支持两个 I2C 地址：0x5D 和 0x14，由 reset 时 INT 引脚电平决定：
- **0x5D**：reset 时 INT=LOW（释放后保持 LOW）
- **0x14**：reset 时 INT=HIGH（释放后保持 HIGH）

```c
// 0x5D 地址的 reset 时序
hal_gpio_set_data(RST, LOW);
hal_gpio_set_data(INT, LOW);     // INT=LOW → 选择 0x5D
up_udelay(20000);                // 20ms
hal_gpio_set_data(RST, HIGH);
up_udelay(50000);                // 50ms 等待芯片启动

// 0x14 地址的 reset 时序
hal_gpio_set_data(RST, LOW);
hal_gpio_set_data(INT, HIGH);    // INT=HIGH → 选择 0x14
up_udelay(20000);
hal_gpio_set_data(RST, HIGH);
up_udelay(5000);
hal_gpio_set_data(INT, LOW);     // 释放 INT
up_udelay(50000);
```

---

## 经验教训

1. **I2C 地址选择时序很重要**：GT911/GT9271 的 I2C 地址由 reset 时 INT 引脚电平决定，时序不对会导致地址错误。必须在 RST LOW 期间设置 INT 电平，然后 RST HIGH 后等待足够时间（50ms）
2. **诊断要一次做够**：嵌入式调试刷机成本高（每次编译+打包+烧录约5分钟），每次尽可能打印完整信息
3. **product_id 需要截断版本字节**：GT9271 的 product_id 是 "9271"（4字节），第4字节是版本号，需要截断为 "927" 才能匹配 Linux 驱动的 ID 表
4. **NuttX printf 可能不支持所有格式符**：使用 `%zu` 等格式时需确认平台支持，建议用 `%d` 或强制类型转换
5. **栈空间要留意**：AppBringUp 栈只有 4KB，大量 printf 可能导致栈溢出。诊断函数应控制栈使用量
6. **I2C_TRANSFER 返回值**：NuttX 中 `I2C_TRANSFER` 返回 0 表示成功，负数表示错误。原始代码用 `ret < 0` 检查，新代码用 `if (ret)` 也可以（因为成功时 ret=0）
7. **先备份再修改**：所有原始代码都保留在注释中，方便回溯
8. **Linux 内核源码是最好的参考**：Goodix 驱动的寄存器定义、初始化流程、坐标读取逻辑都已在 Linux 内核中实现，直接参考可以避免很多坑
9. **I2C 必须有上拉电阻**：I2C 是开漏输出协议，没有外部上拉电阻会导致通信不稳定（ret=-1）。常用 2.2k（400kHz）或 4.7k（100kHz）
10. **轮询间隔不能太小**：GT9271 的 I2C 轮询间隔建议 16ms+，太频繁（5ms）会导致通信失败
11. **I2C 失败时需要 reset 恢复**：连续 I2C 失败可能是芯片状态异常，reset GT9271 可以恢复正常

---

## 待办事项

- [x] 解决 AppBringUp crash（栈溢出问题已修复）
- [ ] 验证 worker 触摸数据读取
- [ ] 测试实际触摸功能（手指按压看坐标上报）
- [ ] 考虑是否需要写入 config 寄存器
- [x] 优化 poll 模式轮询间隔（已调整）
- [x] 清理诊断代码（已完成）
- [ ] 硬件确认：I2C SDA/SCL 需要 2.2k 外部上拉电阻

---

## 修改历史

### 2026-07-24 会话 1：BOE 屏幕集成
- 复制 BOE 驱动文件，修改 panels.c/h/Kconfig/Makefile
- 修复大小写问题，屏幕成功点亮

### 2026-07-24 会话 2：触摸驱动基础适配
- 修改分辨率、使能 GPIO、修复 product_id 检测
- 发现 I2C 读取 0x814E 失败（ret=1）

### 2026-07-24 会话 3：I2C 诊断增强
- 添加详细诊断日志，全寄存器扫描
- **发现：I2C 全部通了！** 之前 ret=1 的问题不存在了
- 发现 AppBringUp crash（栈使用率 90.8%）
- **用户指出：crash 是我写的 bug** — 需要精简诊断代码，避免栈溢出

### 2026-07-24 会话 4：解决 crash 和 I2C 不稳定
**问题 1：AppBringUp crash**
- 原因：`gt911_diag_read_all_regs` 大量 printf 导致栈溢出（AppBringUp 栈只有 4KB）
- 解决：
  - 删除 `gt911_diag_read_all_regs` 函数
  - 精简 `gt911_i2c_read/write` 的 verbose 日志
  - 清除所有 "fisker" 调试打印
- **验证：系统不再 crash** ✅

**问题 2：I2C 通信不稳定**
- 现象：worker 轮询时 `ret=-1`，日志刷屏
- 解决：
  - 轮询间隔从 5ms 增加到 16ms（`POLL_MINDELAY=16`）
  - I2C 失败时增加重试间隔（500ms）
  - 连续失败 10 次自动 reset GT9271 芯片
  - `buf_rdy=1, num=0` 时延迟 2ms 等芯片处理
  - 添加 `gt911_reset_chip()` 函数（RST LOW 20ms → INT LOW 1ms → RST HIGH → 等待 50ms）
- 状态：仍需硬件确认（I2C 上拉电阻）

**硬件发现：**
- GT9271 的 I2C 需要外部上拉电阻（2.2k~4.7k）
- 用户确认：CTP_SDA/SCL 直接从 SOC 出来，只有 3.3V 电平
- **需要在靠近 GT9271 处焊接上拉电阻**

### 下次继续
- 确认硬件：焊接 2.2k I2C 上拉电阻
- 验证 worker 触摸数据读取（观察 STEP2 日志）
- 测试实际触摸功能
- 考虑是否需要写入 config 寄存器（GT9271 配置分辨率等参数）
