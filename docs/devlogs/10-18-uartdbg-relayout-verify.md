# UART 调试工具重排 · goldfish 渲染验收（P272 迁移示例 / P273 入口）

目标：按新 1440×720 重排 uart_dbg（左竖向工具栏 + 右日志），goldfish 编译+运行验收；R528 待打包。
平台：共享 LVGL 源码；goldfish 构建+运行；R528 触摸/实机待验。

## 布局（ui_uart_dbg.c 重写，Option A）
- 左栏宽 300px（x26–326）竖向玻璃工具栏：端口卡 `UART1·波特·○`、5 个等高分栏操作钮
  （清屏/注入/HEX/暂停/回车，各带主题色图标+文字）、3 主题色点、底部命令框+蓝色「发送」钮。
- 右区日志终端（x338–1414，主题深色底）占满剩余，纯矩形，无重叠/越界。

## 编译修复（LVGL v9 不兼容）
- 本树 v9 已删 `LV_FLEX_ALIGN_STRETCH` → 三处改 `LV_FLEX_ALIGN_START`；
  交叉轴填充改显式 `LV_PCT(100)`（port_card/ops/theme_row 宽度）。
- goldfish 重建通过（约 10s，exit 0），ui_uart_dbg.c 干净编入镜像。

## 渲染验收（goldfish 1440×720）
- 启动 → 点 dock UART 磁贴 (795,553)（P273 同坐标）→ 进 UART 子页。
- 截图像素核验：顶栏浅色；左栏玻璃 (230,235,247)；发送钮蓝 (16,130,247=COL_BLUE)；
  右日志终端深底 (41,45,49)，x≥338 起、右抵边，结构正确。
- 日志区显示预期占位「打开 /dev/uart1 失败」（goldfish 无真实串口，UI 外壳正确）。

## 附：VNC 访问恢复（run-emu.sh）
- 现象：http://<private-ip>:6080/vnc.html 打不开。
- 根因：上轮验证完截图后关闭模拟器+Xvfb，x11vnc/websockify 随依赖一起退出。
- 修复：`bash /data/vela/run-emu.sh restart` 重建 Xvfb:99 → qemu(-gpu swiftshader)
  → x11vnc:5900 → websockify:6080(--web /usr/share/novnc)；setsid+nohup 常驻。
- 现状：:5900/:6080 LISTEN 0.0.0.0，vnc.html HTTP 200，可实时看 HOME/子页。

## 代码边界
- 仅动 ui_uart_dbg.c（重排 + LV_FLEX 修复）。与 P272/P273 同轮编入 goldfish；
  R528 `pack.sh` 已跑通（build+pack+check_res 通过），镜像可烧录；硬件观感待验。
