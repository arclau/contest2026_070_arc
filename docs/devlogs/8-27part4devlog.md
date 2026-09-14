# DevLog 2026-08-27 — P125：锁屏人体距离显示 + 在座距离阈值定稿（1.5m 配置化）

> 承接 P124（UART 分工定稿，ok-20260827-25）。本节点 = ①锁屏显示 LD2410B 探测距离
> ②距离判断条件定稿：阈值 1.5m（2 门）改配置化 + 短暂离开 30s 只暂停不重置。

## 背景

P124 已把 LD2410B 迁到 /dev/uart3（恢复 init_sensors），待上板复测人体数据。
用户追加需求：①锁屏要显示距离（不只"有人/无人"）；②距离判断条件按实际代码实现拍板
（"距离感应器 1 米内都算坐下之类的问题"）。

## 关键认知：距离门吸附（200cm 阈值实际=150cm）

LD2410B 探测距离**按 0.75m 一个距离门**上报，只能是 0/75/150/225/300...cm 整数倍：
- 旧代码 `LD2410B_SEAT_CM_DEFAULT 200`（注释"≤2m 在座"）是**虚的**——
  200 落在 2 门（150cm）和 3 门（225cm）之间，实际只有 ≤150cm 能通过
- **真实生效边界 = 1.5m（2 门）**，注释与实现不符

## 发现：prox_seat_cm=30 是 LTR553 时代死配置

`dm_prox_get_cm()`（luncher_dm.c）已把距离压成三态：`0`=有人且在座距离内 / `99`=无人或超阈值 / `-1`=传感器不可用。
而 `dm_health.c` 仍用 `prox <= CFG_I("prox_seat_cm", PROX_SEAT_CM=30)` 判断——
`0<=30` 恒真、`99<=30` 恒假，**该配置项完全失效**（真正阈值是 luncher_dm.c 写死的 200，
且不读配置文件，`ld2410_seat_cm` 覆盖注释是空话）。

## 方案（用户三问拍板，2026-08-27）

| 问题 | 拍板 |
|------|------|
| 在座距离阈值 | **1.5m（2 门）**——人在桌前正常距离 |
| 阈值管理 | **改走 dm_health_cfg 配置化**（新增 ld2410_seat_cm=150，顺带删 prox_seat_cm=30 死配置） |
| 短暂离开处理 | **只暂停不重置**：离开立即暂停倒计时，防抖 5s→30s 才清零（倒杯水回来接着计） |

## 改动清单（全部在 `vendor/allwinnertech/apps/luncher_dm/`）

| 文件 | 改动 |
|------|------|
| `ui/ui_home.c` | 锁屏 `standby_prox_lbl` 刷新：有人 → `人体：有人 x.xm`（探测距离/100，0.1m 精度）；无人/不可用文案不变；注释去"临时 debug"（review-15 已定稿） |
| `dm_ld2410b.h` | 头注释 UART0/PB22/23 → UART3/PD10/11 + init 已恢复调用；`LD2410B_SEAT_CM_DEFAULT` 200→150 + 注释直写生效边界 |
| `dm_health_cfg.c` | 模板：删 `prox_seat_cm=30`；新增 `ld2410_seat_cm=150`；`prox_absent_debounce_s` 5→30 |
| `luncher_dm.c` | `dm_prox_get_cm()`：写死 200 → `dm_health_cfg_get_int("ld2410_seat_cm", LD2410B_SEAT_CM_DEFAULT)`（加 include dm_health_cfg.h） |
| `dm_health.c` | `prox <= prox_seat_cm(30)` 死判断 → `prox == 0`（三态语义直写，注释说明）；`PROX_ABSENT_DEBOUNCE_S` 5→30 |

## 验证

1. 编译：`./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh -j$(nproc)` ✅（仅既有 unused 警告）
2. 打包：`pack` ✅；产物一致 `nsh.fex == vela.bin`，md5 = **f13cc1d2…**（8131280B）
3. 资源检查：`bash /data/vela/check_res.sh` ✅（wifi 固件/字体/12 提示音路径+字节全对）
4. 固化：**ok-20260827-26**（锁屏距离显示 + 注释同步）+ **ok-20260827-27**（阈值配置化 + 防抖 30s）

## 遗留（下会话）

1. 🔴 **上板复测 ok-20260827-27**：①LD2410B 接 UART3（PD11=RX）→ 锁屏应显示
   `人体：有人 x.xm`；②有人坐下 → 5s 防抖后播欢迎语并开始计时；③离开 30s 内回来 → 倒计时恢复不归零；
   >30s → 会话清零；④`uartdbg_test loop 115200`（UART1 内部回环）+ 调试工具接 BSN20 外设侧收 LOG
2. 板端 `/data/dm_health.cfg` 是旧文件：模板变更（删 prox_seat_cm/加 ld2410_seat_cm）只在
   **模板缺失时写入**——若板端已有旧 cfg 文件，需手动改或删文件让其重写（核实 cfg_load 行为）
3. defconfig `GEMINI_XTS=y` 疑似误配（仅 drv_gpio.c 用）——暂保留

## 沉淀教训（P125 新增）

- **LD2410B 距离是门控整数倍**：0.75m/门，阈值必须对齐门档（1 门=0.75m/2 门=1.5m/3 门=2.25m），
  中间值会被"吸"到下档——写阈值先查门档，注释直写生效边界
- **三态接口后接阈值判断 = 死配置温床**：dm_prox_get_cm() 把距离压成 0/99/-1 后，
  dm_health 再用 `<=30` 判断已无意义——接口语义变了，调用方判断必须同步简化（prox==0），
  否则留下"看着能改其实没效"的配置项
- **配置化要一次到位**：宏注释声称"可经 ld2410_seat_cm 覆盖"但代码写死 200 不读配置——
  注释与实现不符比没有注释更危险，配置化改动要同时更新读方与写方

*DevLog by AtomCode (deepseek-v4-flash)*
