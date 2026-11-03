# P269 — 双按钮弹窗溢出修复（P251 根因2：dm_popup_btn 定宽）

目标：修复两颗胶囊按钮并排时超出窄玻璃卡右缘（P251 根因2）的裁切/溢出。
平台：共享 LVGL 源码；goldfish ARM64 编译与运行验收；R528 待板端复验。

## 根因

`deskmate_ui.c` 的 `dm_popup_btn` 固定 `DM(176)×DM(60)`。按钮行用 `SPACE_EVENLY`
排定宽子项，两颗合计 `2×DM(176)+pad_column DM(20)`=372 逻辑宽，超出窄卡内容宽
（如健康弹窗 `DM(300)` 卡内容仅约 `DM(244)`）→ 右缘溢出、错位。

## 修复

- 新增 `dm_popup_btn_flex(btn)`（`deskmate_ui.c/.h`）：`set_width(0)` + `flex_grow(1)`，
  让多按钮随按钮行内容宽等分；单按钮弹窗不受影响（仍走 `dm_popup_btn` 定宽设计）。
- 调用点：`ui/ui_books.c`（取消/删除）、`ui/ui_home.c`（健康提醒双子键）改用该入口；
  `ui/ui_files.c` 原名项目弹窗既有内联写法（`set_width(0)`+`flex_grow(1)`）收敛为同一入口。

## 验证结果

| 项目 | 结果与证据 |
|---|---|
| goldfish 编译 | `build.sh ...goldfish-arm64-v8a-ap --cmake -j$(nproc)`，`build completed successfully` |
| 头文件隔离 | 构建前移开 Make 生成头，构建后全部恢复 |
| goldfish 运行 | 文件页选 FSCK0000.REC → 删除，确认弹窗「取消 / 删除」并排完整显示，无溢出；见 [截图](../specs/ui/popup-two-button.png) |
| 单按钮回归 | 书籍内置书弹窗仍为单颗定宽「知道了」，未受影响 |
| R528 | **未验**：待板端编译打包与实机 |

## 代码边界

所属仓 `vendor/allwinnertech`，起点 HEAD `20b1e1dc612d2a8788871cd207820267f53324dd`。
修改 `deskmate_ui.c`、`deskmate_ui.h`、`ui/ui_books.c`、`ui/ui_home.c`、`ui/ui_files.c`；
开始即有 8 个 dirty 文件（前序会话改动）完整保留，非本轮新增。
