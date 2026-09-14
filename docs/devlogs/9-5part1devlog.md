# 9-5 Part1 Devlog — UART 调试工具键盘统一 + 共享 dm_kb_* 重构

> 日期：2026-09-05｜P164~P169

## P164 ✅ UART 调试工具键盘弹出修复

**问题**：点击「输入文本发送到 UART1...」输入框，LVGL 键盘不弹出。
**根因分析**：
- WiFi 键盘（能弹）：`lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0)`，无 FLOATING
- UART 键盘（不弹）：`lv_obj_set_pos` + `LV_OBJ_FLAG_FLOATING`，FLOATING 打断 LVGL 内部键盘自动显隐路径
**修复**：去掉 FLOATING + set_pos，改用 `lv_obj_align(LV_ALIGN_BOTTOM_MID)` 对齐 WiFi 模式。
**固化**：ok-20260905-21

## P165 ✅ UART 调试工具功能增强（P0~P2）

| 改动 | 说明 |
|------|------|
| 暂停/继续按钮 | 工具栏新增 `LV_SYMBOL_PAUSE/PLAY`，timer 头部 `if (paused) return` |
| 去掉 4×256B 读取限制 | `while ((n = read(...)) > 0)` 全排空，防高波特率丢包 |
| HEX 地址列修正 | 文件级 `uart_dbg_hex_addr` 累加，跨 feed 连续 |
| 统计接口 + 丢包计数 | `dm_uart_dbg_get_stats()` 暴露 dropped_bytes |
| 工具栏 RX/DROP 显示 | 实时显示后被用户要求去掉 |
**固化**：ok-20260905-18

## P166 ✅ LVGL 键盘字体统一

- WiFi + UART 键盘统一 Montserrat 30px（可用字号 12/14/16/24/30/36/48，30 是 24 上一档）
- 修复用户反馈"WiFi 键盘字超小"
**固化**：ok-20260905-19

## P167 ⏳ 键盘进入页面自动弹出 bug

**问题**：P164 修复后键盘默认可见，进入 UART 调试工具页面就弹出，点空白处关不掉。
**根因**：`lv_keyboard_set_textarea` 一调用就绑定，键盘默认可见。
**修复**：
1. 键盘创建加 `LV_OBJ_FLAG_HIDDEN`（初始隐藏）
2. textarea 加 `FOCUSED` → 显示键盘
3. textarea 加 `DEFOCUSED` → 检查新焦点不是键盘/textarea 后隐藏
4. 键盘加 `READY`（回车发送）/ `CANCEL`（隐藏）事件
**固化**：ok-20260905-22

## P168 ✅ 共享键盘工具 dm_kb_* 重构

**动机**：用户指出键盘到处都要用（文件浏览器重命名等），调用逻辑不科学。
**设计**：
- `dm_kb_state_t` 结构体：kb + ta + on_ready 回调 + user_data
- `dm_kb_create(parent, ta, on_ready, ud)`：创建+绑定+自动显隐
- `dm_kb_hide(state)`：手动隐藏
- `dm_kb_destroy(state)`：解绑+释放
**实现**：
- 定义在 `deskmate_ui.c`（struct 内部），声明在 `deskmate_ui.h`（opaque 指针）
- `ui_uart_dbg.c` 重构：删 3 个手动回调 + 删 `uart_dbg_kb` 变量，改用 `dm_kb_create` 一行 + `dm_kb_destroy` 一行
**注意**：`deskmate_ui.c` 不能 `#include "deskmate_ui.h"`（会导致 dm_track 等重复定义），typedef 在 .c 内部重复声明。
**固化**：ok-20260905-23

## 工具栏布局优化（P164 附带）

- port_card 文字 `/dev/uart1` → `UART1`，pad_hor DM(14)→DM(8)
- bar pad_column DM(10)→DM(6)
- tool_btn pad_hor DM(14)→DM(10)，内部 pad_column DM(8)→DM(4)
- 确保三色主题色块不被挤出屏幕右侧

## 1500000 波特率评估（结论：不可行）

R528 UART 时钟 APB1=24MHz，`dl = 24M/(1.5M×16) = 1`（分频器极限），P120 实测回环 rx=0。需改时钟树到 48/72MHz 或外挂 USB-UART 桥。
