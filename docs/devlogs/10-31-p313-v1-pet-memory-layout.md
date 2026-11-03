# P313 — V1 宠物解码排障：修正显示缓冲与堆的内存布局

2026-10-31；目标平台 R528 `ynm3000`，V1 1920×1200。
目标：消除确定的显示越界/堆重叠，再验证宠物及壁纸 PNG 错误是否消失。
**最终交接：板测 FAIL，宠物 PNG 故障仍在。内存布局修正已生效，但不能据此宣称宠物修复。用户要求停止排障并交接。**

## 给接手者先读这段

**当前任务是让 V1 猫能持续显示和播放动作，先解决 PNG 解码失败。现在不是补养成规则，也不是改 V2。**

1. P312 留下的问题是宠物 PNG 解码随机失败；日志有 `IDAT: CRC error`、`bad adaptive filter value`。
   宠物渲染器遇到失败会隐藏图像并报 `missing frame`，所以用户会看到猫消失或动作缺帧。
   目前没有证据证明是美术 PNG 源文件本身坏了，也没有证实 NAND、G2D 或 libpng 中哪一层是根因。
2. 本轮查到一个真实的底层缺陷：V1 三张显示缓冲需要约 26.4 MiB，原来只留 12 MiB，
   旋转目标越过预留区乃至物理 DRAM 上界，堆也覆盖显示区。已经改为 28 MiB，并把堆止于 `0x45e00000`。
   **这是确定需要保留的边界修正，不是宠物故障已经修好的证明。**
3. 用户已经刷了本轮包（MD5 `e2e9a6b716f05c848fd9dc1ce00dbeb1`），串口证明新布局已生效。
   但启动约 55 秒后仍出现宠物解码错误；固定五分钟窗口有 **293 次解码失败和对应的 293 条缺帧日志**。
   这是同一批失败的两层日志，不能相加成 586 次独立故障。窗口内壁纸解码错误为 0。
4. 开头几次动作 `resource_ok=1` 不能当作完整动作通过：`pet_renderer_step()` 只在动作切换时打印它，
   后续帧仍可能失败。宿主 9 组测试通过也不能当作板端通过，因为其链接的是宿主系统 libpng。
5. 后来尝试从串口读回 PNG：最初因 NSH 变量拼接错误读到了目录，那些哈希差异无效；
   改正后只确认目标 PNG 的前 128 字节一致。两次整文件采集都不完整，不能据此宣布文件读坏或 NAND 损坏。
6. **停在这里：`pet_assets.c` 没有新增修复，仍走 `png_image_begin_read_from_file()`；没有第二个诊断包。**
   下一步建议只做“完整压缩输入快照 + 解码前后校验”这个实验，先回答输入何时变坏，再决定改哪层。

接手时先读本页后面的串口入口与证据路径。守护器仍运行，硬件在宿主上；沙箱内看不到设备不代表没插 USB。
不要按 P312 的建议去关闭 `dm_perf`：它只负责统计，不会关闭 G2D。不要同时改几个模块后再让用户盲刷。

## 起点与范围

用户要求读取 AGENTS 与最新 devlog 修宠物；最新阻塞为 [P312](10-31-p312-pet-asset-corruption.md)。
[P307](10-31-p307-v1-pet-acceptance-audit.md) 的养成合同/计时/提醒/音效仍待闭环，本轮不叠加功能变更。
源码实际位于 `vendor/allwinnertech/apps/luncher_dm`，不是根 `apps/luncher_dm`。
厂商仓 HEAD `4c122d896b8ca786ad3f9a6768f37eeecdfe71e8`，开始已有 122 项差异；不回滚、不提交。
基线 `.config`、ELF、厂商仓 tracked diff 和三个修改文件原件见 `output/p313-pet-memory/before/`。

## E1：固定显示内存布局的边界实验

