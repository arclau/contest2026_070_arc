# P314 — V1 宠物解码：输入快照 + 期望 CRC 诊断包

2026-10-31；目标平台 R528 `ynm3000`，V1 1920×1200。
目标：回答"解码开始前输入是否已经坏了"，再决定改读取层还是解码层。
**状态：诊断包已出（镜像 `3943e45a…`），板端未刷。** 板子在 `reboot` 后挂在 U-Boot，需断电重上电。

## 本轮新增的观测能力

P313 交接时以为只能看过滤后的串口日志。实测 `dm_serial_log.py send` 的 **stdout 是未过滤原始串口**，
板端 NSH 完整可用（`ls/cp/cmp/dd/hexdump/free/ps/mount/kill/reboot`）。
注意：命令串必须用单引号传给工具，否则 `$`/`${}` 会被宿主 shell 先展开。
长文件名用 `set a <前32位>` + `set b <后32位>`，NSH 侧 `${a}${b}` 展开。

## 板端实测（P313 包 `e2e9a6b7`，uptime ~1690 s）

1. 失败率从"5 分钟 293 次"涨到 **每次解码都失败**：asset=0 frame=0 也失败，`resource_ok=0`。
   之前"每个动作第一帧成功"是**缓存命中**（5 槽里已有该帧），不是解码成功；不能当作部分可用。
2. `free`：总 62,029,164 B，已用 14,468,988，最大空闲 47,517,136 —— **堆没有耗尽**。
3. `/resource` = romfs on `/dev/res`（2048 B 块，22.3 MB）；`/data` = yaffs；`/tmp` tmpfs 只有 11×512 B。
4. 用 `hexdump skip=<off> count=16` 抽查 asset 80 的 0/10000/30000/59000 偏移，**与宿主文件逐字节一致**；
   `hexdump` 读 512 B/次，同一文件连读三次输出完全一致。
5. `cmp`（nshlib 内建，128 B/次 `read()`）会报 `files differ`，**甚至同一文件开两个 fd 自比也偶尔报差异**，
   而 `hexdump` 同时刻读到的字节是对的。`cmp` 打印的"byte N"是**本次块长**不是文件偏移。
   该现象尚未定性：可能是读路径对小读不稳定，也可能是 `cmd_cmp` 栈上 2×128 B 缓冲的栈问题。
   **不能据此判定 NAND/romfs 读坏**，也不能忽略。
6. `pkill luncher_dm` 后重开，进程在但**完全不打印、不刷屏**（等信号量 0% CPU）——app 不能原地重启，只能 `reboot`。
7. `reboot` 后串口停在 `[ND] not find panic data`，3 分钟无输出；同一次开机里 U-Boot 本来就有
   `nand wait_reg_status timeout` / `nand_first_reset_chip, reset failed!`。**需断电重上电**。

结论：文件内容抽样正确、堆充足，但**还没有证据说明解码输入在进入 libpng 时是否正确**，因此出诊断包。

## 唯一变量：输入获取方式 + 输入校验

`pet_assets.c`：把 `png_image_begin_read_from_file()`（stdio 流式）换成
`open/read` 整文件读入 **静态 BSS 缓冲**（98,304 B，最大素材 87,100 B）→ 算 CRC32 →
`png_image_begin_read_from_memory()` → 解码后再算一次 CRC。缓存/镜像/像素路径未改。

- 静态缓冲**不在堆上**：堆重叠不能再解释 CRC 不一致。
- `pet_png_crc.h`（`tools/gen_pet_png_crc.py` 生成）给 82 张素材的期望 size+CRC32，
  取值来自 `assets/pet`，且 `lichee/board/r528s3/ynm3000_nand/data/res/pet` 与其 `sha256` 全等（diff 空）。
- 日志：`[pet] PNG input mismatch asset=… size=…/… crc=… exp=…`、`PNG decode failed … after=…`、
  `PNG input changed during decode`、前 8 次成功打 `PNG input verified`。未吞错误、未关断言。

## 判定表（刷包后按日志读）

| 日志 | 结论 | 下一步 |
|---|---|---|
| `input mismatch`（crc≠exp） | 读到的字节就不是文件内容 | 复读同文件比较两次 CRC，定位读层/写入者 |
| crc=exp 且 `decode failed` | 输入正确，问题在 libpng/zlib 运行态或其堆 | 查解码库/内存写者，不再动读路径 |
| `input changed during decode` | 解码期间有人写这块内存 | 找写者（G2D/DMA/其它线程） |
| 只有 `PNG input verified`、无失败 | 原 stdio 流式读路径有问题 | 保留内存读，补对照实验后再定根因 |

## 验证与交付

- 宿主 `output/p314-pet-png-input/host/test_crc.c`：82/82 期望值与本机 zlib CRC32 一致（含 size）。
- 宿主 `test_decode.c`：82/82 内存解码与文件解码 BGRA 像素完全一致（ASan+UBSan）。
- `cc -Werror` 单独编译 `pet_assets.c` 通过。
- 官方流程增量构建 + `pack_deskmate.sh ynm3000` PASS（配置/ELF 入口/启动/121 资源/nsh/整包）。
- 镜像 `output/ynm3000/ynm3000.img`，51,482,624 B，MD5 `3943e45a47a7de095291c7ff2cf06735`；
  `vela.bin == nsh.fex`（`fecfc6b6a6df0801bf506284d1500c8f`）；`res.fex` 未变（`66ea36de…`）。
- 未提交/未推送/未打 tag；板端结论待刷包。

## 已知风险

- 新增 98 KB BSS 改变了内存布局，理论上可能掩盖布局相关的旧 bug；判定时以 CRC 三分支为准，不以"不报错"为准。
- 每次解码多两次 CRC（约 10 ms）与前 8 条成功日志，只影响性能不影响正确性。
- 板端 `reboot` 后挂死的原因未查；先断电重上电，若复现再单独记录，不与宠物问题合并。

