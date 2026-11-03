# HX83102B 触摸修复流程（P286-R4）

> **状态：已停止（2026-10-27）。** 本文保留为 R4 阶段执行规范与实验记录；用户决定 V2 触摸不再投入，停止依据见 [P287 收尾](../../devlogs/10-27-p287-v2-wrapup.md)。
> 平台：R528 V2 / NuttX-OpenVela，HX83102B @ TWI0 `0x48` 50 kHz，1440×720。
> 驱动：`vendor/allwinnertech/boards/r528/drivers/hx83102_iic_touch.c/.h`。
> 参考：Firefly/Himax `hxchipset`（`/home/<user>/Firefly_SDK/kernel-6.1/drivers/input/touchscreen/hxchipset/`）与 R1/R3 板测日志。
> 硬规则：一次一个变量；怀疑不得写成根因；失败只回滚本轮改动；刷机需用户确认。
> **2026-10-28 证据清理**：`output/p286-*` 目录已删除（V2 停止投入），下文日志/补丁路径仅作历史记录。

> **2026-10-27 R4s 更正/交接**：当前源码保留 R4a 门控并修复 STOP 假成功，增加有限观测；
> 16 项故障注入与 V2 整包校验通过，未刷。见 [R4s 记录](../../devlogs/10-25-p286-touch-driver-port.md)。
> 下文 E1.2 是待验证提案，**Firefly 只在 checksum 通过且 raw_out_sel 不符的特定分支补读 72 字节**，
> 不能泛化为所有校验失败/总线失败都补读。隐藏重试推进事件栈仍是假设；R4s 未加入清栈。
> 本轮使用已核验的新入口 `bash vendor/allwinnertech/tools/pack_deskmate.sh ynm3000_v2`，
> 内部执行本文 V2 build/pack 路径，并通过 check_deskmate 验证 181 资源和整包。

## 1. 目标与验收现象

目标：消除拖动中的事件帧错位、校验和拒收、非预期合成抬手与 RST 打断，使连续滑动与点按达到 R1 的可用水平，同时保持 R3 的空闲稳定。

板端验收现象：

1. 启动身份串 `P286-R4-...`，ID `00 2b 10 83`，`init ok`，LVGL 打开 `/dev/input0`。
2. 空闲 5 min：`link down` 0、`RST pulse` 0、`reject frame` 0。
3. 点按 20/20 命中（含短按）。
4. 日历连续滑动 3×30 s：合成 `UP` 0、RST 0、`reject frame` 0；帧间坐标无超过 P0 标定阈值的突跳；无“手指仍按着但 UI 已抬手”。
5. 跨应用点击 7 次无死区（R1 基线）。
6. 构建、打包、镜像哈希证据齐全；显示无回归。

## 2. 事实与未知

### 已证实（板测/源码）

