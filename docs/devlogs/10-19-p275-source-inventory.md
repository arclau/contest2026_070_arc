# P275 · 源码盘查与 UART 收口复核

日期：2026-10-19。状态：**UART 回车/字形已在源码、离屏测试和 goldfish 实跑中成立；设置右栏已改平并进入 10:14 R528 包；板测未做。**
本节点先做盘查与文档校正，后按用户要求改设置右栏并重建 R528 包（改动未提交）。

## 1. 盘查结论

| 项 | 状态 | 证据 |
|---|---|---|
| vendor 提交 | `4c122d89`（2026-10-19 03:14）“P275: subapp flat design language + UART enter/glyph fix” | `git log`；22 文件 +2420/−1527 |
| UART Enter 事件 | 已修 | 键盘映射用可见文本 `"Enter"`（不用无字形的 `LV_SYMBOL_NEW_LINE`）；`LV_EVENT_READY` 绑在 textarea，Enter/OK 单一路由 |
| UART 字形 | 已修 | 键盘浅拷贝 `lv_font_montserrat_30` + `dm_font_symbol`(FA) fallback，补 BACKSPACE U+F059；不动全局字体/共享键盘 |
| 离屏复跑（今日全新目录） | 通过 | 文件 11 项、工具 8 项，[files-checks](../specs/ui/tool-pages-p275/files-checks-2026-10-19.txt)、[tools-checks](../specs/ui/tool-pages-p275/tools-checks-2026-10-19.txt) |
| goldfish 构建 | 通过 | 01:25，SDK build.sh，10 秒，`/tmp/goldfish_build.log` |
| goldfish 实跑 UART | Enter 路径成立 | 点输入框→键盘出现且 Enter/OK/退格无缺字；输入 `gi` 点 Enter→状态变“串口未连接，命令已保留；请重连后发送”，证明 Enter 走到发送逻辑 |
| goldfish 实跑其它页 | 已看 | 主屏、设置(网络)、文件列表；[截图](../specs/ui/tool-pages-p275/goldfish-home.png)等 |
| goldfish 串口真实收发 | 未验 | 模拟器无 `/dev/uart1`，`dm_uart_dbg_open` 失败；只能证明 UI 路由，不能证明 TX/RX |
| R528 打包 | 通过 | 02:22 包（md5 `55deff18…`）含 UART；10:14 新包（md5 `aec4e204…`）另含设置右栏平整，check_res 全过 |
| 设置右栏平整 | 已改并重新入包 | `settings_group_flat` + 去掉外层白卡；10:14 R528 build/pack/check_res 通过，镜像 md5 `aec4e204…`；[离屏截图](../specs/ui/tool-pages-p275/settings-flat-panel.png) |
| 设置右栏离屏 | 通过 | 今日 tools 复跑编译的是工作区源码，七分类切换/重复选择/重开均 PASS |
| r528 提交镜像 | 落后 | `sync_r528.sh --check` 报 DIFF；镜像文件停在 10-16，tests 只有 files_offscreen |
| 板端触摸/TX/RX/G2D | 未验 | 无硬件证据 |

## 2. 被推翻的旧交接结论

旧交接 §5 第一条“UART 键盘回车事件与字形：下一步第一项”**已完成**，不应再按原计划重做事件路由或改共享键盘。
分栏落地细节见 [10-18-uartdbg-leftright-fix.md](10-18-uartdbg-leftright-fix.md)。

旧 §5 的 `uart_tx_enable` 重新绑定、240 字节上限、RX 预算、失败保留命令等项，现有离屏测试已覆盖；**底层共享 ring buffer 的并发安全仍未验证**，结论保持未知。

## 3. 当前真正的剩余项

1. ~~设置右栏 `settings_group_flat`~~：已保留并去掉外层白卡，R528 包已重建；goldfish 实机画面待看。
2. 设置真实内联详情（Wi-Fi/蓝牙/日期/宠物）布局与返回：仍只有桩。
3. r528 镜像同步 + tools_offscreen 一并导出，属交付动作，不是验证。
4. 板端：UART 实际 TX/RX、触摸、G2D、观感。
5. 文档预算：devlog 49 行超 40；P274～P272 三行超 120 字符，已在本轮压到限内。

## 4. 复跑入口

```bash
python3 vendor/allwinnertech/apps/luncher_dm/tests/files_offscreen/run.py --output /tmp/dm-files-next
python3 vendor/allwinnertech/apps/luncher_dm/tests/tools_offscreen/run.py --output /tmp/dm-tools-next
```

goldfish 当前实例：PID 43458/43487，VNC `:5900`、noVNC `:6080`；截图用 `DISPLAY=:99 import -window root`。
模拟器 `-no-window` 下无法截屏；本次实例带窗口，可直接看目标页。

## 5. 边界

- 设置右栏已按要求改平并重建 R528 包；改动仍未提交，按用户要求保留在工作区。
- 未 commit、未 push、未打 tag、未同步镜像。
- 截图是 goldfish 模拟器画面，不是 R528 板端。
