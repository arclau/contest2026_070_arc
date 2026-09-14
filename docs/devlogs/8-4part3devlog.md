# DevLog 2026-08-04 (Part 3) — Desktop Mate UI 视觉修整 + 实时天气 + FontAwesome 图标

> 工程：openvela (R528) — nsh 配置
> 工作目录：`/data/dm`
> 工具链：SDK 自带 prebuilts/gcc/linux-x86_64/arm-none-eabi (GCC 13.4.0)
> 打包流程：`lichee` 下 `source envsetup.sh && lunch_nuttx 2 && pack`，镜像输出至 `lichee/out/r528s3/gemini-s1_nand/`
> **AI 交接：新会话请先读 `AGENTS.md`，本文件是当日归档，仅按需查阅。**

---

## 一、任务背景

Part 2 已把 Desktop Mate UI 搬进板端 `apps/luncher_dm` 并打通模拟器 web 预览免刷机。Part 3 围绕用户三轮实测反馈迭代 UI：

1. **视觉比例失调**（8 英寸 1920x1200 屏幕）：dock 图标符号小、图标下英文名小、状态栏太厚、天气/AI 卡片过大、状态栏时间温湿度小、WIFI/蓝牙/电量小
2. **天气模块丑**：要求抄 GitHub 开源天气插件布局 + 接实时天气 API（显示"每天"而非"每时刻"）
3. **dock 图标反复残缺**：放大符号被裁剪 → 自绘丑 → 最终引入 Font Awesome 图标字体

## 二、设计规范 v1：8" 1920×1200 物理基准（写入代码头部）

| 维度 | 值 |
|------|-----|
| 物理基准 | 8 英寸 16:10，PPI≈283，1mm≈11.2px，字号按物理可读性定值 |
| 字号六档 | title 110 / **symbol 140** / icon 76 / body 56 / label 66 / caption 34（板端 MiSans/freetype；模拟器 montserrat 48/48/36/24/30/14） |
| 结构规则 | 状态栏 ≤132px 单行；触摸目标 ≥110px；dock 符号 ≥ 图标 45%；卡片 ≤400px；留白边缘 ≥96px |

## 三、布局调整（对应每轮用户反馈）

| 反馈 | 修改 |
|------|------|
| dock 符号/标签小 | 新增 symbol 档(114→140)、label 档(44→66)；符号占按钮 ~90% |
| 状态栏太厚、字小 | TOP_BAR_H 115→92→132（容纳 110px 大时钟）；时间用 FONT_TITLE、温湿度/图标用 FONT_ICON 黑色 |
| 天气/AI 卡太大 | 卡片 480→312→360px；AI 环 100→64→48 |
| 天气卡平铺难看 | 横向 4 列 → 行列表 → **iOS 每日卡片**（每列独立圆角浅底卡片，圆角 28，flex_grow 等宽铺满） |
| 温度贴左上角/太小 | 温度+描述改一行居中，温度升 FONT_TITLE(110) |
| 点击动画裁剪右缘 | 放大 1.15x 会溢出裁剪 → 改**只缩不放大** 0.85x 按压 + `lv_anim_path_bounce` 回弹 + 阴影脉冲 |

## 四、实时天气 API（Open-Meteo，免费无 key）

### 新增模块 `dm_weather.c/.h`（板端 + 模拟器共用一份）
- **手写 HTTP GET**（BSD socket，5s 超时，零依赖，NuttX/POSIX 通用）
- **手写 JSON 提取**（strstr 定位字段，不依赖 cJSON）
- Zeller 公式从日期算星期几；WMO code → 文字描述映射
- **假数据兜底**：网络失败时填充上海夏日典型数据（33°C/60%/多云→雷暴→冰雹→雨），UI 永不挂 Loading
- 每 10 分钟 lv_timer 刷新；状态栏显示实时"温度 | 湿度%"

### 天气卡 UI（canvas 自绘天气图形）
| 天气 | 图形 | 颜色 |
|------|------|------|
| 晴 | 圆盘+8 射线 | 橙 |
| 多云/阴 | 三圆云朵 | 灰 |
| 雾 | 云+雾线 | 灰 |
| 雨 | 云+斜雨线 | 蓝 |
| 雷暴 | 云+闪电折线 | 黄 |
| 雪 | 云+雪点 | 浅蓝 |

需给板端开 `CONFIG_LV_CANVAS=y`（此前未启用）。

## 五、dock 图标三版演进（残缺问题根治）

