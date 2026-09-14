# DevLog 2026-08-05 (P4~P6) — 上板差异全面修复（字体/符号/主题）+ 状态栏 WiFi/蓝牙符号

> 工程：openvela (R528) — nsh 配置
> 工作目录：`/data/dm`
> 工具链：SDK 自带 prebuilts/gcc/linux-x86_64/arm-none-eabi (GCC 13.4.0)
> 打包流程：`lichee` 下 `source envsetup.sh && lunch_nuttx 2 && pack`，镜像输出至 `lichee/out/r528s3/gemini-s1_nand/`
> **AI 交接：新会话请先读 `AGENTS.md`（当前状态/下一步）与本文件（本次会话全过程），详细归档在 `devlog.md` 节点表。**

---

## 一、任务背景

模拟器（lvglsim + noVNC Web 预览）拉起后，用户上板（09:02 固件后）发现与模拟器存在系统性差异：

1. **字体过大**、dock 变大/显示不全/比例失调
2. **播放键等符号全部口口**（豆腐块）
3. **状态栏 WiFi/蓝牙不显示，只显示电池**

用户要求"比对双方源码"并"整个 APP 查一遍"。核心结论：**UI 源码两端完全一致（deskmate_ui.*/icons.* diff=0），所有差异都来自渲染环境（字体/主题），不是 APP 代码问题**。

## 二、会话历程（P4→P6 三段修复）

| 节点 | 问题 | 根因 | 修复 |
|------|------|------|------|
| P4 | 字体过大 / dock 失调 / 符号口口 | 字号 110/76/56/66/34 为 800x480→1920x1200 ×2.3 过度标定（dock 标签 66px 行高 92px 撑爆 DOCK_H=240）；LV_SYMBOL_* 用 MiSans freetype 渲染（无符号字形 + fallback=NULL） | 字号对齐模拟器 **48/36/24/30/14**；fallback→montserrat 内置（**后证明无效，见 P6**） |
| P5 | 暗色观感差异 | 板端 `CONFIG_LV_THEME_DEFAULT_DARK=y`（暗色主题）vs 模拟器浅色，影响 switch/slider knob/滚动条/屏幕背景 | defconfig 删除该配置 |
| P6 | **WiFi/蓝牙不显示（只显示电池）** | **LVGL 官方 montserrat 内置字体符号字形 44/62 残缺**（见下） | fallback 链改为 **MiSans → fa-solid → fa-brands**，新增 fa-brands-400.ttf 资源 |

## 三、P6 详解：状态栏 WiFi/蓝牙符号根因（本次重点）

### 现象
状态栏 wifi（COL_TEXT 深色）、蓝牙（COL_BLUE 蓝）**空白**，电池（COL_GREEN）"显示"。**模拟器与上板一致**（模拟器截图像素分析：蓝牙蓝=0、wifi 深色=0、电池绿 727px）。

### 排查过程
1. 两端源码 diff = 0（排除 APP 代码差异）
2. res.fex 内字体文件齐全（fa-solid 6.7.2、MiSans 均在）
3. LV_SYMBOL_WIFI/BLUETOOTH 宏 UTF-8 编码正确（EF 87 AB → U+F1EB 等）
4. MiSans-Normal.ttf **不含任何 LV_SYMBOL 码点**（fontTools cmap 验证）
5. montserrat_36 **cmap 含全部符号码点**（unicode_list 62 个偏移）——但！
6. **关键发现**：montserrat_36 的 `glyph_bitmap` 数组仅 **29193B**，而 WIFI/BT/BATTERY 等符号的 `bitmap_index`（45238/49260/46003）**远超数组边界** → **越界读**！

### 根因（100% 确认，脚本解析 glyph_dsc + unicode_list + glyph_bitmap 交叉验证）
**LVGL 官方内置 montserrat 字体的符号字形本来就不完整**：
- 62 个 LV_SYMBOL 中仅**前 18 个**（AUDIO/VIDEO/LIST/OK/CLOSE 等）有真实位图数据（bitmap_index 在数组内）
- 其余 **44 个**（PREV/PLAY/PAUSE/STOP/NEXT/EJECT/LEFT/RIGHT/WIFI/BATTERY_*/BLUETOOTH 等）bitmap_index 指向数组之外 → 渲染时读 .rodata 越界内存 → **空白（读到 0）或垃圾图形（读到非零）**
- **电池"显示"是越界读到非零内存的假象**（垃圾图形）；wifi/蓝牙读到 0 填充 → 空白
- 板端与模拟器同源（md5 验证板端 montserrat_36 == 9.6 官方备份），两端表现一致

