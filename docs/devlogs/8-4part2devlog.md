# DevLog 2026-08-04 (Part 2) — Desktop Mate UI 移植 + 模拟器 Web 预览免刷机

> 工程：openvela (R528) — nsh 配置
> 工作目录：`/data/dm`
> 工具链：SDK 自带 prebuilts/gcc/linux-x86_64/arm-none-eabi (GCC 13.4.0)
> 打包流程：`lichee` 下 `source envsetup.sh && lunch_nuttx 2 && pack`，镜像输出至 `lichee/out/r528s3/gemini-s1_nand/`
> **AI 交接：新会话请先读 `AGENTS.md`，本文件是当日归档，仅按需查阅。**

---

## 一、任务背景

用户在 `/data/lv_port_linux`（LVGL Linux PC 模拟器，自己用 AI 搭建，含 web 预览）里设计了一套 **Desktop Mate UI**（桌面/时钟/天气/AI 卡/Dock 6 应用/子页面/锁屏待机）。目标：

1. 以 `luncher_dm`（luncher 副本，`CONFIG_LUNCHER_DM_APP`）为架构壳，把 Desktop Mate UI 移植到板端；
2. 用模拟器 + web 预览免刷机迭代 UI，最终一致后再上板。

## 二、评估结论：不重写，走「UI 代码搬移 + 后端替换」

| 组成 | 性质 |
|------|------|
| `lvgl/` 子模块（9.6 master） | 不动 |
| `src/lib/` 后端（SDL/wayland/drm/fbdev） | 不动 |
| `src/main.c`（1914 行 Desktop Mate UI） | **搬移主体** |
| `src/music_player/`（lv_demo_music 拷贝） | 板端裁剪为占位页 |
| `src/screenshot.c` | 纯工具，板端不需要 |
| 用户提到的 web 预览 | 仓库内无 web 代码，实为 skill 记载的 `Xvfb+x11vnc+websockify+noVNC` 架构 |

差异面全部在"皮肤以下"：渲染后端（SDL→lv_nuttx）、分辨率（800x480→1920x1200）、字体（montserrat→MiSans）、输入（键盘→触摸）、音乐（demo→占位）、LED/传感器（并入）。

## 三、luncher_dm 新 APP 基线（Part 1 延续）

- 复制 `apps/luncher` → `apps/luncher_dm/`，注册 `CONFIG_LUNCHER_DM_APP`，PROGNAME=luncher_dm，入口 `luncher_dm_main`
- LED 模块（`lv_demo_panel_rgb_control.c`）非 static 函数全部加 `dm_` 前缀（28 个符号），避免与 luncher 并存编译时链接冲突
- 编译打包验证通过（nsh.fex == vela.bin）

## 四、Desktop Mate UI 移植到 luncher_dm

### 文件改动

| 文件 | 改动 |
|------|------|
| `apps/luncher_dm/deskmate_ui.c`（新，~1877 行） | 从 `lv_port_linux/src/main.c` 搬入全部 UI；删模拟器入口/后端依赖；音乐页改占位；布局常量 ×2.4 标定 1920x1200 |
| `apps/luncher_dm/deskmate_ui.h`（新） | 导出 `deskmate_ui_create()` + `dm_font_title/body/caption` |
| `apps/luncher_dm/luncher_dm.c` | 入口 `create_main_screen()` → `deskmate_ui_create()`；`init_fonts` 增加 MiSans 110/56/34 三档（旧 66/72/96 保留）；传感器采集/LED 适配架构保留，移除旧 UI observer 绑定 |
| `apps/luncher_dm/Makefile` | `CSRCS` 加 `deskmate_ui.c` |

### 关键决策
- **只利旧 luncher_dm 架构和逻辑，UI/元素用新的**：lv_nuttx 初始化、传感器订阅/poll、LED 适配、主循环全部保留；旧 UI（create_main_screen/灯光窗/gamble 窗）不再调用
- 音乐页占位：板端无音频链，不链接 lv_demo_music
- 字体：模拟器 montserrat 无中文，板端 MiSans freetype 110/56/34