- O（前人报告）：宠物/壁纸 PNG 随机 CRC/解码错误；WiFi 下载错误尚未独立复测。
- H：固定 framebuffer/旋转缓冲越界，同时堆覆盖显示内存，可能破坏解码输入或状态。
- 唯一变量：V1 显示预留区及与其配套的堆上界；不改 G2D 算法、解码器、PNG 或互动逻辑。
- 预测：旧布局边界检查失败；新布局三个缓冲均在物理 DRAM 与显示预留区内，且不与堆交叠。
- 实验：宿主编译真实 `r528_memorymap.h`，读取 BOE 属性表尺寸与 `FB_NUM`，检查全部缓冲区间。
- 结果：旧布局失败，新 defconfig 和构建后的有效 `.config` 均通过；旧 `.config` 被新增编译期检查拒绝。
- C：证实并修复布局缺陷；板端布局生效后 PNG 故障仍复现，尚无实际损坏写入现场，根因未知。
- 补丁保留；用户已刷包并判 FAIL，后续只读文件观测的有效性限制及下一实验见本文交接段。

当前驱动 `FB_NUM=1`，`fb_g2d_rot_create()` 在源帧后放置两张旋转目标帧。
BOE 属性表给出 1200×1920、270° 旋转，`up_fbinitialize()` 得到横屏 1920×1200、32bpp。
一帧 9,216,000 B，三帧共 27,648,000 B（约 26.4 MiB）。

| 区间（半开） | 旧布局 | 新布局 |
|---|---|---|
| 堆上界 | `0x48000000` | `0x45e00000` |
| 显示预留 | `0x46e00000..0x47a00000`（12 MiB） | `0x45e00000..0x47a00000`（28 MiB） |
| 源 framebuffer | `0x46e00000..0x476ca000` | `0x45e00000..0x466ca000` |
| 旋转目标 0 | `0x476ca000..0x47f94000` | `0x466ca000..0x46f94000` |
| 旋转目标 1 | `0x47f94000..0x4885e000` | `0x46f94000..0x4785e000` |
| DSP 预留 | `0x47a00000..0x47f00000` | 同前 |
| 日志预留 | `0x47f00000..0x48000000` | 同前 |

P311 的 `CONFIG_RAM_SIZE=113246208` 恰好使 RAM_END 到物理内存末端，但仍包含固定显示区。
本轮设为 77,594,624 B，即从 `0x41400000` 到 `0x45e00000`；可用堆会相应缩小。

## 修改与验证

- `chips/r528/hardware/r528_memorymap.h`：仅 `CONFIG_ARCH_BOARD_YNM3000` 使用 28 MiB；其他板保留原值。
- `boards/r528/ynm3000/configs/deskmate/defconfig`：修正 RAM_SIZE；NAME_MAX=80 保留。
- `disp2/disp/dev_fb.c`：清 framebuffer、启动 G2D 前检查容量；不足返回 `-ENOMEM`；输出一次布局身份日志。
- 新增 `contest2026_070_arc/tools/tests/fb_memory/run.py`；原宠物程序和资源未改。
- 有效配置与新 ELF `up_allocate_heap` 反汇编确认堆止于 `0x45e00000`，见 `heap-disassembly.txt`。
- 现有宠物 ASan+UBSan 9 组 PASS，含 82 张 PNG、镜像像素 CRC、缓存上限及 pinning。
- 沙箱内 LeakSanitizer 受 ptrace 限制，改用获批的沙箱外运行；没有关闭检查器。
- 完整 `dev_fb.c` nxstyle 有历史告警；本轮新增 22 行范围检查通过，`git diff --check` 通过。
- 六维增量审查：初始化/容量边界 PASS；PM/ISR 无变更；新增 API/局部变量/格式检查 PASS。
- 官方 `pack_deskmate.sh ynm3000` 内部构建成功；沙箱内 dragon 被 Bad system call 终止，整包检查拒绝旧包。
- 获批后沙箱外重跑成功：配置、ELF 入口、启动脚本、121 项资源、nsh、完整镜像全部 PASS。
- 证据统一在 `output/p313-pet-memory/`；未启动模拟器、未截图。03:31 UTC 用户刷入本候选包。