## P315 — 板端结果（P314 包 `3943e45a`）与下一包

用户断电重上电并刷 P314 包后，04:18:24～04:22:42（4.3 分钟）实测：

| 项 | 数值 |
|---|---|
| `PNG input mismatch` | **124** |
| `PNG decode failed` | 124（同一批，不是两批） |
| `missing frame` | 124 |
| `input changed during decode`（after≠crc） | **0** |
| `input unverified`（3 次全失败） | 0 |
| 动作实例 | 113 |

- 失败率约 **29 次/分钟**（P313 为 59 次/分钟），宠物仍掉帧，用户确认"依然会有素材未就绪"。
- 每次失败都是 `size=78504/78504`（长度对）而 `crc≠exp`，`after==crc`：**字节在交给解码器之前就已经是错的，解码器无辜**。
- 同一文件紧接着再读一次就恢复（asset 0 首次失败、9 ms 后 verified；其余失败后 3 次尝试内必有一次通过）。
  即**读路径间歇性返回错误字节**，不是文件损坏、不是堆、不是解码库。

### 顺带否掉一条旧证据

板端 `cmp` **不可信**：拿 `dd bs=98304`（直读多扇区路径）与 `cp`（小读缓存路径）的两份 78202 B 文件，
`cmp` 报 `files differ: byte 128`，但 `hexdump skip=0 count=64` 与 `skip=64 count=64` 逐字节相同。
P313/P314 里所有基于 `cmp` 的"文件读坏"说法**全部作废**（`hexdump` 的结论仍有效）。

### 路径分析（读代码得出，待本包验证）

`fs_romfs.c` 有两条读路径：
- **直读**：`sectorndx==0 && nsectors>=rf_ncachesector` 时 `romfs_hwread()` 直接把多扇区读进用户缓冲；
- **缓存**：否则经 `romfs_filecacheread()` 单扇区入 `rf_buffer` 再 memcpy。

`CONFIG_FS_ROMFS_CACHE_FILE_NSECTORS=1`。libpng 的 stdio 每次要 8 KB（4 扇区）→ 走直读；
P314 的整文件读（38 扇区）也走直读 → 都失败。`hexdump` 512 B → 走缓存路径 → 从未出错。
按每次直读调用约 15% 出错估算：10 次/解码 ≈ 80% 失败（P313 接近全失败），1 次/解码 ≈ 15%（P314 的 29 次/分钟）——与两轮数据吻合。

### P315 改动（同一文件，只动"取输入"这一族）

1. `PET_PNG_READ_CHUNK=1024`：小于 2048 字节扇区，**永远走缓存单扇区路径**，绕开直读。
2. 校验失败时最多重读 3 次，每次单独打 `attempt=`；全失败才 `input unverified` 并丢帧（不吞错误）。
3. 失败时对**同一缓冲**再算一次 CRC 打 `again=`：`crc≠again` 说明有写者，`crc==again` 说明读返回的就是坏字节。

镜像 `output/ynm3000/ynm3000.img`，51,482,624 B，MD5 `e83bd6f54e548bd2e455b54af1f4e40d`，
`vela.bin==nsh.fex`（`d091159294dd1d126b7b0f0595cec74d`），`res.fex` 未变。板端待刷。


## P315 板测结果（04:44:16～04:47:56，3.7 分钟）——预测被否定

| 项 | P314（1 次大读） | P315（1 KB 分块） |
|---|---|---|
| `input mismatch` | 124 / 4.3 min ≈ 29/min | **331 / 3.7 min ≈ 90/min** |
| `input unverified`（4 次内全失败→丢帧） | 0 | **67（≈18/min）** |
| `missing frame` | 124 | 67 |

- **"直读路径坏、缓存路径好"被否定**：改成 1 KB（走缓存单扇区路径）后单次读的出错率反而大幅升高。
  结论改为：**损坏随 `read()` 调用次数增加，与 romfs 走哪条路径无关**。P314 一次读整文件反而是目前最好的读法。
- 重试有效：虽然单次读失败率升高，丢帧反而从 29/min 降到 18/min。
- 新观测：开机 5～6 s 内 LVGL 自己的解码器 `lv_libpng.c:302` 也报了 28 次 `IDAT: CRC error`（壁纸路径），
  说明**与宠物读法无关**，任何从 /resource 读 PNG 的读者都会中招。
- `again==crc`（P315 新增字段）在失败行里一致，未见"同一缓冲两次哈希不同"，暂不支持"有写者"假设。

## P316 改动与镜像

`PET_PNG_READ_CHUNK = sizeof(png_input)`（一次读完整个文件，回到 P314 的读法），
`PET_PNG_READ_TRIES = 4`（每次读后都比对 size+CRC，任何一次通过就用它；全失败才丢帧并记 `input unverified`）。
每次失败仍单独记录，不吞错误。按单次读约 15% 出错估算，4 次全失败约 0.05%，预计丢帧从 29/min 降到可忽略；
真实单次出错率由 `attempt=` 分布给出。

镜像 `output/ynm3000/ynm3000.img`，MD5 `a6b0c68f9b71270fabb5e10943e6b81c`，
`vela.bin==nsh.fex`（`6526658ff3425a58b859fdddc1baef76`），`res.fex` 未变。板端待刷。

**根因仍未定位**：重试是止血不是修复。已排除：文件内容（hexdump 与宿主逐字节一致）、解码器与堆（`after==crc`）、
romfs 直读路径（P315 否定）。待查：NFTL/块设备层多字节损坏、并发读者（`rm_buffer`/代理队列）、启动期更严重的原因。

