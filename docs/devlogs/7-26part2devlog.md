# geminifix.md — LVGL Demo 触摸点击与 180° 屏幕显示修复记录

> 本文档由 AI 助手创建于 2026-07-26，记录针对 Allwinner R528 平台上 **LVGL 触摸按钮点击无响应** 及 **屏幕 180° 颠倒显示** 的完整修复原理与相关代码修改。

---

## 1. 问题概述

在之前的调试工作中，已完成了 GT9271 芯片的 I2C 通信恢复、坐标 Mask 提取、burst 读对齐及 `npoints` 尺寸匹配，屏幕可正常上报 `(x, y)` 坐标。然而依然存在以下两个未解决问题：

1. **触摸按钮点击无响应**: 触摸屏滑动/点击时日志有坐标输出，但点击 LVGL 按钮无法触发 `LV_EVENT_CLICKED` 回调。
2. **画面颠倒 180°**: BOE 1200x1920 屏幕 UI 显示上下颠倒。

---

## 2. 根因分析 (Root Cause Analysis)

### 2.1 触摸点击无响应根因
1. **未发送 `TOUCH_UP` 事件**:
   - 当手指从触摸屏抬起时，GT9271 芯片标志位 `status & 0x80` 为真，但 `touch_num = status & 0x0f` 变为 `0`。
   - `gt911_iic_touch.c` 原代码在 `touch_num == 0` 时直接执行了 `goto error_read;`，彻底绕过了 `gt911_touch_process_event_one()`。
   - 导致驱动无法将状态由 `TOUCH_VALID` 切换为 `TOUCH_INVALID`，也无法向 NuttX 环形缓冲区写入 `TOUCH_UP` 事件。
   - 结果：LVGL 后端 `touchscreen->last_state` 永远卡死在 `LV_INDEV_STATE_PRESSED`（按下状态）。由于 LVGL 必须接收到 `PRESSED → RELEASED` 的状态转换才会触发 `LV_EVENT_CLICKED`，因此按钮点击回调永远不被触发。

2. **抬起采样数据缺失**:
   - 即使触发 release，`gt911_touch_process_event_one()` 在 `case TOUCH_VALID → TOUCH_INVALID` 时未设置 `sample->npoints = GT911_MAX_TOUCH_POINTS(5)`，且未设置抬起时的 `(x, y)` 坐标和时间戳。
   - 导致传给 LVGL 的数据包被判为无效或释放坐标掉落至 `(0, 0)`。

3. **子 Demo 输入设备路径错误**:
   - `lvgldemo.c` 的 `menu_btn_cb()` 中，重新初始化 LVGL 时 `info.input_path` 被误写为 `/dev/input/event0`（主入口 `main()` 已修改为 `/dev/input0`），导致点击菜单切换子 Demo 后触摸设备打开失败。

### 2.2 画面颠倒 180° 根因
- BOE 1200x1920 MIPI DSI 屏幕在面板物理安装方向上颠倒了 180°。
- 通过在 LVGL 显示层设置 `LV_DISPLAY_ROTATION_180`，LVGL 会在渲染时将图形旋转 180°，同时 LVGL 的 input 驱动会自动对触摸坐标实施同步镜像变换（`x' = width - 1 - x`, `y' = height - 1 - y`），使得画面与触摸点 100% 精准对齐。

---

## 3. 相关修改代码 (Code Changes)

### 3.1 修改文件 1: `vendor/allwinnertech/boards/r528/drivers/gt911_iic_touch.c`

#### (1) 处理 `touch_num == 0` 抬起逻辑 (Line 458-466)

```c
  touch_num = status & 0x0f;
  if(touch_num == 0){
    gt911_i2c_write(priv, GT911_REG_COORD_ADDR, 0);
    up_udelay(2000);
    /* 关键修复：当手抬起 (touch_num == 0) 且此前处于按下状态时，主动触发 TOUCH_INVALID 事件以发送 TOUCH_UP */
    if (priv->last_state == TOUCH_VALID)
      {
        gt911_touch_process_event_one(TOUCH_INVALID, priv, sample);
      }
    priv->poll_interval = POLL_MINDELAY;
    goto error_read;
  }
```

