# P312 — V1 宠物素材解码损坏（未解决，交接）

2026-10-31。V1 `ynm3000` 板端。**结论：未定位。** 本文只记录已验证事实、已排除项和剩余线索，不写"已修复"。

## 一句话现状

宠物素材 PNG 解码失败率极高，**壁纸同时开始报错**，WiFi 固件下载也失败。三者都是"从文件读数据 → 解压 → 得到垃圾"。

## 现象

| 通道 | 日志 | 频率（25s） |
|---|---|---|
| 宠物 | `[pet] PNG decode failed asset=N: IDAT: CRC error` | 10 |
| 壁纸 / LVGL | `decode_png: png decode failed: IDAT: CRC error` | 38 |
| WiFi | `RTL8723X: download_fw: download firmware FAIL! status=0x27` | 每 boot 2 次 |

三种错误串全部是 **zlib 输入侧**错误：`IDAT: CRC error`、`IDAT: invalid distance too far back`、`bad adaptive filter value`。说明数据进了 inflate 就是坏的，不是解压后处理错。

**关键特征：同一个 asset 有时成功有时失败。** `resource_ok=1` 与 `resource_ok=0` 在同一段日志里交替出现，`asset=0` 在 `idle_sit` 里成功、在 `settle` 里失败。→ 不是静态文件损坏。

## 已确认事实

- 服务器 `output/ynm3000/res.fex`：扫描全部 85 个 PNG 签名，逐个验 chunk CRC，**85/85 通过，0 坏**。
- 板端 `/resource/wallpaper/blue-hour.png` = 1,611,686 字节，与 P298 记录一致。
- 板端 `/resource/pet/revision` = `b8239a6ce265bbbd`，与 P305 期望一致。
- 板端 `/resource/pet` 目录 84 个文件，名字完整 68 字符（含 `.png`）。
- `lv_fs_load_with_alloc()` 严格校验 `bytes_read == size`，日志无 `read ... failed` → 整文件读入且字节数正确。
- 宿主机回归测试（ASan+UBSan）`contest2026_070_arc/tools/tests/pet_actions/run.py`：**9/9 PASS**，其中含"82 images, BGRA/mirror pixel CRCs, 2MiB bound, pinning, revision and decode errors"——同一批 PNG 在宿主上逐像素 CRC 全对。
- 日志中**无** NAND/ECC/bad block 报错。

## 已排除（含验证方式）

| # | 假设 | 排除依据 |
|---|---|---|
| 1 | 资源打包损坏 | res.fex 85/85 PNG CRC 通过 |
| 2 | 文件不存在 / 文件名被截断 | `CONFIG_NAME_MAX` 64→80 后，`ls` 显示完整 68 字符名，`PNG open failed` 归零 |
| 3 | 尺寸不符 | 无 `PNG dimensions invalid` 日志 |
| 4 | 缓存槽耗尽（5 槽全 pin） | 无 `PNG cache exhausted` 日志 |
| 5 | `malloc(PET_FRAME_BYTES)` 失败 | 无 `PNG allocation failed` 日志 |
| 6 | 解码器逻辑错误 | 宿主 82/82 像素 CRC 通过 |
| 7 | 堆区超出物理内存 | `CONFIG_RAM_SIZE` 917MB→108MB 已生效（`free` total 902MB→93MB），**错误依旧** |

> #7 值得记一笔：原值 `961943040`（917MB）在 128MB DRAM 的板子上是错的，虽然修完不解决本问题，但它本身是个真实缺陷，建议保留这个修正。

## 剩余假设（按可信度排序）

**H1. G2D / DMA 与 romfs 读取的总线冲突（最可疑）**

依据：p308 版宠物因 NAME_MAX 无法解码时，`decode_png` 报错数为 **0**；NAME_MAX 修好后宠物开始解码，壁纸**同步**开始报错。也就是"加入一个解码者，把另一个原本正常的解码者搞坏了"。

日志显示 G2D 负载很高：`[fb] upd avg: clean=~400us g2d+cfg=~15000us`。若 NAND/romfs 读与 G2D 共用 DMA 通道或总线，读回的数据可能被破坏。

