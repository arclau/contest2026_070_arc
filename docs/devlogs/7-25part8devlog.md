# MiuMiu 接力 Prompt — GT9271修复 + LVGL Demo + 音乐播放

> 本文档是给下一任 AI 助手的完整任务上下文。请先**完整阅读本文档**，然后按照"当前状态"逐步执行。不要跳过任何验证步骤。

---

## 1. 项目背景

### 硬件

- **SoC:** Allwinner R528 (sun8iw20)，双核 Cortex-A7
- **开发板:** r528s3-gemini-s1
- **屏幕:** BOE 1200x1920 MIPI DSI
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

---

## 2. 已完成的工作

### GT9271 触摸修复 ✅

**问题:** GT9271 开机后第一次 I2C 读取必然 NACK

**修复:** `gt911_worker()` 中 I2C 失败时执行 `gt911_reset_chip()` + 重试

**关键代码 (gt911_iic_touch.c):**
```c
// 在 gt911_worker() 中
ret = gt911_i2c_read(priv, GT911_REG_COOR, buf, len);
if (ret < 0) {
    printf("[GT911] I2C fail, resetting chip\n");
    gt911_reset_chip(priv);
    ret = gt911_i2c_read(priv, GT911_REG_COOR, buf, len);  // 重试
    if (ret < 0) {
        printf("[GT911] Still failing after reset\n");
        // ... 错误处理
    }
}
```

**验证结果:**
- 第一次读取: `msgs_idx=32` (SLA+NACK) ← 芯片拒绝
- 重置后重试: `msgs_idx=1` (成功) ← 芯片就绪
- 之后所有读取: 全部成功, 零失败

**硬件结论:**
- RST 上拉: 无效
- INT 上拉: 有害 (两个地址都 NACK)
- **保持原始电路, 不加任何上拉**

### LVGL 触摸 Demo ✅

**功能:**
- 开机显示菜单 (深蓝色背景)
- Touch Color Test: 点击切换颜色 + 显示坐标
- Widgets Demo: LVGL 内置控件演示
- Music Demo: LVGL 内置音乐播放器 UI

**开机自启:** `rcS.nsh` 中 `lvgldemo &`

### NXPlayer 音乐播放 ✅

**功能:**
- `nxplayer /data/laojie.mp3` — 播放文件后退出
- 原交互模式不受影响

**开机自启:** `rcS.nsh` 中 `nxplayer /data/laojie.mp3 &`

**MP3 文件:** `/data/openvela/vendor/allwinnertech/lichee/board/common/data/UDISK/laojie.mp3` → 打包进 `usrdata.fex`

---

## 3. 当前状态

### 固件已编译打包
最后一次烧录的固件已就绪:
```
vendor/allwinnertech/lichee/out/r528s3/gemini-s1_nand/rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img
```

### 开机流程
1. NuttX 启动
2. `rcS.nsh` 执行:
   - 挂载 `/resource` 和 `/data`
   - 启动 `adbd`, `kvdbd`, `bluetoothd`
   - 显示小米 logo (`showlogo`)
   - 启动 `lvgldemo &` (显示菜单)
   - 启动 `nxplayer /data/laojie.mp3 &` (播放音乐)

### 验证清单
- [ ] 开机显示菜单 (Touch Color Test / Widgets / Music)
- [ ] Touch Color Test: 点击切换颜色
- [ ] Touch Color Test: 显示触摸坐标
- [ ] 开机自动播放 laojie.mp3
- [ ] Widgets Demo 正常运行
- [ ] Music Demo 正常运行

---

## 4. 已知问题

### GT9271 首次 NACK
GT9271 在开机后第一次 I2C 通信时必然 NACK。已通过 `fail → 重置 → 重试` 机制修复。根因是芯片内部行为, 不影响使用。

### NXPlayer 交互模式
NXPlayer 是交互式 shell。当前修改支持 `nxplayer <file>` 播放后退出。如需更复杂的播放控制 (暂停/下一首/音量), 需要进一步开发。

### LVGL Music Demo 无音频
LVGL Music Demo 是纯 UI 视觉演示, 不对接任何音频驱动。频谱动画是伪造的。要真正播放音频需自己对接 NuttX audio 驱动。

---

## 5. 关键文件路径

| 文件 | 说明 |
|------|------|
| `boards/r528/drivers/gt911_iic_touch.c` | GT9271 触摸驱动 (修复在这里) |
| `chips/r528/drivers/rtos-hal/hal/source/twi/hal_twi.c` | TWI HAL |
| `chips/r528/drv/twi/drv_twi.c` | TWI lower-half |
| `apps/examples/lvgldemo/lvgldemo.c` | LVGL Demo |
| `apps/system/nxplayer/nxplayer_main.c` | NXPlayer |
| `boards/r528/r528s3-gemini-s1/src/etc/init.d/rcS.nsh` | 启动脚本 |
| `board/common/data/UDISK/laojie.mp3` | MP3 音乐文件 |

---

## 6. 如果需要修改

### 修改触摸驱动
```bash
vi boards/r528/drivers/gt911_iic_touch.c
# 修改 gt911_worker() 中的修复逻辑
```

### 修改 LVGL Demo
```bash
vi apps/examples/lvgldemo/lvgldemo.c
# 修改菜单、颜色、按钮等
```

### 修改 NXPlayer
```bash
vi apps/system/nxplayer/nxplayer_main.c
# 修改命令行参数支持或播放逻辑
```

### 修改启动脚本
```bash
vi boards/r528/r528s3-gemini-s1/src/etc/init.d/rcS.nsh
# 修改自启程序
```

### 添加新的 MP3 文件
```bash
cp your_song.mp3 board/common/data/UDISK/
# 重新 pack 即可
```

### 重新编译打包
```bash
cd vendor/allwinnertech/lichee
source tools/scripts/envsetup.sh
lunch_nuttx 2
m nsh c    # clean build
m nsh      # build
pack       # pack image
```

---

## 7. 文档索引

| 文档 | 说明 |
|------|------|
| `miumiu.md` | 烧录进度总览 |
| `miumiu-thinking.md` | 调试思考方式和方法论 |
| `miumiu-construction.md` | 施工逻辑和每次烧录详情 |
| `miumiu-devlog.md` | 施工日志和发现 |
| `miumiu-handoff.md` | 本文档 (接力 Prompt) |