- R3 已刷：启动身份 `P286-R3 idle-zero benign, R2c HAL, 50k`；ID `00 2b 10 83`；空闲 73 s 内 `link down`/RST/`reject frame` 均为 0。
- R3 日历拖动（`logs/serial/20261027-004751_p286-r3.log`，00:55:31–00:56:00，29 s）：11 次 `link down`（678–1229 ms）、4 次 RST、8 个 `stalled frame`、10 次 `retry failed`、9 次 ID 重读。
- P0 拖动基线（同一会话，02:13:09–02:13:46，用户确认全程触摸）：22 次 `link down`、7 次 RST、3 个 `reject frame`、9 个 `stalled frame`；HAL 61 次 `retry ok`、22 次 `retry failed`；驱动侧 `transfer=-16` 10 次、`-5` 7 次。
- 总线忙是硬证据：`[TWI0-E14] ... lcr=2a st=f9` 表示 SCL 高、SDA 被拉低；本段 16 次 `still stuck`，9 个时钟脉冲未能释放，随后只能 RST。`bus busy before xfer` 15 次。
- 驱动未做 Firefly 的 AHB burst 门控：Firefly 每次读 0x30 前写 `0x11=0x00`、读后写 `0x11=0x01`；本驱动常开。
- 拖动坏帧不是全 00：带真实坐标（如 `x=433,y=91`、`x=615,y=87`），夹 `02 cf ff ff` 空槽与残留字节，校验和失败；IC 在复位后仍可读 ID。
- 驱动当前行为：20 ms 跟踪轮询、100 ms 空闲门控；校验和失败后立即重读一次（`hx83102_iic_touch.c:690`）；200 ms 无好帧合成抬手（`:792`）；600 ms 无好帧进入恢复阶梯（`:734`）。
- HAL 行为（`hal_twi.c:2945`）：事务前等总线空闲 10 ms（`:2953`）；失败先恢复总线，再以同频率重发同一消息一次；`:2999` 记录 `retry ok/failed`。`drv_twi.c:126` 只把 `BUS_STUCK` 映射为 `-EBUSY`，其余失败映射为 `-EIO`。
- 代码评审（2026-10-27）：77.2/100，`NEEDS_FIX`，无 Critical；4 High + 2 Medium；设计健康度 6/10（B）。
- Firefly/Himax 正常读 touch_info_size（本模组 56）；checksum 通过后检查 raw_out_sel，不匹配且 diag_cmd=0 时补读 72 字节清栈。checksum 错误走错误/ESD 分类；复位条件不只 GET_DATA_FAIL，须结合 reset/ESD 分支核对。

### 未知（不得写成根因）

- H1（主候选）：HAL 的隐藏恢复/重发推进了 IC 事件栈指针；驱动看不到重试，下一帧从错误槽位开始。已知代码路径存在，**未经单变量实验确认**。
- H2（次候选）：轮询节奏与 IC 事件产生速率不匹配，读发生在帧更新中途。日志不足以区分“半帧”与“指针错位”。
- H3（新证据）：IC 在事件读期间或之后拉住 SDA；AHB burst 未门控、读被截断、IC 固件状态都可能，需 E1.0 区分。`lcr=2a` 只证明 SDA 低，不证明是谁拉住。
- 0x30 读失败/半帧是否消费 FIFO、消费多少，未知。
- 正常帧 `raw_out_sel` 高半字节的取值分布未知，不能硬编码 `0xF`。
- 复位后第一帧是否必然是新触点帧，未知。

## 3. 评审发现与阶段映射

| ID | 级别 | 问题 | 位置 | 阶段 |
|---|---|---|---|---|
| DR-F001 | High | 读失败/HAL 重试/E23 立即重读后无排空，事件栈失步 | `:675–698` | P1 |
| DR-F006 | Medium | 未校验 `raw_out_sel`、`count==0` 槽位与结构一致性 | `:357–418` | P1 |
| DR-F002 | High | 恢复与触点状态次序错；`now` 过期；复位后首帧变 MOVE | `:734–796` | P2 |
| DR-F004 | High | 全 00 语义：跟踪中当故障，可能误合成抬手/升级 RST | `:379–382` | P4 |
| DR-F003 | High | `work_queue` 重排队失败后轮询静默死亡 | `:812–828` | P5 |
| DR-F005 | Medium | register 未重置 `recover_tries`/`int_level`/计数等 | `:854–860` | P5 |
| W-02 | 警告 | 空闲 100 ms 门控可漏短按 | `:665` | P3 |
| W-10 | 警告 | LPWORK 单次事务可阻塞至 ~500 ms，恢复更久 | `hal_twi.c:2064` | P3 |
| W-01 | 警告 | unregister 与已打开 fd 的 UAF 契约未固化 | `:903` | P5 |
| W-06 | 警告 | 复位低脉冲 10 ms，Firefly 为 20 ms | 复位函数 | P2 |
| W-03/W-05/W-09/DP | 警告/设计 | `found!=count` 过严、`touch_register` 参数语义、初始化失败回滚、printf 过多 | 多处 | P1/P5 |

## 4. 阶段计划

总原则：P0 不改行为；P1 先解决“帧是否完整、事务是否失步”；P2 再解决“恢复后触点状态”；P3 调节奏；P4 改全 00 语义；P5 收尾健壮性。每阶段通过验收才进入下一阶段；任一实验使空闲出现 `link down` 或 RST，立即回滚。

