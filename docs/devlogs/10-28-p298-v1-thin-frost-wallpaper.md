# P298 — V1 壁纸改为原图薄霜版

> 2026-10-28。目标平台：V1 `ynm3000` 1920×1200 与 goldfish V1。验收现象：Home/锁屏共用原蓝青大圆弧壁纸，色彩略柔和，但不泛白；不截图，视觉由用户复核。

## 素材与改动

- 用户反馈当前 P297 淡化版过白，要求从第一张蓝青大圆弧壁纸加薄霜。源文件确认是 `output/p297-v1-soft-glass/blue-hour-before.png`；P292 更早的分层曲线稿构图不同，不用于本次。
- 图像编辑模型的候选稿移动了弧线，未接入。为严格保留原图位置和纹理，只对 P297 原图每个 RGB 像素做 `90% 原色 + 10% 白色`，无模糊、形变或新增图层。输出 `output/p298-v1-thin-frost/blue-hour-thin-frost.png`。
- 平均 HSV 饱和度从 165.37 降至 145.35（0–255），平均亮度从 144.45 升至 155.06。PNG 1920×1200 RGB、1,901,814 字节、SHA256 `1633bd228d526b52041e0c03772a331d391a982355e121ca72d7129b848a6d25`。
- 同一 PNG 已替换板端 `lichee/board/r528s3/ynm3000_nand/data/res/wallpaper/blue-hour.png` 和模拟器 `boards/vela/prebuilts/data/wallpaper/blue-hour.png`，两份哈希一致。C 代码和资源路径未改。

## 验证

| 范围 | 证据与结论 |
|---|---|
| 静态 | Home/锁屏均调用 `dm_glass_apply_wallpaper`，指向同一 `blue-hour.png`；尺寸及两份源资源哈希一致 |
| goldfish 构建 | `build.sh ...goldfish-arm64-v8a-ap --cmake -j16` 成功；ELF SHA256 `3e335873ea9ec92540eebed48b2517e462d1f73c864b8408c6db684e9b1c21eb` |
| 模拟器资源 | 宿主修复脚本停机备份 userdata、逐文件验证 175 项资源、重启；QEMU 仍在运行且 `/tmp/emu.log` 有持续 `[perf]` 刷新记录。脚本的旧宠物首帧判据未满足，故返回 1；壁纸客体解码与视觉未由日志单独证实 |
| V1 板包 | `build.sh ...ynm3000 -j16` 成功，宿主官方 `pack_img.sh -b r528s3-ynm3000` 成功；`vela.bin` 与 `nsh.fex` MD5 同为 `51093191f5ae9856edf598f6d6ac600e`；`res.fex` 含逐字节相同的 PNG；整镜像 MD5 `6f545390d23f221b5c8f3af82519506b`，副本见 `output/p298-v1-thin-frost/ynm3000-thin-frost.img` |
| 板端运行 | 新包未刷板；颜色、文字对比和锁屏唤醒待用户复核 |

沙箱内 goldfish 编译因 ccache 只读失败，宿主重跑通过；沙箱内 pack 的 `dragon` 报 `Bad system call` 却返回 0，宿主重跑无该错误并生成新镜像。两次失败日志保留在 P298 输出目录，不作为通过证据。

## 收尾

### 用户板测反馈与追加 30% 白霜

- 用户刷入 `ynm3000-thin-frost.img` 后反馈饱和度仍高。直接检查该整镜像，包含原 thin PNG 的完整字节，MD5 仍为 `6f545390d23f221b5c8f3af82519506b`；可确认交付包已替换图片，不能据此单独核实设备当前分区。
- 用户明确要求在 thin 上再叠 30% 白霜，故采用 `70% thin + 30% 白色`，不是从原图重做 30%。新图 `output/p298-v1-frost30/blue-hour-frost30.png`，1920×1200 RGB，1,611,686 字节，SHA256 `dd73c505c398f5573d4205fd6116145f0254c4e003727a889201f973532a23d3`。
- 两目标资源源文件已同步；本次验收平台为用户正在刷机的 V1 板端，模拟器运行镜像仍是先前 thin 版，未宣称更新。
- V1 官方 `build.sh` 构建及宿主 `pack_img.sh` 打包通过。新 ROMFS 和整镜像均包含新 PNG 完整字节，`vela.bin==nsh.fex`。证据：`output/p298-v1-frost30/verification.json`。
- 新包 `output/p298-v1-frost30/ynm3000-frost30.img`，54,402,048 字节，MD5 `aa13efc0693d0891584a6e8276213813`。新包尚待用户刷机复核。

- 代码/资源：两目标源资源与构建、板包已核；模拟器视觉和板端运行待验。
- 文档：本节点与 devlog 已导出，`sync_docs.sh --check` 一致；`doc_check.py` 留有 17 条旧节点未索引警示，本节点无新增问题。索引 40 行、每行不超过 120 字符。
- 会话日志缺失：Codex session `01a180a6-091c-7e21-b63e-cc95c61513ee` 的定向导出预检返回 `No sessions matched`，没有可核对的事件数，未伪造补录。
- 源码镜像：`sync_r528.sh --check` 检出既有应用代码差异后退出 2。本轮仅换 PNG，manifest 未映射此壁纸路径；未执行会覆盖既有差异的全量源码导出。
- `finalize_check.py`：CODE WARN（已有脏仓与生成头提示）、DOCS WARN（上述索引警示）、LOGS MISSING。历史日志 190 份的凭据模式扫描命中 16 份，仅报告路径/数量，未输出正文；命中尚未逐项判定，未提交或上传。
