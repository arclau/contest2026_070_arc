# P273 — HOME UART 图标不跳转 UART 调试工具

目标：修 home 入口网格 UART 图标点击无反应、不进入 UART 调试工具子页。
平台：共享 LVGL 源码；goldfish 构建+运行验收；R528 待打包。

## 现象

点 home 左下入口网格的 UART 图标（键盘符号）毫无反应，不进调试工具；其余图标正常。

## 根因

事件绑定本身正确：create_app_dock(ui_home.c:518-560) 给每个入口 btn 设
user_data=apps[i].name 并挂 app_icon_click_cb；UART 项 name="uart_dbg"、icon=NULL
（渲染 LV_SYMBOL_KEYBOARD 绿标），绑定与其他图标完全一致。
断点在派发链 launch_app_after_fade(ui_home.c:113-126)：if/else 仅覆盖
music/calendar/books/files/ai/settings/lock，**缺 "uart_dbg" 分支**，点击匹配不到
任何 case → 无动作。show_subpage("UART") 的接收端 deskmate_ui.c:1244 早已就绪
（title "UART" → ui_uart_dbg_create），只是没人发。

## 修复

ui_home.c:124 在 settings 与 lock 之间加：
  else if (strcmp(name, "uart_dbg") == 0) show_subpage("UART");

## 验证

goldfish 重建（ui_home.c 重编，ELF 15:48:56）→ 重启模拟器 → 点 UART 磁贴(795,553)
（4×2 网格 row2 col2 中心）→ 屏帧整体变化（diff bbox 全帧 vs home）→ 确认进入
UART 调试工具子页。

## 代码边界

仅动 ui_home.c（launch_app_after_fade 增 1 分支）。与 P272 同轮编入 goldfish；
R528 pack.sh 待跑。
