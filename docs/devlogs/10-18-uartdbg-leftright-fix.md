# P275 · UART 调试工具左右分栏落地（左功能 / 右日志）

日期：2026-10-18（P275 §7 续接）。状态：**布局落地；离屏 8 项 PASS；goldfish 构建与实跑、R528 打包已补验（2026-10-19）；板测待验。**

## 决策与边界

- 用户确认「P275 范围左右分栏」：左栏功能按键，右栏 logs 显示；保留 P275 已删的注入/暂停。
- 前一次尝试（波特率移发送上方 + 返回键右放三主题色 FAB）已回退：与左右分栏约定冲突，
  且用户在模拟器未见变化（背景构建静默失败掩盖了修改）。

## 实现（ui/ui_uart_dbg.c · ui_uart_dbg_create 重写）

- 左栏 `DM(150)`=300px：端口卡 `UART1·波特·○`；2×2 操作钮 清屏/ASCII/重连/回车（ASCII 切换 HEX/ASCII 标签）；
  三主题色点 深/浅/护眼；命令框（flex-grow）+ 换行后缀下拉(CRLF/LF/CR/无) + 蓝色发送。
- 右栏 flex-grow：日志终端（主题底色）+ 状态行（RX/TX/丢弃 + 反馈右对齐）。
- 状态点：连接中红、成功绿(`0x2ECC71`)；主题点切换终端 fg/bg。
- 字形（上轮已修）：UART 键盘 items 用 `lv_font_montserrat_30` 替代 `FONT_BODY`，Enter/OK/Backspace/方向/符号正常。

## 编译修复（LVGL v9）

- 无 `lv_obj_set_flex_wrap` / `LV_FLEX_WRAP_WRAP` → 改用 `LV_FLEX_FLOW_ROW_WRAP`（ops 行 2×2 换行）。
- 删除未使用的 `uart_mode_cb`，保留切换式 `uart_mode_btn_cb`。

## 键盘遮挡修复（关键）

- `lv_keyboard_create` 默认 `lv_obj_align(BOTTOM_MID)` 浮动覆盖整页底部，会盖住左栏发送钮。
- 改为父到右栏(logs)并加 `LV_OBJ_FLAG_FLOATING`：仅覆盖右栏日志底部，左栏功能键全程可见，
  契合「左功能 / 右日志」分栏。

## 验证

- `tests/tools_offscreen` 8 项全 PASS：断开保留命令、重连、键盘布局、Enter/OK 单发、短写顺序、
  EAGAIN 重试、永久错误/超时保留命令、波特率回滚、RX 预算、HEX、超长文本、关闭重开、设置七分类。
- 测试断言由「`input.y2 < keyboard.y1`」改为非重叠 `_lv_area_is_on`，契合分栏（键盘在右、输入在左）。

## 边界

- 2026-10-19 补验：goldfish 构建通过、UART 页面 Enter 路径实跑成立；R528 build/pack/check_res 通过。
  板端 TX/RX/触摸仍**未验**；模拟器无 `/dev/uart1`，不能证明串口收发。
- 详见 P275 交接 [10-18-subapp-design-language-handoff.md](10-18-subapp-design-language-handoff.md) §5/§7。