## 五、Review + 几何修复（用户提醒盲移植有 BUG，成立）

`code_review` 工具因目录无 git HEAD 不可用 → 手动审查 + 逐尺寸几何核查，发现 3 类问题全部修复：

1. **几何溢出/重叠（最严重）**：字体放大到 110/56/34（×2.4）但卡片容器高度/图标/按钮未跟随 → 新增 `DM(x)` 宏（×2.4），替换 ~30 处硬编码尺寸（卡片高度 200/160/140/100/80/260、back_btn 44、badge 28、switch、standby ring 200、health 800x96 等）；content pad_top 72→DM(72)
2. **板端 API 不兼容**：`lv_group_get_obj_by_index` 板端 LVGL 无 → 改 `lv_group_focus_next`
3. **时区 BUG**：deskmate 用 `localtime_r`，板端无 TZ 配置会显示 UTC → 改 `gmtime`+8 手动东八区（与 luncher_dm 的 update_time_cb 一致）

审查确认无问题：29 个 `LV_SYMBOL_*` 板端全有；`lv_anim_set_user_data`/`lv_obj_set_user_data`/`lv_snprintf` 板端存在；无模拟器残留引用。

验证：`./build.sh` → `lunch_nuttx 2 && pack` → `nsh.fex` 与 `vela.bin` 字节一致（4640192 字节）。

## 六、模拟器 Web 预览免刷机

### 架构（复用用户旧 skill 记载）
```
浏览器(ws://localhost:9009) → websockify → x11vnc(:5900) → Xvfb(:99) → lvglsim
```

### 实施
| 项 | 内容 |
|----|------|
| 模拟器 UI 同源 | `/data/lv_port_linux/src/deskmate_ui.c` = 板端文件复制件；新增 `deskmate_main.c`（提供 dm_font_* montserrat 48/24/14 + main） |
| CMakeLists | `add_executable(lvglsim src/deskmate_main.c src/deskmate_ui.c)` |
| 构建 | `cd /data/lv_port_linux/build && cmake .. && make -j$(nproc)`，`bin/lvglsim` 冒烟通过（1920x1200） |
| 服务 | Xvfb :99（1920x1200x24）、x11vnc :5900（-nopw -shared -forever）、websockify 9009→5900、python http.server 8080 提供 noVNC（/data/vela/novnc-web） |
| 截图验证 | `DISPLAY=:99 import -window root` → 1920x1200 PNG 正常 |

### 访问地址
```
http://192.168.*.*:8080/vnc.html?host=192.168.*.*&port=9009&autoconnect=1&resize=scale
```

日志：`/data/vela/logs/{lvglsim,x11vnc,websockify}.log`

## 七、遗留事项 / 下一步

1. **UI 修整（下会话重点）**：打开 web 预览逐页检查卡片溢出/间距/子页面布局/锁屏，视觉问题在模拟器里改板端源文件→同步→重启 lvglsim 迭代
2. **服务开机自启**：把 Xvfb+lvglsim+x11vnc+websockify 写成 systemd 服务/启动脚本，开机自动拉起（见 AGENTS.md 规则）
3. **代码自动同步规则**：模拟器 `deskmate_ui.c` 必须以板端 `apps/luncher_dm/deskmate_ui.c` 为唯一源，改完板端后同步复制到模拟器再重启 —— 已写入 AGENTS.md 硬性规则
4. 传感器数据采集保留但未接入新 UI（后续接天气卡/状态栏）；LED/触摸测试入口未挂 Dock
5. 模拟器字体 montserrat 与板端 MiSans 观感差异（布局一致，字体不同，可接受）

---

*DevLog by AtomCode (deepseek-v4-flash)*
