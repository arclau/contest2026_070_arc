# P259 — goldfish 首刷崩溃根因定位与修复（init 栈溢出）

> 日期：2026-10-16。平台：SIM（goldfish-arm64-v8a-ap）。状态：**崩溃已修复并复现对照**；中文/宠物资源显示另列。
> 承接 [P255 交接](10-16-emu-repair-handoff.md)。本文是新会话实际运行后的根因结论，取代 P253 的“根因未知”。

## 1. 结论

`luncher_dm_main` 作为 **init 入口**运行，用的是 `CONFIG_INIT_STACKSIZE`（当时 **8192 = 8K**），
不是 `CONFIG_LUNCHER_DM_APP_STACKSIZE`（512K，仅 NSH 起 app 时才用）。
首刷 `lv_refr_now` 递归深度超过 8K → 栈向下越过栈底，压坏栈底的 **TCB/TLS**（`tls_get_info` 取 `stack_alloc_ptr`），
于是 `task_get_info()->tl_task` 变成野指针；下一次 `puts()` 用野 `stdout` 调 `flockfile` → Data Abort → recursive assert。

**这与“渲染器越界写”“锁实现有错”“GPU 模式”无关**；P253 的 syslog 禁用、pthread 地址阈值、跳过宠物等只是掩盖症状。

## 2. 证据（同次构建 ELF 反解，2026-10-16）

临时在 `arm64_fatal.c` 的 data abort 路径加 up_puts + FP 回溯（诊断用，已还原）后抓到：

```text
X0=29300000a25 X1=4072e780 X2=40a9c910 X3=b8
FP=40ebaa50 LR=404d0038 SP=40ebaa40 ELR=404d2188
  bt[0] lr=404d0038  -> puts        lib_puts.c:59
  bt[1] lr=402ee3dc  -> luncher_dm_main:684（编译器把 printf("[bc] refr_now OK") 折叠成 puts）
  bt[2] lr=402ee3dc  -> nxtask_startup:66
```

- 调用链：`nxtask_start → nxtask_startup → luncher_dm_main → puts → flockfile → nxmutex_get_holder`，FAR 为野 `stdout`。
- 行 682 的 `puts` 正常（浅栈），行 684 的 `puts` 崩 → 崩点在 `lv_refr_now` **返回之后**，即刷新期间栈已溢出。
- 成功启动后主循环里 TLS 稳定：`tls_pre tl_task=0x40a900c0 tl_mhead=0`，无异常。

## 3. 修复与验证

- `vendor/openvela/.../goldfish-arm64-v8a-ap/defconfig`：新增 `CONFIG_INIT_STACKSIZE=524288`（与 APP_STACKSIZE 一致）。
- 单变量对照：仅改此项 → 首刷不再崩，进入主循环，抓屏见 1440×720 主屏（图标/时钟/健康条），时钟持续走动。
- 还原 P253 临时补丁后仍正常：syslog、pthread_mutex_inconsistent、mm/kasan/hook、arm64_fatal 全部 `git checkout`；
  应用 `luncher_dm.c`（恢复 libuv 路径、宠物挂载、去强制首刷/面包屑）、`ui_home.c`、`deskmate_ui.c`（去 PWM 守卫）同样还原 → 运行正常。
  - 说明：goldfish 的 `hal_pwm_control` 已由 `sim/dm_sim_stubs.c` 桩掉，亮度守卫本就多余。
- KASAN 恢复为 defconfig 原值 `CONFIG_MM_KASAN=y`（需同时选子模式 `MM_KASAN_GENERIC`，否则 `kasan_*` 链接失败）；开启后运行无告警。
- `prebuilts/emulator/tools/emulator.sh`：保留去掉 ARM `virtio-snd` 注入（bus full 致 qemu 退出，实证）。

## 4. 顺带修掉的构建阻塞

`include/nuttx/config.h` 比 `.config` 旧：缺 `CONFIG_LV_USE_FREETYPE` → uikit `lv_freetype_font_create` 隐式声明编译失败。
`nuttx/cmake/nuttx_mkconfig.cmake` 在 `.config == .config.prev` 时跳过重生成；`touch nuttnx/CMakeLists.txt` 触发 reconfigure 即修。

## 5. 仍待办（与本次崩溃无关，另列）

- 主屏**中文与宠物未显示**：guest 侧 `/data/data/{fonts,pet}` 未挂载/读不到（goldfish 数据分区 FS 链路待查），非崩溃。
- P255 阶段 B：统一 `doctor/build/start/status/stop/logs` 入口与文档，未做。

*证据副本：`.emu-debug-backup/`（各仓 HEAD/diff、截图 shot3、日志 /tmp/emu.log）。*
