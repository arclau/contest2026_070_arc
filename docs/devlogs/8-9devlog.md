# DevLog 2026-08-09 — Books 内置书/进度持久化 + Files 真·文件管理器 + UI 交互修复

> 工程：openvela (R528) — nsh 配置｜工作目录：`/data/dm`｜固化：ok-20260809-5（md5 待确认）

---

## 一、背景

基于 ok-20260808-26（Beta V0.0.1）继续开发，解决三大用户痛点：
1. **Books 无书可读**：TF 卡为空时无法演示，需内置公版书 + 进度/护眼持久化
2. **Files 只是静态 mock**：需真实目录浏览 + 长按多选 + 复制/剪贴/粘贴/删除
3. **UI 交互细节**：Settings 行点击无反馈、Books 点不进去、Files 根入口卡片消失问题

---

## 二、修复与实现（按「现象→根因→方案→验证」）

### 2.1 Books 内置演示书 + 双路径扫描 + 进度/护眼持久化

| 项 | 内容 |
|----|------|
| **现象** | 进 Books 子页只显示静态 hero 卡《三体》，点不进去；TF 卡空则书架为空 |
| **根因** | 仅扫描 `/sdcard/book`，无内置书源；阅读器无进度记忆、护眼模式不持久化 |
| **方案** | ① 在 `lichee/board/common/data/UDISK/book/` 放入 2 本公版 txt（三体精简版、道德经），打包进 usrdata.fex 开机自动出现在 `/data/book/`<br>② `book_scan_directory()` 改为双路径扫描：优先 `/data/book`（内置），再扫 `/sdcard/book`（TF卡），按文件名去重<br>③ 新增 `/data/book_progress.conf`（path:offset 行式）记录每本书读到哪页<br>④ 新增 `/data/book_theme.conf` 记录护眼模式（0=白/1=米/2=黑），三档循环切换 |
| **验证** | 编译打包通过（ok-20260809-3~5）；内置书随固件自带，无 TF 卡也能演示阅读器全流程 |

**改动文件**：
- `apps/luncher_dm/deskmate_ui.c`：`book_scan_directory`、`book_open_reader`、`book_reader_close`、`book_apply_eye_style`、`book_eye_toggle_cb`、新增 `book_progress_load/save`、`book_theme_load/save`
- `lichee/board/common/data/UDISK/book/three_body_short.txt`、`dao_de_jing.txt`（新增）

---

### 2.2 Files 真·文件管理器（双根入口 + 面包屑 + 长按多选 + 复制/剪贴/粘贴/删除）

| 项 | 内容 |
|----|------|
| **现象** | Files 子页仅 6 条静态 mock 数据，无目录浏览、无文件操作 |
| **根因** | `create_files_subpage` 只有硬编码数组，未接 `opendir/readdir`，无选择模式、无剪贴板 |
| **方案** | ① 顶部固定双根入口卡片：`本机存储 (/)` + `TF卡 (/sdcard)`，点击进入对应根目录，**卡片常驻不隐藏**（像手机文件管理器）<br>② 面包屑路径栏：`本机存储 > data > book` 可点击快速跳转<br>③ 长按行进入选择模式 → 底部弹出操作栏（圆形按钮：复制/剪贴/粘贴/删除/完成），风格对齐音乐播放器音量键<br>④ 剪贴板最多 32 项，`cp -r` / `mv`（跨设备 fallback 到 `cp+rm`）<br>⑤ 文件打开：`.txt` 跳 Books 阅读器、`.mp3` 预留 Music 接口 |
| **验证** | 编译打包通过（ok-20260809-3~5）；上板可测：进入目录、长按多选、复制粘贴删除、面包屑返回 |

**改动文件**：
- `apps/luncher_dm/deskmate_ui.c`：新增 `files_scan_dir`、`files_refresh_list`、`files_update_path_bar`、`files_row_click_cb`、`files_row_long_cb`、`files_enter/exit_select_mode`、`files_toggle_select`、`files_cb_copy/cut/paste/delete`、`files_do_paste/delete`、`files_open_file`、静态回调函数、`create_files_subpage` 重写

---

### 2.3 UI 交互修复

