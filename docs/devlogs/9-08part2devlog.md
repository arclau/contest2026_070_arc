# 9-08 part2 — P198 开机随机卡 LOGO 修复（NuttX 显示链路 DSI 死等）

> 现状：新精灵验证前的阻路 Bug。2026-09-08 用户报告：**自分区表调整（bootloader 24→50MB）后，开机有概率卡 LOGO 进不了桌面，DEBUG 看不出异常，重启一次又正常**。

## 现象与定位边界
- 用户确认：**卡在 NuttX 阶段**（u-boot 已正常跑完），串口无明显错误日志。
- 排除：u-boot 整读 50MB 分区（固件仅 33.34MB，读 16.7MB 陈旧 NAND 区）→ 是隐患不是本次根因（uboot 确实跑完了）。
- 结论必须先锁定「挂死在哪层」：整合「u-boot 已完成 + NuttX banner 后无 luncher_dm 日志 + 随机 + 重启自愈」→ 指向**显示/面板初始化某处无超时同步等待**，时序偶发竞争。

## 根因（代码铁证）
- R528 编译的是 DSI version-40 驱动：`disp2/disp/de/Makefile`（`CONFIG_ARCH_SUN8IW20`）→ `lowlevel_v2x` → `de_dsi.c`。
- 启动链路：`r528_boot.c r528_disp_init` → `fb_register` → `disp_probe` → `start_work` → `bsp_disp_device_switch` → `disp_lcd_sw_enable` → BOE `lcd_open_flow` → `lcd_panel_init` → **~30+ 次 `sunxi_lcd_dsi_gen_write_*`** → `dsi_gen_wr()`。
- `de_dsi.c dsi_gen_wr()`（原 419-421 行）：
  ```c
  while (dsi_inst_busy(sel))    /* 无超时、无调度、无 delay → inst_st 不清就永久 spin */
      ;
  ```
- 对照同文件 `dsi_dcs_wr()`（266-277 行）：**带 `count<50 × dsi_delay_us(100)`（≤5ms）+ 超时强清 `inst_st=0`**。不对称 = smoking gun。
- 触发机理：`lcd_power_on` 只开 DSI clk，LPTX 起使能在 `dsi_start(DSI_START_LPTX)`（每条命令写完后）；某次开机 PHY/时钟建立时序迟滞 → `inst_st` 不回落 → 第一条 gen 写（0xB0,0x05）就死锁 → 卡 LOGO。重启重竞时序 → 概率自愈。

## 修复（P198，snapshot ok-20260908-2）
`dsi_gen_wr()` 完整镜像 `dsi_dcs_wr()` 有界模式：
```c
u32 count = 0;
while ((dsi_dev[sel]->dsi_basic_ctl0.bits.inst_st == 1) && (count < 50)) {
    count++;
    dsi_delay_us(100);
}
if (count >= 50) {
    dsi_dev[sel]->dsi_basic_ctl0.bits.inst_st = 0;
    DISP_PRINT("dsi_gen_wr inst busy timeout, sel=%d\n", sel);
}
```
- **关键坑**：NuttX 目标下 `hal_log_warn`→`printk` 是**空宏**，`DE_WRN(...)` 及其字符串会在编译期被整吞（string 在 vela.bin 里查不到）。诊断必须用 `DISP_PRINT`(=printf，`disp/de/include.h` 无条件定义)。本次正是先 `strings vela.bin` 查不到 "inst busy timeout" 才发现 → 反汇编 `.o` 确认循环已变成有界 → 换 `DISP_PRINT` 重编后 `strings` 命中（vela.bin 第 40434 行）。
- 修复后若仍偶发：串口会先打 `dsi_gen_wr inst busy timeout`，可继续查 DSI PHY/clk 建立时序（`lcd_power_on` 加点、`dsi_start` 前等 PHY ready 等）。

## 次要隐患（未改，记录备查）
- `board/r528s3/*/configs/env.cfg`：`boot_normal=sunxi_flash read 44000000 ${boot_partition};boot_rtos 44000000` 无长度参数。
- `nsh.fex` 无 `FREERTOS` RTOS_BOOT_MAGIC 头（首 8B=`1e0000ea` "rtos"），`u-boot-2018/cmd/sunxi_flash.c sunxi_flash_read_part` 无 magic 命中 → `rbytes = info->size * 512` = **整读 50MB bootloader 分区 + 每帧 ECC**。固件仅 33.34MB → 16.7MB 陈旧/擦除区被反复读（NAND 边际页风险）。
- 若要改：env 加显式长度（如 `0x2200000`=35.7MB）+ 分区收缩，但**固件峰值曾 42.7MB（P189）**，收缩分区有溢出风险，本次不做。

## 固化
- 编译通过（vela.bin 33,342,256B，含 `dsi_gen_wr inst busy timeout` 串）
- pack 成功（nsh.fex=0x1fcc330 与 vela.bin 一致）+ check_res.sh 通过
- snapshot：**ok-20260908-2**（唯一改动 = vendor/allwinnertech，de_dsi.c +12/-5）
- **遗留待上板验证**：反复断电开机 N 次观察是否还卡 LOGO；若出 `dsi_gen_wr inst busy timeout` 说明该分支被真实触发（属瞬时恢复路径）；若无打印且不再卡 = 闭环。