| 版本 | 方案 | 结果 |
|------|------|------|
| V1 | label + LV_SYMBOL + transform_scale ×3(768) | **只剩一个角**（LVGL 9.1 label transform 绘制区域裁剪，transform_width/height 扩展无效） |
| V2 | 120×120 canvas 自绘白色图形 | 完整但**丑** |
| V3（终） | **Font Awesome 图标字体** fa-solid-900.ttf + freetype 140px 原生渲染 | 完整+美观+两端一致 |

- 板端：`/resource/fonts/fa-solid-900.ttf`（426KB，从 FortAwesome 官方仓库下载，进 romfs 打包）；`dm_font_symbol` 加载路径改为 fa-solid-900.ttf
- 模拟器：`lv_conf.defaults` 启用 `LV_USE_FREETYPE 1`（CMake 自动 `find_package(Freetype)` 链接），`deskmate_main.c` main() 里 `lv_freetype_font_create("../fa-solid-900.ttf", 140)` 加载
- dock 图标改回 label + `LV_SYMBOL_*`（FontAwesome 码点兼容）

## 六、模拟器断连修复（会话末期）

现象：web 预览连不上，链路断在 lvglsim 进程退出（websockify/x11vnc/Xvfb 均存活）。
处理：重启 lvglsim → 恢复；日志中 `lv_image_src_get_type: invalid magic 0x18` 定位为 luncher_dm 背景图 `/resource/imgs/luncher_mini_bg_new.png`（320x240 旧图 PNG 解码告警，AGENTS.md 已知遗留），与本次改动无关、不导致崩溃。

## 七、文件改动汇总

| 文件 | 改动 |
|------|------|
| `apps/luncher_dm/luncher_dm.c` | 字体六档（110/140/76/56/66/34）；symbol 档加载 fa-solid-900.ttf；`dm_symbol_scale` 全局 |
| `apps/luncher_dm/deskmate_ui.c` | 设计规范 v1 注释；六档字体宏；状态栏/卡片/dock 全部布局调整；点击动画改按压回弹；天气卡 canvas 自绘 + 每日卡片；dock 图标 label+FontAwesome |
| `apps/luncher_dm/deskmate_ui.h` | 新增 `dm_font_symbol/icon/label` extern |
| `apps/luncher_dm/dm_weather.c/.h`（新） | Open-Meteo 客户端（HTTP GET + JSON 提取 + WMO 映射 + 假数据兜底） |
| `apps/luncher_dm/Makefile` | `CSRCS` 加 `dm_weather.c` |
| `configs/nsh/defconfig` | `CONFIG_LV_CANVAS=y` |
| `lichee/board/common/data/res/fonts/fa-solid-900.ttf`（新） | Font Awesome 图标字体（进 romfs 打包） |
| `/data/lv_port_linux/src/deskmate_main.c` | freetype 加载 fa-solid-900.ttf；dm_symbol_scale=256 |
| `/data/lv_port_linux/lv_conf.defaults` | `LV_USE_FREETYPE 1` |
| `/data/lv_port_linux/CMakeLists.txt` | `add_executable` 加 `src/dm_weather.c` |
| `/data/lv_port_linux/src/dm_weather.c/.h`（新） | 板端同款副本 |

## 八、验证

```bash
# 模拟器（freetype 版）
cd /data/lv_port_linux/build && cmake .. && make -j$(nproc)   # 3355664 bytes, 0 error
# 板端
cd /data/dm && ./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh -j$(nproc)
cd lichee && source envsetup.sh && lunch_nuttx 2 && pack
# 产物一致性
ls -la lichee/out/r528s3/gemini-s1_nand/image/nsh.fex nuttx/vela.bin   # 4644284 bytes
cmp nsh.fex vela.bin && echo OK   # OK
```

模拟器 web 预览：`http://192.168.*.*:8080/vnc.html?host=192.168.*.*&port=9009&autoconnect=1&resize=scale`（强刷 Ctrl+F5 看新图标）。

## 九、遗留事项 / 下一步

1. **上板验证**：FontAwesome 图标渲染、WiFi 联网取真实天气（板端需可连外网 + DNS；断网时假数据兜底不崩溃）
2. 背景图 `/resource/imgs/luncher_mini_bg_new.png` 仍为 320x240 旧图（PNG 解码告警 `invalid magic`），建议换 1920x1200 横屏图
3. 天气城市坐标写死上海（`dm_weather.c` 的 `DM_W_LAT/LON`），后续可做设置页切换
4. AI 卡仍为占位（"Ready to help"），可换音乐/日程/健康等实用功能卡
5. 模拟器 montserrat 与板端 MiSans 观感差异仍在（布局一致、字体不同）；dock 图标已用同一 FontAwesome 字体两端一致
6. 状态栏无 WiFi/蓝牙真实状态，仍为静态图标

---

*DevLog by AtomCode (deepseek-v4-flash)*