### 为什么 P4 的 montserrat fallback 无效（重要教训）
LVGL fallback 机制：主字体 `get_glyph_dsc` 返回 found=true 就不走 fallback。montserrat 对越界符号 **cmap 有映射 → found=true → fallback 链被"截胡"**，永远走不到外部字体 → 渲染越界内存（空白/垃圾）。**所以 P4 的 montserrat fallback 从头就没生效过**（AUDIO 等 18 个有效符号碰巧正常）。

### 修复方案
fallback 链：**MiSans → fa-solid（覆盖 57/61 符号）→ fa-brands（补 BLUETOOTH U+F293，FA6 只收录于 brands 字重）→ montserrat 兜底**。

- 板端 luncher_dm.c `init_fonts()`：`DM_SYMBOL_FALLBACK(dm_font_*, sz)` 宏为每个字号创建 fa-solid/fa-brands freetype 字体并绑定链（MiSans 无符号码点 → 不截胡 → 链必然走到 fa-solid）
- 新增资源 `fa-brands-400.ttf`（FA6 6.7.2，210KB，cdnjs 下载）→ 板端 `lichee/board/common/data/res/fonts/`（pack 打进 res.fex）+ 模拟器 `/data/lv_port_linux/`
- 模拟器 deskmate_main.c：因模拟器 FONT_ICON 直接是 montserrat（截胡），改用**组合字体**（`make_sym_font`：拷贝 fa-solid 的 lv_font_t 作链头 → fa-brands → montserrat）

### 文件改动

| 文件 | 改动 |
|------|------|
| `configs/nsh/defconfig` | 启用 `CONFIG_LV_FONT_MONTSERRAT_14/24/30/36=y`；删除 `CONFIG_LV_THEME_DEFAULT_DARK=y` |
| `luncher_dm.c` | `init_fonts()`：字号 110/76/56/66/34 → 48/36/24/30/14；fallback 链 P4(montserrat) → P6(fa-solid → fa-brands → montserrat) |
| `deskmate_main.c`（模拟器） | 组合字体 `make_sym_font`（fa-solid 优先）+ 注释更新 |
| `fa-brands-400.ttf`（新增） | 板端 res 资源 + 模拟器目录（FA6 6.7.2，含 BLUETOOTH U+F293） |

### 验证

```bash
cd /data/dm && ./build.sh .../configs/nsh -j$(nproc)        # 0 error
cd /data/dm/vendor/allwinnertech/lichee && source envsetup.sh && lunch_nuttx 2 && pack
# res.fex 验证: fa-solid-900.ttf / fa-brands-400.ttf / MiSans-Normal.ttf 均 found
md5sum out/r528s3/gemini-s1_nand/image/nsh.fex nuttx/vela.bin  # 9975ca85… 一致
# 镜像: out/r528s3/gemini-s1_nand/rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img (28080128 B)
```

## 四、遗留事项 / 下一步

1. **上板验证 9975ca85（重点）**：状态栏 WiFi/蓝牙/电池三个图标都应显示（fa-solid/fa-brands 字形）；播放键/返回箭头/settings 图标等全 APP 符号不再空白
2. **模拟器 freetype 问题（新会话深挖）**：deskmate_main.c 组合字体改造后，模拟器 wifi/bt **仍空白**——疑似 openvela 9.1 模拟器 `lv_freetype_font_create` 加载失败（LV_LOG 不输出无法确认、进程 fd 无 ttf 文件）。建议排查：模拟器 freetype 依赖/`LV_FREETYPE_USE_LVGL_PORT=0` 的系统 freetype 环境、LVGL 9.1 freetype cache 配置。**模拟器当前仅符号渲染受影响，板端不受影响**
3. 蓝牙符号观感（fa-brands 品牌风格 vs fa-solid 实心）待用户确认，不满意可换 PNG 小图标
4. 延续项：背景图 1920x1200、AI 卡换功能、Text Size 演示态、sync_deskmate.sh 误报改进、G2D vs 软件渲染潜在差异

---

*DevLog by AtomCode (deepseek-v4-flash)*