#### (2) 补齐 `TOUCH_UP` 采样数据 (Line 272-282)

```c
      case TOUCH_VALID:

       /* 关键修复：补齐 npoints 确保与 LVGL 读取尺寸匹配，并保留抬起时的坐标与时间戳 */
       sample->npoints = GT911_MAX_TOUCH_POINTS;
       sample->point[0].flags = TOUCH_UP;
       sample->point[0].x = priv->last_x;
       sample->point[0].y = priv->last_y;
       sample->point[0].timestamp = touch_get_time();
       priv->last_state = TOUCH_INVALID;
       priv->last_x = 0;
       priv->last_y = 0;
       touch_event(priv->lower.priv, sample);
       break;
```

---

### 3.2 修改文件 2: `apps/examples/lvgldemo/lvgldemo.c`

#### (1) `menu_btn_cb` 设备路径修正与 180° 旋转 (Line 180-195)

```c
#ifdef CONFIG_INPUT_TOUCHSCREEN
  info.input_path = "/dev/input0";  /* 修复：将原 /dev/input/event0 修正为 /dev/input0 */
#endif
  lv_nuttx_init(&info, &result);
  if (result.disp == NULL)
    {
      return;
    }
  /* 关键修复：设置 LVGL 180 度旋转，画面与触摸同步旋转 */
  lv_display_set_rotation(result.disp, LV_DISPLAY_ROTATION_180);
```

#### (2) `main()` 180° 旋转配置 (Line 288-297)

```c
  lv_nuttx_init(&info, &result);

  if (result.disp == NULL)
    {
      printf("[LVGL] ERROR: display init failed, fb=%s\n", info.fb_path);
      return 1;
    }

  /* 关键修复：主程序启动时设置 180 度旋转 */
  lv_display_set_rotation(result.disp, LV_DISPLAY_ROTATION_180);
```

---

### 3.3 修改文件 3: `apps/graphics/lvgl/lvgl/demos/music/lv_demo_music_main.c` & `lv_demo_music.c`

为了让 LVGL Music Demo 能直接通过触摸屏播放/暂停/切换位于 `/data/laojie.mp3` 的音频文件，在 `lv_demo_music_main.c` 中对接了 NuttX `nxplayer` 音频驱动：

1. **调用 NxPlayer 接口**:
   - `nxplayer_create()` 创建播放器实例。
   - `nxplayer_setdevice(g_nxplayer, "/dev/audio/pcmC0D0p")` 指定使用 R528 ACodec 硬件音频设备。
   - `nxplayer_playfile(g_nxplayer, "/data/laojie.mp3", 0, 0)` 播放目标 MP3 文件。
   - `nxplayer_pause(g_nxplayer)` / `nxplayer_resume(g_nxplayer)` / `nxplayer_stop(g_nxplayer)` 响应 UI 触摸控件点击。

2. **歌曲列表更名**:
   - 在 `lv_demo_music.c` 中将 Track 0 标题修改为 `"LaoJie (laojie.mp3)"`，演唱者修改为 `"Li Ronghao"`。

---

## 4. 固件编译与验证 (Build & Verification)

### 编译与打包命令
```bash
cd vendor/allwinnertech/lichee
source tools/scripts/envsetup.sh
lunch_nuttx 2
m nsh c && m nsh
pack
```

### 固件输出位置
`lichee/out/r528s3/gemini-s1_nand/rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img`

### 验证标准
1. 开机后 LVGL Demo 界面显示为正向（不再上下颠倒）。
2. 点击 "Touch Color Test" / "Widgets Demo" / "Music Demo" 按钮能正常响应并跳转界面。
3. 进入 Music Demo 界面后，通过触摸点击**播放/暂停**按钮，可以控制 `/data/laojie.mp3` 从 `/dev/audio/pcmC0D0p` 输出声音，且支持播放/暂停切换。