交付镜像 `output/ynm3000/ynm3000.img`，51,482,624 B：

- MD5 `e2e9a6b716f05c848fd9dc1ce00dbeb1`
- SHA256 `4d497337a86a8a1e97939fae056f65ca7d3ac10e94367dfb2c40f656f399e06f`
- `vela.bin == nsh.fex`，MD5 `fa0285b98de45c30f132856a14928543`
- 有效配置、ELF、资源及本轮源码哈希见 `delivery.json`；官方清单为 `output/ynm3000/manifest.json`。

## P312 交接结论的修正

`dm_perf` 仅统计，不控制 G2D；关闭它不能验证 G2D 假设。
旧宿主测试链接系统 libpng，只证明该宿主路径可解码，不能排除目标 libpng/zlib/读路径。
资源字节数相等也不证明内容相等；WiFi 下载错误本身不能证明发生 zlib 解压错误。
本轮没有足够证据把三条报错合并为同一根因。

## 板测失败与接手入口（2026-10-31 03:43 UTC）

**用户要求停止工作、写交接；到此停止排障，不继续改代码或出包。**

### 已复现的事实

- 用户 03:31 UTC 刷机；03:32:11 串口出现新 `[fb] memory` 日志，heap_end=`0x45e00000`。
- 该行被多核日志交织截断；随后 LVGL PlaneInfo 明确显示 framebuffer mem=`0x45e00000`、fblen=9216000、stride=7680。
- 03:32:19 宠物开始动作，资源 revision=`b8239a6ce265bbbd`；最初多个动作 `resource_ok=1`。
- 首次错误 03:33:04，应用 tick 55.373s：asset=68，`IDAT: CRC error`；随后持续增加。
- 固定五分钟窗口 **03:32:19.591～03:37:19.591 UTC**：119 条动作记录、293 次 PNG 解码失败、293 次缺帧。
- 同窗口壁纸 `decode_png` 错误 **0**；这不证明壁纸长期正常，也不能把 WiFi 与宠物合并为同一根因。
- 结论：本包宠物验收 **FAIL**；布局修正不足以消除故障，不能排除整个内存/G2D/文件系统/解码器模块。
- 原始统计与身份行：`output/p313-pet-memory/board-e1-result.json`。
- 对应包/ELF/config/manifest/vela.bin/res.fex 已另存 **`output/p313-pet-memory/e1-delivery/`**；后续出包勿覆盖此证据。
- 布局补丁保留：静态越界确实存在；保留不等于已证实它是 PNG 故障根因。

### 服务器串口监控仍运行，直接接手

上轮“服务器无 USB 串口”的判断错误：沙箱隐藏宿主 `/dev`；**必须在宿主/沙箱外访问硬件**。
已确认 CH340（USB ID `1a86:7523`）对应 `/dev/ttyUSB0`，波特率 **1500000**。
交接时 `connected=true`，RX=1186959 B、TX=1118 B，2419 条已保留记录；没有停止守护器。

```bash
# 以下控制命令在宿主执行（Codex exec_command 需 require_escalated）
python3 contest2026_070_arc/tools/dm_serial_log.py status --json
python3 contest2026_070_arc/tools/dm_serial_log.py mark "next experiment"
# 读已落盘文件可在沙箱执行
python3 contest2026_070_arc/tools/dm_serial_log.py read \
  --session 20261031-032521_p313-pet-board.log \
  --grep 'PNG decode failed|missing frame|resource_ok=0|memory:' --limit 30
```

- 日志：`logs/serial/20261031-032521_p313-pet-board.log`；对应 `.meta.json` 保存流量信息。
- 当前入口：`output/p313-pet-memory/serial_monitor.py`，复用原 `dm_serial_log.py` 的守护/重连/控制。
- 适配器在写盘前仅保留宠物/壁纸解码、布局/启动/崩溃/WiFi 固件诊断，并过滤凭据类行和网络地址。
  **这不是全量串口日志**；没有命中不能证明过滤范围之外的故障不存在。