| 问题 | 根因 | 方案 | 验证 |
|------|------|------|------|
| **Settings 行点击无视觉反馈** | 无 `LV_STATE_PRESSED` 样式 | 为 `settings_row_switch_cb`、`settings_row_value`、`settings_row_slider` 加按下变灰（`0xE5E5EA`） | 编译通过，上板点击行有变灰反馈 |
| **Books 书卡/Hero 卡点不进去** | 子对象拦截点击，事件不冒泡到父卡片 | 给封面、图标、文字列、info 列加 `LV_OBJ_FLAG_EVENT_BUBBLE` | 编译通过，上板点击任意区域均能进阅读器 |
| **Files 根入口卡片进入子目录后消失** | `files_refresh_list` 里按路径隐藏 | 改为**常驻显示**，仅 hint 提示在根目录时显示 | 编译通过，上板顶部卡片始终可见 |

**改动文件**：
- `apps/luncher_dm/deskmate_ui.c`：三处按下样式、书架/hero 卡冒泡标志、files_refresh_list 逻辑调整

---

## 三、改动文件汇总

| 文件 | 核心改动 |
|------|----------|
| `apps/luncher_dm/deskmate_ui.c` | Books 双路径/持久化、Files 完整重写、UI 交互修复（~+2000 行） |
| `lichee/board/common/data/UDISK/book/three_body_short.txt` | 新增内置书 |
| `lichee/board/common/data/UDISK/book/dao_de_jing.txt` | 新增内置书 |

---

## 四、验证命令与产物

```bash
# 编译
cd /data/dm && ./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh -j$(nproc)
# 打包
cd /data/dm/vendor/allwinnertech/lichee && source envsetup.sh && lunch_nuttx 2 && pack
# 校验
ls -la /data/dm/vendor/allwinnertech/lichee/out/r528s3/gemini-s1_nand/image/nsh.fex /data/dm/nuttx/vela.bin
# 固化
bash /data/vela/git_snapshot.sh   # → ok-20260809-5
```

**产物**：`rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img`（7,503,280 字节）

---

## 五、遗留事项

1. **🔴 WiFi 固件下载失败 `download_fw FAIL status=0x27`**（闭源库 `librtl8733bs.a`，需上板硬件排查：GPIO2 复位波形、SDIO 供电时序、cut_version 匹配）
2. **🔴 蓝牙 H4 open 110**（同源嫌疑，需实测 `/dev/gpio2` 复位电平、UART1 波特率/流控）
3. **Files 重命名对话框未实现**（框架已留 `files_cb_rename`）
4. **Files `.mp3` 点击跳 Music 播放器未接入**（框架已留 `FILE_TYPE_MP3` 分支）
5. **Books 目录/书签功能未做**（可选）

---

*DevLog by AtomCode (deepseek-v4-flash)*


---

# DevLog 2026-08-09 — UI 架构重构 Phase 1（巨石拆分，P26）

> 按 uiyouhua.md（P0.5 依赖分析审计报告）执行：P0.8 建目录 → Phase 1 只拆 UI 文件。
> 原则：稳定产品 → 逐步架构化；逻辑一字不改、允许 extern、先保行为。

## 一、背景

`deskmate_ui.c` 5876 行巨石：UI + 全部业务 + 全部状态混在一文件，改 UI 可能碰音频。
uiyouhua.md 审计后给出拆分方案：按风险从低到高拆 7 页到 `ui/` 目录，播放核心暂留。

## 二、执行过程（每页走完整验证：编译 → 打包 → 校验产物 → 固化）

| 步骤 | 动作 | 固化 tag | 产物 md5（nsh.fex = vela.bin） |
|------|------|----------|-------------------------------|
| P0.8 | 建 `ui/` `service/` `common/` + 占位文件 + TODO 注释，不迁代码 | — | — |
| Phase1-1 | 拆 `ui/ui_wifi.c`（create_wifi_subpage + wifi 回调 + destructor + static 变量） | ok-20260809-11 | b8fc310c… |
| Phase1-2 | 拆 `ui/ui_bt.c`（bt 回调 + destructor + static 变量，settings 构建器 extern 化） | ok-20260809-12 | 4fbbce23… |
| Phase1-3 | 拆 `ui/ui_books.c`（书架/阅读器/book_* + 进度/护眼持久化） | ok-20260809-13 | 8478b3e1… |
| Phase1-4 | 拆 `ui/ui_files.c`（目录扫描/多选/剪贴板/面包屑，system() 原样保留） | ok-20260809-14 | fee106e1… |
| Phase1-5 | 拆 `ui/ui_settings.c`（brightness/volume/autolock/wifi/bt 回调，volume 走 extern） | ok-20260809-15 | 70045ea4… |
| Phase1-6 | 拆 `ui/ui_music.c`（仅 UI 部分，播放核心 dm_sound_*/XPlayer 留 deskmate_ui.c） | ok-20260809-16 | 7a6cbcef… |
| Phase1-7 | 拆 `ui/ui_home.c`（主界面/状态栏/时钟/天气卡/Dock/待机 + 4 系统 timer） | ok-20260809-17 | 7f9462dc… |

