# DevLog 2026-08-09 — 子 APP UI 审核整改（Files/Books/Games）+ 布局统一

> 工程：openvela (R528) — nsh 配置｜工作目录：`/data/dm`｜固化：ok-20260809-21~25

---

## 一、背景

P26 UI 架构重构 Phase 1（巨石拆分）后，各子 APP 仅做了功能验证，**UI 未对照设计规范审核**。
本次会话按 deskmate-ui skill（1920×1200 浅色 Liquid Glass 规范）逐页审核 Files / Books / Games，
修复功能 bug、交互缺陷、布局重叠，并统一卡片风格（白玻璃 + 白边 + 阴影 + 层间距）。

用户上板反馈：Files 功能全生效（删除确认/toast/长按多选/面包屑/新建去重），但 **UI 元素重叠、
卡片贴边、操作栏出边界**；Books 封面丑、书架卡片太大；Games 卡片太大。

---

## 二、Files 子 APP 审核整改（固化 ok-20260809-21）

### 2.1 P0 功能 bug（skill 审核发现）

| # | 现象 | 根因 | 方案 |
|---|------|------|------|
| P0-1 | 面包屑中间层级点击无效 | `lv_event_get_user_data(e)` 返回注册回调时的 user_data（NULL），路径存在对象 user_data 里 | 改用 `lv_obj_get_user_data(lv_event_get_target(e))` |
| P0-2 | 长按多选「进即退」 | LVGL 释放时**无条件**发 `LV_EVENT_CLICKED`（lv_indev.c L1046-1048 不检查 long_pr_sent）→ 刚选中的项被 toggle 掉 → 退出选择模式 | `files_long_guard` 标志：长按置 1，click_cb 吞掉一次，PRESS_LOST 清标志 |
| P0-3 | 删除无确认直接 `rm -rf` | `files_cb_delete` 无二次确认 | 玻璃卡确认弹窗（取消/删除），确认后执行 + toast「已删除 N 项」 |
| P0-4 | 面包屑 double free 隐患 | 点击回调 free 后未置 NULL，`files_update_breadcrumb` 回收循环二次 free | `free` 后 `lv_obj_set_user_data(btn, NULL)` |

### 2.2 P1 交互/UX 缺陷

| # | 问题 | 方案 |
|---|------|------|
| P1-5 | 复制/剪切/粘贴无反馈 | 三处加 toast（已复制/已剪切/已粘贴 N 项） |
| P1-6 | 新建文件夹重名静默失败 | 固定名 NewFolder，同名自动加序号 NewFolder1/2/… + toast |
| P1-7 | MP3/图片点击无动作（死区） | MP3/WAV 追加 `dm_tracks` 并 `music_play()` 播放；IMG/其他 toast「暂不支持」 |
| P1-8 | 三重返回导航冗余 | 去掉 app_bar 自建返回键，保留框架返回（退出）+ 上级键 |
| P1-9 | 粘贴同名直接覆盖 | 自动改名「原名 (n).ext」去重，toast 提示改名数量 |

**改动文件**：`apps/luncher_dm/ui/ui_files.c`（全部）
**验证**：build + pack 通过，vela.bin = nsh.fex（md5 328f4020），固化 ok-20260809-21。
编译期修 2 错：`files_row_press_lost_cb` 前向声明、`LV_OPA_95`→`LV_OPA_90`（OPA 仅 10 倍数）。

---

## 三、Books 子 APP 审核整改（固化 ok-20260809-22/23）

### 3.1 P0 显示 bug

| # | 现象 | 根因 | 方案 |
|---|------|------|------|
| P0-1 | 中文书名首字母乱码 | `char fc[2] = {s[0],'\0'}` 只取 UTF-8 首字节（中文 3 字节） | 新增 `book_first_char()` 完整取 UTF-8 首字符（2/3/4 字节识别），替换书架封面 + 横幅两处 |
| P0-2 | 暗色护眼黑档白条 | `book_apply_eye_style` 只改背景/正文，底部 bar/顶部按钮/百分比/slider 轨道仍是浅色 | 全套随档位：bar 黑档 0x2C2C2E、按钮同色、百分比/页码用正文色、slider 轨道深灰；新增 3 个 UI 指针 + close 清理 |

### 3.2 P1 交互缺陷

| # | 问题 | 方案 |
|---|------|------|
| P1-3 | 翻页后滚动位置残留（从中间看） | 保存 `book_reader_scroll`，`book_page_render` 渲染后 `lv_obj_scroll_to_y(0)` |
| P1-4 | 铁律 2 违反：5 处 set_pos 无断言 | spine/cover/back_btn/eye_btn/scroll_cont 全补 assert 边界检查 |
| P1-5 | 护眼切换无反馈 | 新增 `book_toast_show()`，toast「护眼：白/米/黑」，close 清定时器 |

### 3.3 P2 轻微问题

| # | 问题 | 方案 |
|---|------|------|
| P2-6 | 横幅按钮 LV_SYMBOL_PLAY 音乐语义 | → LV_SYMBOL_NEXT（阅读前进语义） |
| P2-7 | 空书架文案漏 /sdcard/book | 文案补全三目录 |
| P2-8 | slider 拖动频繁 fread+重渲染 | 200ms 节流 + RELEASED 松手强制渲染 |

### 3.4 ✨ 书架封面重设计（用户要求：丑爆了）

