# DevLog 2026-09-04（part2）— 亮度显示问题交接（下会话 AI 处理）

> 本段 = P153~P157「锁屏亮度显示 + 暗光开灯提醒」功能的**完整排障史与交接文档**。
> 用户拍板：亮度读数问题本会话告一段落，**下会话 AI 按本文档续查**。
> 本段会话排障详情唯一源：本文档 + 9-4part1devlog.md 的 P153~P157 小节。

## 〇、TL;DR（下会话先看这段）

**症状**：锁屏健康卡「亮度」显示恒为 0（不是 "--"）；曾出现开机即弹暗光弹窗（P156 已加稳定期抑制，P157 加确认门）。

**当前最优假设**：数据链路全通，唯一未决点是 **LTR553 驱动读到的 CH0 原始值为什么是 0**（可能是 I2C/上电/遮光/寄存器时序，非应用逻辑）。

**下会话第一步（务必先做）**：上板烧 ok-20260904-27，抓串口日志 `grep LTR553`：
1. 有 `LTR553: ALS : x.xx lux (CH0=xxx, CH1=xxx)` → CH0 有值但 lux=0 → 查 lux 计算分支（ratio≥0.85 落入 lux=0）；
2. CH0 恒 0 → 传感器没出数：查 I2C 总线/i2c2 设备树/上电 GPIO/是否遮光（用手电照传感器看是否变化）；
3. **一条 LTR553 日志都没有** → activate 未触发/驱动未注册 → 查注册与激活链（见 §五-3）。

---

## 一、功能背景（P153，ok-20260904-22）

**需求**（用户原话转译）：①亮度传感器（LTR553 ALS）数值显示在锁屏健康卡「人体 有人 0M」右侧；②光线太暗时弹窗提醒开灯。

**实现**（本次新增，已固化）：
- 应用层新增 `dm_als.c/h`（`vendor/allwinnertech/apps/luncher_dm/`）：uORB `sensor_light` 订阅 + LVGL 1s 轮询 + 全局 lux 缓存 + 暗光状态机（50lux / 5s 防抖 / 10min 冷却 / 20s 开机稳定期 / **lux≥200 确认门**）。
- UI：锁屏健康卡第 5 项「亮度 N」label（standby_lux_lbl，文本去重防乱码）；暗光弹窗（太阳图标+「光线有点暗」标题+说明+「知道了」按钮，5s 自动消失，仅有人时弹）。
- 驱动：`ltr553.c`（sensor_register 框架，CONFIG_SENSORS_LTR553=y，I2C bus2，0x23，PART_ID=0x92）——**本次未改寄存器配置（经 datasheet 验证正确），只修了读取时序/有效位判断**。

## 二、排障史时间线（勿重复劳动）

| 节点 | tag | 动作与结论 |
|------|-----|-----------|
| P153b | ok-20260904-23 | 应用层审查修复 4 处（ALS 初始化独立于温湿度 / fd 泄漏 / 首帧顺序 / 注释） |
| P154 | ok-20260904-24 | 驱动初修：读失败 continue + 有效位检查——**有效位误用 bit1（错，那是 PS 中断位）**，被 P155 纠正 |
| P155 | ok-20260904-25 | **datasheet + Android 同芯片驱动交叉验证**：STATUS(0x8C) ALS 有效位 = **bit7(0x80) 低有效**；寄存器配置 ALS_CONTR=0x01 / MEAS_RATE=0x08 证实**正确非根因**；defconfig 开 SENSORS_ERROR/WARN |
| P156 | ok-20260904-26 | 用户反馈"亮度依然 0 + 开机弹'知道了'"→ **弹窗=链路全通铁证**（驱动确实在推 lux=0）；加 20s 开机稳定期；弹窗 UX 补图标/标题/说明；defconfig 补开 SENSORS_INFO（sninfo 可见 CH0/CH1）；minidisplay defconfig 对比确认配置非根因 |
| P157 | ok-20260904-27 | 用户"要有确切低亮度才弹"→ 暗光检测加 **lux≥200 确认门**（s_seen_bright），恒 0 坏数据永不弹 |
| **P158** | — | **本交接点**：用户反馈"亮度依然不显示"，转下会话 |

## 三、已确认事实（下会话直接采信，勿再排查）