**建议实验**：临时把 `dm_perf` 的 G2D 路径关掉（或改用纯 CPU 绘制），只保留宠物解码，看 `decode_png` 是否归零。这是最能区分的一条。

**H2. romfs / NFTL 读在特定时序返回坏数据（ECC 未上报）**

NAND 在 boot 阶段曾出现 `nand wait_reg_status timeout` / `no nand found !` 后重试成功（见烧录日志），说明读路径有过抖动。若 ECC 纠错静默失败或未上报，数据会坏但日志干净。

**建议实验**：用 nsh 反复 `cat` 同一个 PNG 到 `/tmp`，配合 `cmp` 或把文件读进内存后算校验（板上无 md5sum，需先加一个简单 hash 命令），看是否稳定。

**H3. libpng / zlib 的非线程安全或静态状态**

若 libpng 被编译成共享静态缓冲，多处并发解码会互相踩。目前解码都在 LVGL 主线程，理论上不并发，但 `ai_agent` 等线程是否也走解码需确认。

**建议实验**：确认 `CONFIG_LV_USE_CLIB_MALLOC=y` 下 libpng 的 `png_create_read_struct` 是否每次独立；检查是否有全局 `png_struct` 复用。

**H4. DRAM 时序不稳**

`CONFIG_RAM_SIZE` 之外，DRAM 参数（`dram_para`）与 792MHz 配置是否对本板余量足够未验证。此假设优先级最低，因为其他子系统未见随机崩溃。

## 本轮改动清单（P307–P311，均已进包）

| 节点 | 改动 | 文件 |
|---|---|---|
| P307 | 喂食弹窗改用统一 `dm_popup_*` 玻璃语言，只留「喂鱼/逗猫」；idle 池按饥饿/精力/心情/冷落时长动态调权；满腹不再刷成长；删除死函数 `pick_from_pool` | `pet_view.c`, `pet_core.c` |
| P308 | `notice_user` 峰值帧 500→1900ms（持续注视）；`groom_paw` 280→700ms + 「舔舔~」气泡；`happiness<20` 呆坐不眨眼 + 「……」 | `pet_actions.c`, `pet_core.c` |
| P309 | 暗光提醒 `DM(500)`→`DM(260)`（1200→624px）；宠物弹窗 `DM(460)`→`DM(360)`，按钮 422→312px | `ui_home.c`, `pet_view.c` |
| P310 | `pet_assets.c` 失败日志补 `png.message`（**关键诊断手段，建议保留**） | `pet_assets.c` |
| P311 | `CONFIG_RAM_SIZE` 961943040→113246208 | `ynm3000/configs/deskmate/defconfig` |

改动前原件备份：`output/rebuild-backup/*.before-p307` / `*.before-p308` / `defconfig.before-ramsize` / `defconfig.before-name-max`。

## 已知环境事实（省得重查）

- 构建：`bash /data/dm/output/rebuild.sh`（内含 `distclean` + `pack_deskmate.sh ynm3000`），约 3 分钟，日志 `output/rebuild.log`。
- 回归测试：`python3 contest2026_070_arc/tools/tests/pet_actions/run.py`（带 `-Werror`，能抓死函数，**每轮改动必跑**）。
- 串口：Windows 侧 COM3 @1500000；服务器侧 `/dev/ttyUSB0`。
- 刷机镜像：`output/ynm3000/ynm3000.img`，`vela.bin` 与 `nsh.fex` md5 必须一致。
- 本机 USB 驱动曾因 `oem25.inf` 被清除导致「未知设备」，修复包在 `~/Downloads/全志USB烧录驱动20201229/UsbDriver`。

## 给接手者的最短路径

1. 先做 **H1 的实验**（关 G2D 看 `decode_png` 是否归零）。这一步信息量最大，且不需要改资源或解码器。
2. 若 H1 排除，再做 H2（读同一文件多次比对校验）。
3. 不要再去查资源打包和文件名——那两项已经验到底了。
