# 9-5 Part2 Devlog — dm_kb_* 审查修复 + UART 调试工具体验优化

> 日期：2026-09-05｜P170~P172

## P170 ✅ dm_kb_* 共享键盘审查 + WiFi 键盘迁移 + 防悬挂指针

**背景**：用户质疑 P168 键盘调用逻辑写得不好，要求审查。

**审查发现**：
1. **WiFi 两处键盘（"加入其他网络"+ 密码弹窗）完全没用 dm_kb_\***——仍用裸 `lv_keyboard_create` 手写，踩中 P164/P167 已修复的坑（进页面键盘立即弹出、点空白关不掉、字体重复代码）
2. **dm_kb_destroy 存在悬挂指针隐患**——父 overlay 先被 `lv_obj_del()` 后，`state->ta`/`state->kb` 已是悬挂指针，再调 `lv_obj_remove_event_cb_with_user_data` 会崩
3. 缺少 `dm_kb_show()` 便利函数（非阻塞，暂不加）

**修复**：
- `deskmate_ui.c` `dm_kb_destroy`：加 `lv_obj_is_valid()` 检查，overlay 已删时安全跳过
- `ui_wifi.c`：两处裸键盘 → `dm_kb_create` 一行（删12行手写代码）；所有 ok/cancel/close_cleanup 路径加 `dm_kb_destroy` 先于 `lv_obj_del`
- `dm_kb_state_t` 新增 `ta_parent_orig_y` + `ta_parent_saved`（为 P171 预留，此快照含字段声明，回调逻辑在 P171 实现）

**固化**：ok-20260905-24

## P171 ✅ 键盘弹出自动上移 textarea 父容器（防遮挡输入栏）

**问题**：UART 调试工具点击输入栏弹出键盘后，键盘（50%屏高）直接盖住输入栏，打字看不到内容。

**根因**：`dm_kb_create` 键盘固定 `LV_PCT(50)` + `LV_ALIGN_BOTTOM_MID`，输入栏 `input_bar` 也在屏幕底部 `sh - DM(58)`，二者重叠。

**修复**：`dm_kb_ta_focused_cb` 增加位移逻辑——
1. 键盘弹出时：保存 textarea 父容器（input_bar）原始 y 坐标 → `lv_obj_set_y` 移到键盘上方（`kb_top - bar_height - DM(4)`）
2. 键盘收起时（defocused/CANCEL）：恢复原始 y 坐标
3. `dm_kb_state_t` 新增 `ta_parent_orig_y` + `ta_parent_saved` 字段记录原始位置

**效果**：点输入栏 → 键盘弹出 + 输入栏自动上移到键盘正上方；点空白/取消 → 键盘收起 + 输入栏复位。此逻辑是通用的，WiFi 密码弹窗也自动受益。

**固化**：ok-20260905-25

## P172 ✅ UART 调试工具两项体验优化

### 终端窗口贴合输入栏（0间距）

**问题**：P171 终端高度减 `DM(58)` 后，终端底边与输入栏顶边之间有 `DM(8)` 间距，视觉上像"没对齐"而非刻意留白。

**修复**：终端高度 = `sh - (TOP_BAR_H + DM(44)) - DM(58)`，底边紧贴输入栏顶边，0间距。经 deskmate-ui skill 审查：UART 调试工具走独立深色终端美学，不走 Liquid Glass 主语言，`DM(8)` 不算违反但意图不明 → 用户选择贴合方案。

### 发送按钮追加 \r\n

**问题**：点"发送"按钮只发文本不带换行，对端收不到完整命令行。键盘回车和发送按钮行为不一致。

**修复**：`uart_dbg_send_ready_cb` 发送文本后追加 `dm_uart_dbg_write("\r\n", 2)`。现在"发送"= 文本+换行，"回车"工具栏按钮 = 单独换行，语义分明。

**固化**：ok-20260905-26 / ok-20260905-27
