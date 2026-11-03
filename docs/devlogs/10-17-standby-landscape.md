# P270 — 锁屏横屏重排（1440×720）

目标：锁屏（standby overlay）改为横屏布局，消除纵向溢出（健康卡排在屏外）。
平台：共享 LVGL 源码；goldfish ARM64 编译与运行验收；R528 待板端复验。

## 根因（P251 §四锁屏条目）

`ui_home.c` 的 `show_standby()` 用单列 flex（sp_top 3 : sp_mid 2 分配）纵向堆叠：
状态栏 + 双圈 `DM(160)×DM(170)×2` + AI 圈 + 提示 + 音乐行 + 健康卡。
固定内容实测≈1004px > 720px（逻辑画布 720×360），健康卡被排到屏外不显示。
另：健康卡 `DM(800)=1600`、欢迎卡 `DM(760)=1520` 均 >1440 屏宽；欢迎卡用
`lv_obj_align` 定位，但它是 flex 子项 → align 无效、被排到列首；裸像素残留。

## 修复

`show_standby()` 首次创建块改为横屏三段：

- overlay flex COLUMN = 状态栏 / 主体行(grow1) / 健康卡；`pad_row DM(6)`、`pad_bottom DM(8)`。
- 主体行 `body` flex ROW：左 `rings_row`（双圈缩到 `DM(120)×DM(130)`、arc `DM(88)`、
  间距 `DM(24)`）| 中 `mid`(grow1, COLUMN 居中：AI 圈 `DM(40)` + 提示 + 音乐标题 + 控制行) |
  右 `pet_col`(`DM(160)` 宠物舞台占位)。
- 健康卡改 `lv_pct(94)` × `DM(72)`，一行容纳 5 项（在座/喝水/起立/人体/光感）。
- 欢迎卡改 `LV_OBJ_FLAG_FLOATING` + `lv_obj_align(BOTTOM_MID, -DM(88))`，
  并在 `welcome_card_show()` 加 `lv_obj_move_foreground` 置顶。
- 裸像素包 `DM()`：健康卡 pad_all `DM(6)`/pad_top `DM(8)`/shadow `DM(6)`、
  prox 宽 `DM(130)`、lux 宽 `DM(100)`。
- 新增 `standby_pet_roam()`：宠物漫游矩形限定右栏上方
  （`x1=x2=sw-DM(90)`、`y1=y2=sh/2`），避开底部健康卡；唤醒时 `wake_from_standby`
  已回 `home_pet_roam_apply()`。

## 验证结果

| 项目 | 结果与证据 |
|---|---|
| goldfish 编译 | `build.sh ...goldfish-arm64-v8a-ap --cmake -j$(nproc)`，`build completed successfully` |
| 头文件隔离 | 构建前移开 Make 生成头，构建后全部恢复 |
| goldfish 运行 | 首页「锁屏」tile → 状态栏/主体三栏/健康卡 5 项全显示，无溢出；见 [截图](../specs/ui/standby-landscape.png) |
| 欢迎卡 | 仅「坐姿/人体传感器」事件触发，模拟器 `人体: 不可用`，**未运行验证**；代码级修正 |
| R528 | **未验**：待板端编译打包与实机触摸/G2D 观感 |

## 代码边界

所属仓 `vendor/allwinnertech`，起点 HEAD `20b1e1dc612d2a8788871cd207820267f53324dd`。
仅改 `ui/ui_home.c`（`show_standby`/`welcome_card_show`/新增 `standby_pet_roam`）；
开始即有 13 个 dirty 文件（前序会话改动）完整保留，非本轮新增。