### P0 基线固化与观测（不改代码）

- 恢复 `/dev/ttyUSB0` 采集，确认采集器进程与日志落盘。
- 在 R3 上采三组基线：启动+空闲 5 min、20 次点按、日历 3×30 s 拖动；另取 R1 最好拖动段作对照。
- 用同一解析口径统计：`link down`、`RST pulse`、`reject frame`、`retry ok/failed`、合成 `UP`、帧间坐标跳变最大值。
- 标定坐标突跳阈值（取 R1 正常段最大跳变的保守上界），写入 P1 验收。
- 已知观测缺陷记录在案：`stalled frame` 转储发生在立即重读之后（`W-07`），可能显示第二帧内容；本阶段不修，统计时标注。
- 产物：基线日志路径、计数表、阈值；不改驱动、不打包。

**通过条件**：三组日志齐全且计数可复现；阈值写入本文件附录。

### P1 读取事务与帧同步（DR-F001 / DR-F006 / W-03）

目标：每个被接受的帧都是完整、结构自洽的事件栈样本；读失败后不再从错误槽位继续。

- **E1.0 AHB burst 门控**（唯一变量）：按 Firefly，每次读 0x30 前写 `0x11=0x00`，读后写 `0x11=0x01`；ID/寄存器读不变。burst-off 写失败时本次不读 0x30，按传输错误走现有退避。
  - 预测：若 SDA 被拉低源于 AHB burst 与事件读冲突，`bus busy`/`still stuck`/RST 显著下降；若无关，指标不变，且多两次写可能略增错误。
  - 通过：3×30 s 拖动 `still stuck` 0、RST 0，`link down` 较 P0 下降 ≥50%；空闲 5 min 不回归。
  - 回滚：撤销门控 helper 调用，恢复 R3 基线。
  - 已构建打包（2026-10-27 02:38，未刷机）：V2 构建 `EXIT=0`，含 `CC hx83102_iic_touch.c`、`LD: nuttx`；`vela.bin` MD5 `7f4f1584…`，8,590,720 B，身份串 `P286-R4a burst-gate`。显式 `pack_img.sh -f/-g r528s3/ynm3000_v2_nand` 打包 `Dragon execute image.cfg SUCCESS !`；`nsh.fex` 与 `vela.bin` MD5 一致；镜像 `out/r528s3/ynm3000_v2_nand/rtos_nuttx_r528s3-ynm3000-v2_uart0_128Mnand.img`，53,346,304 B，MD5 `ebbcf3db…`。
- **E1.1 去掉失败后的立即重读**（唯一变量）：删除 `:690–698` 的第二次 56 字节读，失败后直接走退避/下一轮。
  - 预测：若立即重读加剧失步，`reject frame` 与紧随其后的 `link down` 下降；若无关，指标不变。
  - 通过：3×30 s 拖动 `reject frame` 为 0 或较基线下降 ≥50%，且无空闲回归。
- **E1.2 失败后补读 72 字节清栈**（唯一变量）：在任何失败/拒收后按 Firefly 语义补读 `128−56=72` 字节丢弃，再恢复正常 56 字节读。
  - 预测：撕裂帧后的连续拒收下降，拖动中断减少；若 IC 不支持该长度，会出现稳定错误码，立即回滚。
  - 通过：同上，且不出现新的 `-EIO` 模式。
- **E1.3 结构校验**（唯一变量）：先用 P0 日志确认正常帧 `raw_out_sel` 高半字节分布，再按 Firefly 的“与配置值一致”校验；同时校验 `count==0` 时槽位为 `0xFFFF`，`count>0` 时有效槽位数与计数一致。
  - 预测：把“校验和碰巧为 0 的错位帧”挡在解析前，避免假触点；若正常帧被误拒，说明判据过严，按日志放宽。
  - 通过：正常拖动无新增误拒；错位帧转为明确拒收而非假坐标。
- W-03（`found!=count`）只在 E1.3 日志证明误拒时调整，不单独先改。

**回滚**：只回滚本实验 diff；保留 R3 作为可用镜像。**通过后**才进入 P2。