1. **数据链路全通**：弹窗能弹 ⇒ 驱动在推 lux=0、应用 orb_copy 收到 ⇒ 订阅/activate/read/push/应用轮询全链路 OK，问题不在 dm_als 应用逻辑（除读数本身为 0）。
2. **寄存器配置正确**（LTR-553ALS-01 datasheet 官方示例核对）：ALS_CONTR=0x01（Gain bits[4:2]=000=1X，SW_reset bit1=0，ALS_mode bit0=1 active）；ALS_MEAS_RATE=0x08（积分时间<<3 与 Android `(integration_time<<3)|rate` 一致）。**勿再怀疑配置位域**。
3. **STATUS(0x8C) ALS 数据有效位 = bit7(0x80) 低有效**（0=有效可读，0x80=无效丢弃）。P155 后驱动流程：status bit7==0 → 读 CH0(0x8A)/CH1(0x88) → ch0>0 计算 lux 推送。
4. **I2C 传输层面无可见错误**（无 snerr），checkid(PART_ID 0x92@0x86) 通过才能注册成功。
5. **配置非根因**：nsh 与 nsh_minidisplay 的 LTR553/SHTC3/SGP30/I2C 配置完全相同。
6. **defconfig 已开**：CONFIG_DEBUG_SENSORS_ERROR/WARN/INFO=y → 驱动 sninfo/snerr **上板可见**。
7. 应用 UI 显示 0 ≠ "--" ⇒ orb_copy 成功读到 light.light==0.0（不是无数据）。

## 四、未决问题（仅剩一个主问题）

**LTR553 的 CH0 原始计数为什么是 0？** 候选（按概率）：
1. 传感器 ALS 通道未真正出数（转换未跑/寄存器写了但未生效/需更长上电时间）；
2. I2C 读 CH0/CH1 实际失败但 snerr 被吞（需 INFO 日志确认，理论上有 snerr 会打印）；
3. 硬件：传感器被遮光/未上电/焊接虚焊/在屏背面——**测试时用手电照传感器看数值变化**；
4. lux 计算分支误入 ratio≥0.85 → lux=0（但 ch0>0 时 sninfo 会打 CH0 值，可区分）。

## 五、下会话第一步动作（按序执行）

1. **烧录 ok-20260904-27 → 抓日志**：`grep LTR553`（SENSORS_INFO 已开，驱动每 500ms 打 `sninfo("LTR553: ALS : x.xx lux (CH0=%d, CH1=%d)")`）。
2. 按 §〇 三分支定位（CH0 有值 vs 恒 0 vs 无日志）。
3. 若无任何 LTR553 日志：确认 `ltr553_register`（r528_boot.c:814，i2c bus2）是否执行、checkid 是否过；订阅是否触发 activate（sensor.c:753 nsubscribers 0→1 自动 activate(true)）。
4. 备选手段：`i2ctool`（CONFIG_SYSTEM_I2CTOOL=y 已开）直接读寄存器 0x80/0x85/0x8C/0x8A 验证传感器响应与数据。
5. 参考：Android 同芯片驱动 `android_kernel_lenovo_msm8937/drivers/input/misc/ltr553.c`（bulk_read 0x88 起 4 字节：CH1 低高 + CH0 低高）；datasheet 示例：Enable ALS Gain X1 = ALS_CONTR 写 0x01。
6. 若 CH0 有值但 lux=0：改 lux 计算（对照 Android ltr553_calc_lux 与 gain/int_fac 表）。

## 六、代码位置速查

| 文件 | 角色 |
|------|------|
| `vendor/allwinnertech/apps/luncher_dm/dm_als.c/h` | 应用层订阅/状态机/确认门（P157 现状） |
| `vendor/allwinnertech/apps/luncher_dm/ui/ui_home.c` | 亮度 label（standby_lux_lbl ~1040/refresh ~1007）/ 暗光弹窗（dark_popup ~1350） |
| `vendor/allwinnertech/apps/luncher_dm/luncher_dm.c` | init_sensors：dm_als_init（P153b 已独立于温湿度） |
| `vendor/allwinnertech/chips/r528/drivers/rtos-hal/hal/source/sensor/als/ltr553.c` | 驱动（activate ~444 / thread ~313 / ALS 读取段 ~345 P155 状态） |
| `vendor/allwinnertech/chips/r528/r528_boot.c:814` | ltr553_register(0, i2c_bus2) |
| 对比参考 | `configs/nsh_minidisplay/defconfig`（配置相同非根因）；Linux `ltr501.c` / Android `ltr553.c` / LTR-553ALS-01 datasheet |

## 七、相关 tag 与恢复

- 固化：ok-20260904-22(P153) / -23(P153b) / -24(P154) / -25(P155) / -26(P156) / -27(P157)
- 恢复：`bash /data/vela/git_snapshot.sh -r ok-20260904-27`
- 归档版本：V0.0.6-20260904（md5 321506db，不含 P153~P157 亮度功能，如要归档新版本需重打）
