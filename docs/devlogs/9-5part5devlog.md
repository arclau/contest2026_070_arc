# 9-5 Part5 Devlog — 全量 BUG 修复（14 项：栈溢出/注入/竞态/资源泄漏）

> 日期：2026-09-05｜P180（ok-20260905-39）
> 触发：用户要求「不搞新功能，修 BUG」→ 两轮代码审查（静态分析 + 资源/并发/错误处理）筛出 14 个问题。
> 范围：`vendor/allwinnertech/apps/luncher_dm/` 全目录 .c/.h。

## 审查方法

两轮并行 explore 子代理：
1. **静态分析**：strcpy/sprintf 缓冲区溢出、空指针、命令注入、JSON 注入、类型截断
2. **资源/并发**：内存/fd/timer 泄漏、volatile 缺失、TOCTOU 竞态、线程安全

## 修复清单

### 🔴 高危（3 个）

| # | 文件 | 问题 | 修复 |
|---|------|------|------|
| H1 | `ui_books.c:107` | `strcpy` 栈溢出：`seen[64]` 接收最长 255B `d_name` | → `strncpy` + 强制 `\0` |
| H2 | `ui_files.c:252` | `strncpy` 后缺 `\0`：栈上 `path_copy` 被 `strtok` 越界读 | 补 `path_copy[sizeof-1]='\0'` |
| H3 | `ui_files.c:1060` | `system("rm -rf ...")` 命令注入：文件名含 `"` 可执行任意命令 | 新增 `files_rmrf()` 递归删除（stat+unlink+rmdir），删除 system() 调用 |

### 🟡 中危（6 个）

| # | 文件 | 问题 | 修复 |
|---|------|------|------|
| M1 | `dm_net.c:271` | `memcpy` 无上界：恶意 AP `SSID.len>32` 溢出 `ssid[33]` | 加 `if len>32: len=32` |
| M2 | `dm_net.c:466` | WiFi 配置 JSON 注入：ssid/psk 含 `"` `\` → `wapi.conf` 畸形 | 新增 `json_escape()` 转义写入 |
| M3 | `dm_ai.c:657` | `g_ai_evt_fd` TOCTOU：unsubscribe 检查后 worker 可能换新 fd | 先快照 fd→置 -1→再 shutdown |
| M4 | `dm_uart_dbg.c:321` | 环形缓冲并发写：`clear()` 重置 head/count 与 reader 线程无同步 | 新增 `g_dbg_paused` 标志，clear 时暂停 reader |
| M5 | `dm_net.c:71` | `g_bt_dev_count` 缺 volatile：BT 回调线程写，主线程读 | 加 `volatile` |
| M6 | `dm_net.c:34` | `g_conn_state` 已是 volatile，无需修改 | — |

### 🟢 低危（5 个）

| # | 文件 | 问题 | 修复 |
|---|------|------|------|
| L1 | `dm_health_cfg.c:247` | `strtol` long→int 未检查溢出 | 加 `if (v>INT_MAX\|\|v<INT_MIN) return def` + `limits.h` |
| L2 | `dm_voice.c:147` | `time_t` 差值强转 int 理论截断 | clamp 到 INT_MAX + `limits.h` |
| L3 | `dm_ld2410b.c/h` | 无 deinit：线程/fd 永久占用 UART3 | 新增 `dm_ld2410b_deinit()`（stop+join） |
| L4 | `dm_als.c/h` | timer + uORB 订阅无清理路径 | 新增 `dm_als_deinit()`（del timer+unsubscribe） |
| L5 | `deskmate_ui.c` | `dm_tone_play`/`_seq` 失败路径未调 `dm_audio_fg_release()` → 音乐永远不恢复 | 两函数各 2 处失败路径补 `dm_audio_fg_release()` |

## 改动文件统计

| 文件 | 改动量 |
|------|--------|
| `ui/ui_books.c` | +2/-1 |
| `ui/ui_files.c` | +28/-6 |
| `dm_net.c` | +35/-2 |
| `dm_ai.c` | +4/-3 |
| `dm_uart_dbg.c` | +9/-1 |
| `dm_health_cfg.c` | +3/-1 |
| `dm_voice.c` | +6/-2 |
| `dm_ld2410b.c/h` | +14 (deinit) |
| `dm_als.c/h` | +18 (deinit) |
| `deskmate_ui.c` | +8/-2 |
| **合计** | ~126 增 / 17 删 |

## 编译验证

- `./build.sh ... nsh -j$(nproc)` → **通过，零新增 error/warning**
- 打包 `pack` → **SUCCESS**
- 资源检查 `check_res.sh` → **通过 ✅**
- 固化 tag → **ok-20260905-39**

## 待上板验证

1. **H1 验证**：TF 卡放一个 ≥64 字符文件名 .txt → 进 Books 页不崩溃
2. **H3 验证**：文件管理器删除功能正常（递归删除目录/文件）
3. **M2 验证**：WiFi 密码含特殊字符（如 `p@ss"word`）→ 连接后重启能自动重连
4. **L5 验证**：提示音播放失败时音乐能自动恢复