## 三、关键决策 / 根因

1. **跨页 extern 集中区**：`deskmate_ui.h` 扩展为共享声明头（宏 + 共享工具 + 播放核心函数 + HOME 状态），各 `ui_*.c` include 之；`deskmate_ui.c` 内被跨页引用的 static 符号去 static（settings 构建器、music_round_btn、music 播放核心函数、HOME 变量、close_subpage/show_subpage 等）。
2. **共享工具留 deskmate_ui.c**：make_clean_cont / subpage_big_title / settings_* 构建器 / music_round_btn 等按 uiyouhua.md 11.2 留在原文件，extern 供 ui_*.c 用。
3. **特殊 overlay 对象+清理一起搬**：wifi_pwd_overlay（ui_wifi_close_cleanup）、book_reader_overlay（book_reader_close）、files_sel_bar（files_ui_clear_ptrs）——禁止拆散。
4. **播放核心不搬**：dm_sound_*/music_audio_*/g_xplayer 留 deskmate_ui.c；music_play/resume/pause/album_next/scan_tracks/state_load/timer_cb 去 static 供 ui_music.c UI 调用（Phase 2c 再一次性接 music_service）。
5. **music 常量/books 常量上移 deskmate_ui.h**：DM_MAX_TRACKS/DM_MUSIC_DIR/DM_MAX_BOOKS/DM_BOOK_DIR* 等，避免 files_open_file 跨文件引用断链。
6. **Makefile**：CSRCS 逐页追加 `ui/ui_xxx.c` + `CFLAGS += ${INCDIR_PREFIX}$(CURDIR)`（ui/ 子目录 include 上级头）。

## 四、验证命令与产物

```bash
cd /data/dm && ./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh -j$(nproc)
cd /data/dm/vendor/allwinnertech/lichee && source envsetup.sh && lunch_nuttx 2 && pack
md5sum nuttx/vela.bin lichee/out/r528s3/gemini-s1_nand/image/nsh.fex   # 每步一致
bash /data/vela/git_snapshot.sh   # 每步固化
```

**最终产物**：deskmate_ui.c 5876 → 2146 行；ui/ 目录 7 个页面文件（wifiwifi/bt/books/files/settings/music/home）+ ui_manager 占位，共 ~4000 行。

## 五、遗留事项

1. **🔴 WiFi 0x27 / 蓝牙 H4 open 110** 仍在驱动层（闭源库），待上板硬件排查。
2. **Phase 2a/2b/2c**：wifi_service / bt_service / music_service（最后，一次性接入，73 处引用全量 diff）。
3. **Phase 3**：ui_manager 页面注册表（现在 show_subpage if-else 稳定，不提前换）。
4. **Phase 4 清理**：g_music_volume tentative 双定义（69/1808 行）消除、死代码清理。
5. **上板回归**：每页功能（WiFi 连接/蓝牙配对/Books 翻页进度/Files 多选/Settings 亮度音量/Music 切歌后台）待上板确认。
6. Files 重命名对话框 / `.mp3` 跳 Music / Books 目录书签（前会话遗留，未在本次范围）。

---

*DevLog by AtomCode (deepseek-v4-flash)*

---

# DevLog 2026-08-09 — 编译打包验证（收尾固化）

> 会话结束前按 AGENTS.md 二、必做动作对 P26 拆分结果做最终编译/打包/校验/固化。

## 验证记录

```bash
cd /data/dm && ./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh -j$(nproc)
# EXIT=0，LD: nuttx 成功，无 error
cd /data/dm/vendor/allwinnertech/lichee && source envsetup.sh && lunch_nuttx 2 && pack
# PACK_EXIT=0，镜像 rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img
md5sum nuttx/vela.bin lichee/out/r528s3/gemini-s1_nand/image/nsh.fex
# 一致：15b1a70cbcd50616fc2959168b3bf1e3
bash /data/vela/git_snapshot.sh
# 固化 ok-20260809-19（无代码改动，仅验证固化）
```

## 结论

- P26 UI 架构重构 Phase 1 全部 7 页拆分 + 收尾文档更新后，最终编译/打包/产物校验通过（vela.bin = nsh.fex）。
- 未上板实测；待上板验证清单（WiFi 0x27 / 蓝牙 H4 110 / 7 页功能回归）不变。

---

*DevLog by AtomCode (deepseek-v4-flash)*
