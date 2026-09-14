# 9-11 part1 devlog — P212~P215：res 烧录排障 / 打包防呆 / 提示音尾音根治 / 公共 PR 决策

> 日期：2026-09-11 ｜ 基线 ok-20260911-15 → **ok-20260911-19**

## 一、P212 911 bootlog 排障：res 分区未完整烧录（ok-16/17）

**现象**：911 bootlog `RTL871X: download_fw: download firmware FAIL! status=0x27` → `Start WIFI Failed!` → 全程 `ioctl[SIOCGIW...]` 刷屏；字体 U+F015 取不到（对比 910 正常）。

**排查**：
- 910↔911 build 源码 diff：WiFi 驱动目录 / `res/` / `rcS`/`start_wifi` / wifi defconfig **全无改动** → 排除代码回归。
- `status=0x27` = HALMAC 固件下载校验失败。旧 res 里固件是同一文件、校验会通过；只有**写坏/截断**才 fail → 板端 res 非最新/未完整写入（烧录环节，非打包）。
- 打包产物正确：`check_res.sh` 路径级+字节==源、整包镜像含完整 res 块。

**处置**：重编重打整包（烧录务必含 res 分区）；固化 **ok-20260911-17**（nsh md5 `b679d4a6`）。

## 二、P213 主机打包防呆（ok-18）

- `check_res.sh` 升级为产物自证：缺产物即非零退出 / `nsh.fex==vela.bin` / 整包镜像含完整 res+nsh 块 / res 源新鲜度 / 输出 md5 指纹。
- `pack` 末尾装 `post-dragon` 钩子自动调用（**失败即中断打包**）；权威副本 `/data/vela/post-dragon`（`.hooks` 被 SDK gitignore）。新增 `/data/vela/pack.sh` 一键（编译+打包+补钩子）。
- AGENTS §二第 4 步、§四坑 3③ 同步。
- 固化 **ok-20260911-18**（镜像 md5 `07151be8`）。

## 三、P214 健康提示音"尾音"根因与修复（ok-19）

**现象（用户）**：**只有喝水/久坐的 WAV 提示音**播完"多一声"尾音；音乐、AI 回复均无。

**根因**：`tone_play_one`（`luncher_dm/deskmate_ui.c`）**只设 hw_params，漏了 sw_params**。健康提示音文件短，EOF 后 DMA 欠载；缺 `silence_size` 时硬件不补静音而**重播上一段** → "多一声"。音乐 `dm_sound_open`、AI `audio_playback_open` 都设了 `sw_params` → 干净。devlog P137/P140/P141/P174 追的"驱动关断顺序"是**另一类** PA 关断爆音，与本尾音无关。

**修复**（App 侧，1 文件）：补与工作路径完全一致的 `sw_params`（`start/stop_threshold=buffer`、`silence_size=boundary`、`avail_min=period`）；顺带 `writei` 返回值检查 + XRUN(EPIPE) `prepare` 恢复。

**产物**：nsh md5 `de90c4e8` / 镜像 md5 `56136c19`；固化 **ok-20260911-19**。**上板验证已过**（用户确认尾音消失）。

## 四、P215 公共仓 PR 范围决策 + 子 APP 遍历审查

**公共 PR 决策**：尾音真因在 App 侧 → **公共 PR 去掉 P140/P141**（`sunxi_codec_playback_lineout_route` / `hp_route` 两处 OFF 分支）；保留麦克风关断 POP（ok-15，`sunxi_codec_dapm_control` capture 分支）+ `de_dsi` + BOE 面板 + `ltr553` + LVGL 三 fix。已写入 `project_docs/submission/提交检查清单.md §五`。

**子 APP 遍历审查**（并行审全部 UI/服务/宠物模块，**未修，暂缓同步 fork**）：

| 模块 | 高危项 |
|---|---|
| ui_wifi | `scan_done` 粘滞标志 → 每 500ms 全量重建列表（UI 抖动 + 触摸中的行被清） |
| ui_files | 复制线程默认 4KB 栈 + 递归每帧 ~1KB → 栈溢出；关页不停 worker 可重入共享状态 |
| ui_music / deskmate_ui | `music_ui_clear_ptrs` 漏清 `music_playlist_overlay`/`music_empty_lbl` → 潜 UAF；`DM_FORCE_MONO` 用 `#ifdef` 判定，值为 0 也生效 → 音乐每包强制混单 |
| dm_ai | EOF 后 errno 残留 EAGAIN → 死 fd 忙等；127 帧 `(int)plen` 负值绕过截断 → 越界写 |
| dm_uart_dbg / dm_ld2410b | ring head/count 跨线程竞态越界；poll 未校验 revents → 错误忙转 |
| 其他 | luncher_dm 湿度 pollfd 写死下标；dm_voice `memcpy` 用错 sizeof；pet_core 存档并发；dm_alarm 旧档解析错位 |

**结论**：待高危项修完再同步 `r528/luncher_dm` 进 fork（fork 已明显落后当前源码）。

## 待办
1. 修上述子APP高危项 → 复编复核。
2. 同步 fork `r528/luncher_dm` + 脱敏（UDISK wapi.conf）→ push（需用户点头）。
3. 公共仓 PR（去 P140/P141 后）+ CLA；交付物 README/报告/视频/日志。

## 五、P216 子APP崩溃/挂死类6项修复（ok-20，用户定范围：只修崩溃挂死）

- `ui_music`：`music_ui_clear_ptrs` 补 `overlay/empty_lbl` 置空（UAF）。
- `deskmate_ui`：`#ifdef DM_FORCE_MONO` → `#if defined&&值`（0 值误触发致恒混单）。
- `ui_wifi`：加 `wifi_list_built` 守卫（对照 bt），新扫描/进页/退出清零，粘滞不再每500ms重建。
- `dm_ai`：`ws_recv_text` 全路径 EOF 置 ECONNRESET（残留 EAGAIN 忙等根治）；127 帧改 size_t 比较+超长截断收+丢尾保流对齐；调用方 ping(n==0)→continue。
- `dm_uart_dbg`+`dm_ld2410b`：poll 成功须验 `revents&POLLIN`，ERR/HUP 勿 read 忙转。
- `dm_alarm`：fscanf 跨行偷数 → fgets 按行+sscanf，坏行跳过不丢整档。
- 编译过 / pack 自证过 / 固化 **ok-20260911-20**（nsh `c5e508da`/镜像 `f42e8714`）。
- 剩余未修（files 栈/并发、ring 竞态、pollfd 下标、voice memcpy、pet 存档锁）：冻结，下轮再说。
