# P291 — V1 Home/锁屏玻璃材质与壁纸

> 2026-10-27。目标：V1 `ynm3000` 1920×1200 横屏，Home/锁屏接近 iOS 26 的透底、边缘和层次感；兼顾资源预算。
> 用户随后要求停止截图；此前曾采到一张设置页叠层画面，不能作为材质基线。此后未再截图。壁纸已在模拟器更新，视觉观感与锁屏唤醒待用户/板端复核。

## 范围与依据

- P271 记录的有效方向：淡彩背景透过白色半透明卡、细白边、对称柔影；贴图会露方角，硬边色块会在卡内形成接缝。
- 开始时 V1 `style_card` 为白底 `LV_OPA_90`、无边框、8px 黑影；Home 背景为浅灰渐变，锁屏为纯 `COL_BG`。这解释玻璃层次变弱，但不是任何运行问题的根因判定。
- `luncher_dm` 既有未提交差异保留。V2 源码未修改。完整设计与后续阶段见 [V1 UI 材质与资源优化方案](../specs/ui/v1-glass-ui-optimization.md)。

## 本轮改动

1. 新增 V1 `dm_glass.h`，集中淡彩退路、壁纸入口、卡面遮盖度、边框和阴影参数。无全屏子对象或定时动画。
2. Home 与锁屏的现有屏幕对象绘制同一张蓝调壁纸；渐变作为资源缺失时的退路。共享 `style_card` 改为 70% 白底、1px 半透明白边、16px 对称蓝灰柔影，卡内不绘渐变。
3. 锁屏双圈与欢迎卡调用同一材质辅助函数；底部健康卡不再覆盖为蓝色光晕。布局尺寸、事件、数据路径保持当前 V1 实现。
4. `style_card` 也被音乐播放列表使用；该页已有局部 80% 遮盖度与阴影覆盖，材质影响需在板端复核。
5. 用户选定蓝调方向后，首版用了分层曲线与少量暖光。用户在模拟器看到后反馈不像连续渐变且观感差；参照 iOS 26 浅色壁纸的大圆弧蓝青过渡重新生成横屏版。最终 1920×1200 RGB PNG 在板端 `/resource/wallpaper/blue-hour.png`、SIM `/data/data/wallpaper/blue-hour.png`；两屏复用，未增加交互层。首版备份在 `output/p292-v1-wallpaper/blue-hour-before-gradient.png`。

## 构建与验证

| 项 | 结果 |
|---|---|
| 静态 | `git diff --check`（本轮两个 C 文件）无新格式错误；材质入口及调用点已核对 |
| goldfish | 最终 `./build.sh ...goldfish-arm64-v8a-ap --cmake -j16` 成功，ELF SHA256 `e631c137…`，日志 `output/p292-v1-wallpaper/goldfish-build.log`；SIM 资源源与板端 PNG SHA256 同为 `1d0e54f7…` |
| SIM userdata | `repair_emu_resources.sh` 停机保留备份并验证 183 份用户文件不变、175 项资源；镜像内 `/data/wallpaper/blue-hour.png` 为 1,969,016 字节；模拟器已重启。脚本的旧“宠物首帧”日志判据未出现，故客体运行验收未闭环 |
| V1 板端 | 第二版接入后 `pack_deskmate.sh ynm3000` 成功；`check_deskmate.py` 报 config/ELF/启动/37 资源/nsh/整镜像 PASS；日志 `output/p293-v1-gradient/ynm3000-pack.log`；镜像 `output/ynm3000/ynm3000.img` SHA256 `86e662c5c035e78869660546768ae204bfc01fb361e99bd1a2d4393b3f71676e` |
| 运行/观感 | 用户已在模拟器看过首版并要求重做；第二版的 Home/锁屏观感、文字对比与唤醒仍待用户确认。无截图；板端仍未测 |
| 资源预算 | 最终壁纸 PNG 1,969,016 字节；板端 ROMFS 已纳入。LVGL ARGB8888 全图解码约 9.2 MB；缓存复用、堆峰值和刷新耗时未实测。本次新增资源，不能宣称降低占用 |

首次 goldfish 构建在沙箱内因 ccache 目录只读失败；宿主构建通过。壁纸后的首次 V1 打包在沙箱内因 `dragon` 报 `Bad system call` 未通过整镜像校验；同一命令在宿主重跑通过。V1 pack 日志中的 `usrdata.fex` 缺默认数据警示由打包器回退处理，最终资源及整镜像校验通过。

用户首次刷新模拟器仍是旧界面：当时仅板端资源和固件更新，goldfish 程序与已有 userdata 未刷新。现已按 AGENTS 新规同步两目标资源、重编 SIM 并停机修复 userdata。修复脚本原用 QEMU 命令行中的绝对 `nuttx` 路径判断归属，对当前实例误报；改为核对进程工作目录和可执行文件路径后通过安全检查。脚本输出的内网地址已脱敏，未改变保留用户文件与资源哈希的校验。第二版资源更新完成，但脚本所查宠物日志未出现，不能以“started”认定 Home 运行通过。

`vendor/openvela/boards/.gitignore` 原规则会忽略整个 SIM `prebuilts/data` 目录；已仅对 `wallpaper/blue-hour.png` 开跟踪例外，其余资源仍被忽略。否则本机可见的新壁纸不会随源码交付。

## 导出边界与下一步

- `sync_r528.sh` 的预览会删除 `r528/luncher_dm` 镜像内 33 个真源已无的文件（含 V2 风格旧资源/工具），本轮未执行该全量导出，避免覆盖既有镜像差异。
- 板端先复核视觉与点击；若材质过白或色带明显，按方案一次只调一组参数。之后再开展页面样式收敛和资源量测。

## 收尾三段

- **代码**：goldfish 构建、SIM userdata 文件校验及 V1 板端构建/打包/37 项资源/整镜像检查通过；`finalize_check.py` 静态 `CODE WARN` 为现存脏仓和双构建头提示，不替代上述证据。第二版运行观感与板端运行待验。
- **文档**：主源已由 `sync_docs.sh` 导出且镜像一致；`doc_check.py` 仍报 14 条既有 devlog 索引警示，本轮未新增链接错误。
- **会话日志**：Codex session `01a17d2d-d706-7833-9a86-6e537a16e3b1` 用 `export-session.py --session ... --dry-run` 检查为 `No sessions matched`，`finalize_check.py` 为 `LOGS MISSING`；无可核对事件数或来源工具，因此未导出，也未伪造补录。