- 弃深色大色块 + 左侧书脊条（book_colors→0x1C1C1E 深渐变）
- 新：**Liquid Glass 白玻璃卡**（白底 70% + 1px 白边 opa50 + 40px 圆角 + 柔和阴影）
- 上部 62% 彩色→浅灰白(0xF2F2F7) 柔和渐变书封区 + 白色首字符；下部 38% 深色书名(COL_TEXT) + `.txt` 小字(COL_SEC) + 右下蓝进度徽章
- 继续阅读横幅 thumb 同步浅色渐变

### 3.5 书架缩列（固化 ok-20260809-23，用户要求：卡片太大）

- 4 列 396×560 → **6 列 248×350**（A4 1:1.414 比例不变），紧凑统一

**改动文件**：`apps/luncher_dm/ui/ui_books.c`
**验证**：-22 md5 df377a10；-23 md5 b7002517；均 build+pack 通过。

---

## 四、Files 布局修复（固化 ok-20260809-24，用户上板反馈）

### 现象
刷机（最新镜像）后功能全生效，但 UI：卡片互相贴边重叠（本机路径卡压目录）、
操作栏按钮出边界、toast 与操作栏重叠。

### 根因
`ui_files_create` 里 `pad_row = 0`，app_bar/快捷入口/面包屑/列表四层卡片 0 间距贴边；
选择模式操作栏悬浮遮挡列表最后几行；toast(-DM40) 与操作栏(-DM24) 底部重叠。

### 方案
| 项 | 修复 |
|----|------|
| 层间距 | parent `pad_row 0 → DM(6)`，卡片阴影自然显现 |
| 卡片区分 | 四层卡片全补 Liquid Glass 白边（1px 白 opa50），与 Settings 语言统一 |
| 操作栏遮挡 | 进入选择模式列表底部留白 DM(36)，内容可滚到操作栏上方；退出恢复 |
| toast 重叠 | 选择模式下 toast 上移 -DM(110)，平时 -DM(40) |

**改动文件**：`apps/luncher_dm/ui/ui_files.c`
**验证**：md5 76e7d04a，build+pack 通过，固化 ok-20260809-24。

---

## 五、Games 子 APP UI 修复（固化 ok-20260809-25，用户要求：卡片太大）

### 现象
Games 卡片 840×360 巨大，风格未统一（无白边、desc 与名称同字号、无按压反馈、层间距 4px）。

### 方案
| 项 | 修复 |
|----|------|
| 卡片尺寸 | 2×2 大卡 → **4 列紧凑卡（396×216）**，对齐 Books 6 列紧凑风格 |
| 层间距 | `pad_row 4 → DM(6)` |
| 字号层级 | desc `FONT_BODY → FONT_CAPTION` |
| 卡片风格 | 补 Liquid Glass 白边；`pad_all 20 → DM(4)`；布局改横向（徽章左 + 文字右，对齐 Files 行卡）；徽章 DM(56)→DM(44)，图标 FONT_ICON |
| 按压反馈 | 补按压放大 + 阴影加深（对齐书架卡） |

**改动文件**：`apps/luncher_dm/deskmate_ui.c`（create_games_subpage）
**验证**：md5 9b4d00de，build+pack 通过，固化 ok-20260809-25。

---

## 六、验证命令与产物

```bash
cd /data/dm && ./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh -j$(nproc)
cd /data/dm/vendor/allwinnertech/lichee && source envsetup.sh && lunch_nuttx 2 && pack
md5sum nuttx/vela.bin lichee/out/r528s3/gemini-s1_nand/image/nsh.fex
bash /data/vela/git_snapshot.sh
```

| tag | 内容 | md5(vela.bin=nsh.fex) |
|-----|------|----------------------|
| ok-20260809-21 | Files 功能整改（P0×4 + P1×5） | 328f4020 |
| ok-20260809-22 | Books 审核整改（P0×2 + P1×3 + P2×3 + 封面重设计） | df377a10 |
| ok-20260809-23 | Books 书架 4→6 列 | b7002517 |
| ok-20260809-24 | Files 布局修复（间距/白边/操作栏/toast） | 76e7d04a |
| ok-20260809-25 | Games UI 修复（缩列/风格统一） | 9b4d00de |

镜像：`lichee/out/r528s3/gemini-s1_nand/rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img`（最新 9b4d00de）。

### 排查备注（用户刷机无变化）
- 用户刷最新镜像后 Files 功能全生效，排除改错文件：Files 页唯一源码在 `ui/ui_files.c`
  （Phase 1 拆分后 deskmate_ui.c 仅分发调用），固化 commit 确认 +275/-24。
- 曾刷 Beta V0.0.1 归档镜像（fd057da1，8-8）则无 8-9 整改——刷机前核对镜像时间戳/md5。

---

## 七、遗留事项

- **上板回归**：Files（面包屑点击/长按多选/删除弹窗/MP3 播放/粘贴改名）、Books（封面中文显示/护眼三档配色/翻页回顶/slider 跳页）、Games（4 列观感/按压反馈）
- 书架封面暂为「彩色渐变 + 首字符」代码生成，无真实书封图片资源
- 操作栏 6 按钮（复制/剪切/粘贴/删除/新建/关闭）无「全选」入口（P2 级，用户未要求）

---

*DevLog by AtomCode (deepseek-v4-flash)*
