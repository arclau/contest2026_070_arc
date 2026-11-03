# P294 — V1 LVGL 无变化刷新收敛

> 2026-10-28。目标：V1 `ynm3000` 横屏 1920×1200 降低常驻状态栏重复失效；不改变视觉与导航。

## 依据与改动

- `clock_update_cb` 每秒写 Home、子页、锁屏各自的时钟和日期。当前 LVGL `lv_label_set_text` 对相同文本仍先 `lv_obj_invalidate`，再重分配与刷新。旧代码最多每秒重复写 6 个标签；分钟和日期未变时无需写。
- `dm_net_status_refresh` 每秒更新三处 WiFi/蓝牙图标。当前 LVGL 的隐藏标志与本地样式 setter 会使对象失效；状态不变时无需调用。
- `ui_home.c` 增加比较当前标签文本的局部辅助函数；处理空指针及文本分配失败。保留每秒网络状态检测、WiFi 连通边沿和首次创建刷新。
- `deskmate_ui.c` 的图标刷新读取对象当前隐藏标志与实际文字颜色，仅变化时调用 setter。没有以全局缓存绑定可能重建的子页对象。
- Qoder CLI `DeepSeek-Flash` 只读复核后处理两处小改动；我核对了实际函数、LVGL 实现和文件差异。既有未提交改动未提交或回滚。

## 验证

| 层级 | 结果 |
|---|---|
| 静态 | 两文件 `git diff --check` 通过；本次 setter 仅在值变化时调用，运行刷新耗时尚未量测 |
| SIM 编译 | V1 goldfish `./build.sh ... --cmake -j16` 成功，最终 ELF SHA256 `fb52bb17ea9af140...` |
| SIM 资源 | 既有 userdata 内 `blue-hour.png` 为 1,969,016 字节；本轮未改资源；客体页面运行未验证 |
| SIM 运行 | 沙箱 QEMU 报实例限制；宿主无窗口启动到 1920×1200 显示配置，但 50 秒内未取得应用运行证据，不能宣称运行通过 |
| 板端包 | 沙箱 `dragon` 报 `Bad system call`；宿主重跑 `pack_deskmate.sh ynm3000` 后 config、ELF、启动、37 资源、nsh、整镜像 PASS；镜像 SHA256 `849406a53be300f9...` |
| 硬件 | 未刷机；帧率、刷新耗时、堆峰值、Home/锁屏视觉与交互均待板端测量 |

## 导出与后续

- 查询 `sync_r528.sh --help` 时，脚本将未知参数当成导出，已同步整个 `r528/` 镜像；其中包含真源已有的大量未提交差异与旧文件删除。两处本轮 C 文件与镜像逐字节一致。未 commit/push/tag，也未恢复镜像旧文件。
- 后续用板端测量刷新面积/耗时，确认收益；再决定是否处理每秒健康环和传感器文本。Qoder CLI 可接限界小任务，主控仍需审 diff 和验收。
- 下一阶段按 [V1 Home/锁屏 LVGL 调优工作单](../specs/ui/v1-lvgl-render-direction.md) 由 AI 或人工分步执行；先做只读 B0，确认量测入口。

## 收尾三段

- **代码**：两处 V1 C 文件通过静态检查、SIM 编译、板端打包整镜像检查；SIM 页面运行及硬件性能待验。`finalize_check.py` 的 CODE WARN 是既有脏仓/头文件提示，不替代构建证据。
- **文档**：`doc_check.py` 仍有 14 条既有 devlog 索引警示；`sync_docs.sh --check` 镜像一致，未新增断链。
- **会话日志**：Codex session `01a17fd7-cffe-7d10-a77c-03c91cad7c9b` 的定向 dry-run 返回 `No sessions matched`；无可核对的非零事件数/来源工具，未导出或伪造补录，`finalize_check.py` 为 LOGS MISSING。