### P2 恢复与触点状态一致性（DR-F002 / W-06）

目标：复位后不产生“无 DOWN 的 MOVE”、不把旧触点状态带过复位、不用过期时间戳。

- **E2.1 恢复后触点基线**（唯一变量）：在 RST 成功后清除跟踪状态（等价于一次明确的释放），使下一帧触点重新从 DOWN 开始；合成抬手使用恢复后的 `now`。
  - 预测：复位后的第一帧不再被当作 MOVE；UI 不会出现“已复位但手指仍按下”。
  - 通过：每个 RST 后事件序列为 UP（如之前有触点）→ DOWN，无 MOVE 先于 DOWN；无卡按。
- **E2.2 复位脉冲时序**（唯一变量）：低脉冲 10 ms 改为 20 ms、释放后等待 50 ms 再探测，与 Firefly 对齐。
  - 预测：`detect` 首次成功率上升，连续 RST 次数下降；若变差立即回滚。
  - 通过：拖动中 RST 后 ID 读取一次成功，无重复复位。
- W-09（初始化失败回滚）在 E2.1/2.2 后单独检查，不夹带。

**通过后**才进入 P3；若 P1/P2 已使拖动达标，P3/P4 可降为回归项。

### P3 节奏与阻塞（W-02 / W-10）

目标：在不引入空闲故障的前提下改善短按命中与轮询阻塞。

- **E3.1 空闲节奏**（唯一变量）：`HX83102_IDLE_MS` 100→20（或经 P0 证据选 50），保持跟踪 20 ms。
  - 预测：短按命中上升；空闲流量上升，若触发 `link down`/RST 则回滚。
  - 通过：20/20 点按、空闲 5 min 零故障、拖动不回归。
- **E3.2 工作队列**（唯一变量）：仅在 P0/P3 日志证明 LPWORK 阻塞造成 UI 卡顿时，把轮询移出 LPWORK 或缩短单次事务等待；否则不改。
  - 通过：UI 卡顿消失且触摸指标不回归。

### P4 全 00 语义与释放策略（DR-F004）

目标：区分“事件栈为空”与“手指抬起”，避免拖动中被 200 ms 合成 UP 打断。

- **E4.1 全 00 分类**（唯一变量）：跟踪触点时，全 00 不再当作坏帧计数，而是“本轮无新事件”；最后一个有效帧时间不前进；释放只认合法 `count=0` 帧，死链仍由 600 ms liveness 兜底。
  - 预测：拖动中不再出现由全 00 触发的合成 UP；真实抬手仍有合法 `count=0` 帧；真死链仍在 600 ms 内释放并升级。
  - 通过：3×30 s 拖动合成 UP 为 0；抬手测试无卡按；拔线/停 IC 测试 600 ms 内释放。
- 若 P0/P1 日志证明拖动坏帧均为非零撕裂帧，本阶段不阻塞 P1 结论，只作正确性修复。

### P5 健壮性与清理（DR-F003 / DR-F005 / W-01 / DP）

- **E5.1 重排队失败**：`work_queue` 失败时不再静默停摆；至少清除 `running` 并记录，能重试则用一次延迟重试。
- **E5.2 register 全量状态**：重置 `recover_tries`、`g_stall_dumps`、`int_level`、`i2c_fail_count`；核对 `touch_register` 第三参数（当前是队列深度，不是 maxpoint）。
- **E5.3 unregister 契约**：固化“先关闭 fd 再 unregister”的调用约束；无树内调用者时只写契约，不引入无依据的 UAF 补丁。
- **E5.4 日志与死代码**：高频 printf 改为可关闭级别；删除死宏/影子状态/只写计数器。最后做，不与行为实验混提交。
- 每项独立变量、独立验证；DP 类改动不参与触摸行为验收。

## 5. 实验记录模板

每个 E 编号按 `project_docs/methodology.md` 记录，并额外填写：