- 若守护器不在，可在宿主运行：
  `python3 output/p313-pet-memory/serial_monitor.py start --port /dev/ttyUSB0 --baud 1500000 --tag p313-pet-board`
- `boots` 不代表真实重启次数：原 BOOT 正则会误命中 `lv_nuttx_*` 日志；需按具体启动时间线核对。

### E2 文件读取观测：有效性限制，勿误用

只做过 NSH 只读 hexdump 与 shell 临时变量设置；未改板端资源文件。
脚本 `output/p313-pet-memory/serial_probe.py`，报告 `board-file-probes.jsonl`。
报错 asset=68 对应文件：
`c936072900cc1e58caf81996dc6c879f59b473dc50bb320e51bc80ec8236b01e.png`，78202 B。

1. 最初使用 `$p313a$p313b` 拼接，NSH 将它当作一个变量名，结果读到了 `/resource/pet/` 目录。
   **这些哈希差异全部无效，不是资源损坏证据**；改成 `${p313a}${p313b}` 后先 echo 验证文件名。
2. NSH LINELEN=64，故长文件名分两段 set；`nsh_dumpbuffer` 的 32 字节标题又截掉文件偏移，解析器已适配。
3. 有效的小样本：asset68 前 128 B 完整读回、PNG 签名正确，与宿主 SHA256 一致：
   `640b2d846f3720561c64d797de3028076eb26b49a8aa2752f804855d8efa70bd`。
   **只证明这次文件头读回一致，不证明 IDAT 或整个 NAND 路径正常。**
4. 整文件采集两次分别只拼出 13874 / 56288 B，均不完整；后一次有 3 个字节差异，
   但串口交织/采集截止/解析不完整尚未排除，**不能将这些差异升级为板端文件读坏**。
5. 连续观测方法失败后已停止大文件 hexdump；不要把 `matches=false` 当成有效根因结论。

### 下一步只有一个（尚未实施）

在宠物读图入口加入**有界完整输入快照校验**，校验 PNG 输入，再对比解码前后输入是否变化，
用一个实验区分“解码开始前输入已坏”与“解码期间被改写/目标库问题”。
前者也不能直接归咎于 NAND，因为读完到校验前仍可能发生内存破坏。
可考虑 `png_image_begin_read_from_memory()` 配合输入校验；须保留错误日志和内存边界，不能吞错误。
若解码前校验失败，调查读取及此前内存写入；若前后内容发生变化，追查写入者；
若前后校验都正常而解码仍失败，再核目标解码库/运行环境。仅换内存读法后不再报错，也需对照验证，不能直接签根因。
**这只是计划：没有改 `pet_assets.c`，没有生成该诊断包，也没有完成对应测试。**
当前源码仍使用 `png_image_begin_read_from_file()`；禁止接手时误认为已切内存解码或可直接刷第二包。

开始下一实验前，重新读完整相关函数/清理路径，保存差异，宿主 ASan+UBSan 回归，再官方板包与串口验真。
至多保留 3 个假设，一次一个变量；不要同时关 G2D、改 NAND、改 libpng。

## 收尾状态

- CODE：编译/打包/内存边界/宠物宿主回归 PASS；**板端宠物 FAIL**，根因未知。未提交/推送/tag。
- 源码镜像：执行 `sync_r528.sh --check`，现有 app 镜像存在大范围差异；XML 也未覆盖本轮 memorymap/主板配置。
  本轮不扩大为镜像/manifest 整改，不用全量导出覆盖已有差异；源码镜像交付仍有缺口，真源及补丁原件在工作区。
- DOCS：详情/索引/AGENTS 更新并导出，镜像一致；doc_check 前后均 19 项既有告警，finalize DOCS=WARN。
- LOGS：本次 Codex session `01a18fdc-f067-75c1-abbe-f7845226db06` 来源 cwd=/data/dm、事件非零。
  当前共享转换器存在 response_item 适配缺口，本轮不导出其他工具/会话，不手写替代原始日志；finalize LOGS=MISSING。
  现有入仓日志凭据扫描命中文件 0；不 commit/push/tag。