```text
E编号 / 日期 / 平台：
O：现象与原始日志位置
基线：R3 镜像哈希 + 当前 diff + 配置
H：待验证解释（不是根因）
唯一变量：函数/常量/事务；其他条件如何保持一致
Firefly 对照：参考实现的做法与差异
预测：H 成立/不成立分别观察到什么
结果：构建/打包/刷机证据；计数表；未运行写未运行
C：支持/削弱/无效；只能说明什么、仍未知什么
补丁去留：保留/撤销/待审
下一步：仅一个，含成功与失败分支
```

## 6. 证据与命令

- 构建：`./build.sh vendor/allwinnertech/boards/r528/ynm3000_v2/configs/deskmate -j$(nproc)`，要求 `EXIT=0` 且出现 `CC hx83102_iic_touch.c`。
- 打包：**必须显式** `cd vendor/allwinnertech/lichee && bash tools/scripts/pack_img.sh -c sun8iw20p1 -p rtos -b r528s3-ynm3000-v2 -o nuttx -d uart0 -s none -m normal -w none -v none -i none -t $PWD -f r528s3/ynm3000_v2_nand -g r528s3/ynm3000_v2_nand`，要求 `Dragon execute image.cfg SUCCESS !`；`/data/vela/pack.sh` 硬编码 gemini-s1，对 V2 会产出错误镜像并切换构建树，禁用。`check_res`/post-dragon 不覆盖 V2，需手工核对 `nsh.fex == nuttx/vela.bin`。
- 串口：`/dev/ttyUSB0` @1.5M；`contest2026_070_arc/tools/dm_serial_log.py start/mark/read/summary`；日志落 `/data/dm/logs/serial/`。
- 每次实验记录镜像 SHA-256、MD5、大小、启动身份串；刷机前先恢复采集。
- 硬件验证单列；编译通过不等于触摸通过。

## 7. 停止与升级规则

- 连续 2 次实验没有缩小范围：停止叠加补丁，回查基线、观测方法与构建身份；有新证据才换方向。
- 任一实验造成空闲 `link down`/RST 或点按回归：立即回滚本实验，保留 R3 回退镜像。
- RST 频率 >5 次/min 或出现卡按：停止后续实验，先恢复上一可用镜像。
- 只有“单变量修复 + 回退复现”同时成立，才可把 H1/H2 升级为根因；否则文档写“候选/未知”。
- 不 commit/push/tag；不删除用户未提交改动；不刷机除非用户确认。

## 8. 当前停止点

- **已停止（2026-10-27）**。用户决定不再投入 V2 触摸；本文后续实验（E1.0～E5.x）不再执行。
- 停止原因：R5 实验（唯一变量 `hx83102_pins_config()` 的 INT pull：`GPIO_PULL_DOWN_DISABLED` → `GPIO_PULL_UP`）刷机后**花屏**，比 R4s 更差，已回退。模组 INT 在 1.8 V 域且无电平转换，R528 PB5 属 3.3 V 域、VIH≈2.31 V；开 3.3 V 上拉会扰动面板，与 E2（RST 推挽 3.3 V 出横纹）同型。**INT 的软件解法关闭**。
- 历史证据：E2（上拉 ON）是唯一出现 `INT 1 -> 0` / `INT 0 -> 1` 的版本；E5 关掉上拉后所有版本 INT 零跳变（含 R3 的 2 h 17 min 日志）。
- 停止时状态：R3 空闲 73 s 零故障；日历拖动 29 s 11 次 `link down`、4 次 RST、8 个撕裂帧。R4s 未刷，R5 已证伪回退。
- 镜像：R3 `output/p286-r3/P286-R3-idlezero.img`、R4a `output/p286-r4a/P286-R4a-burstgate.img` 均可回退；R5 `output/p286-r5-intpull/P286-R5-intpull.img` 保留为证据，**不可用**。
- 若将来重启：只剩硬件路径（电平转换，或 3.3 V 上拉串限流电阻），软件侧不要再在 PB5 叠加 3.3 V 上拉。
- 关联记录：[P287 收尾](../../devlogs/10-27-p287-v2-wrapup.md)、[P286 详情](../../devlogs/10-25-p286-touch-driver-port.md)、[排障模板](../../methodology.md)。
