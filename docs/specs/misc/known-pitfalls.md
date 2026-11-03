# 已知坑 — R528 DesktopMate（V1 全量 + V2 生效项）

> 排障时按需读。V1 经验需核对目标与当前配置；不能把旧现象当今天的事实。构建命令与验收以 [开发流程](development-workflow.md) 为准。

## 构建 / 资源

1. **mipi_config 二选一**  
   `CONFIG_T070S140B_MIPI` 与 `CONFIG_BOE_1200X1920_MIPI` 必须互斥编译，否则 `g_lcd0_config` 重复定义。

2. **`lv_color_t` 只有 3B**（LVGL 9.1 XRGB8888）  
   buffer 按 color format 字节数算（ARGB8888=4B/px），勿用 `sizeof(lv_color_t)`。

3. **LVGL 矩阵旋转死机**  
   横屏走驱动层 `degree0`，勿用 `lv_display_set_rotation`。

4. **资源必须进固件**  
   prebuilt 改后检查生成依赖与产物是否更新；必要时定点重建资源，不默认 `distclean`；烧录**整包镜像**；烧后板端 `ls /resource/` 验证。
   忘记重打包 → 板端是旧 `nsh.fex`/`res.fex`，易被误判成「资源没打进」。

5. **`POWER_ANA_CTL@0x348`**  
   只能 `update_bits()` 局部操作，**禁止整写**（会破坏其它电源域）。

6. **UI 有两棵独立源码树**  
   V1 `apps/luncher_dm/`、V2 `apps/luncher_dm_v2/`，互不同步；不要按“唯一源”假设改一处两边生效。  
   勿跑 `sync_deskmate.sh`（会覆盖板端）。提交镜像方向是**板端 → contest/r528**，用 `bash /data/vela/sync_r528.sh`。

## 打包 / 校验

7. **WiFi `download_fw status=0x27/0x15` + FreeType glyph 刷屏**  
   历史上被误判成「打包打进旧 res」。真相：  
   - 旧 res 里的 WiFi 固件是同一文件，校验会通过；只有固件被截断/写坏才 checksum fail → `0x27` = **res 分区未完整写入**（烧录环节）；  
   - 「忘了重打包 / 镜像里是旧 nsh.fex」同样会让人拿旧产物去烧。  
   → 现由 `pack.sh` post-dragon 钩子调用 `check_res.sh` 硬断言三段一致性。

## 驱动 / 硬件（V1 积累）

8. **BOE 1200×1920 面板**：init 序列由安卓 DTB 逆向翻译，改时序前先对照 `r528/vendor_drivers/.../BOE_1200x1920.c`。  
9. **GT9271 触摸**：地址选择时序、零长度写、返回值判据、坐标 mask/burst/maxpoint/circbuf 都有坑，见驱动源码注释。  
10. **ltr553 ALS**：暗光提醒防抖已做（9-15），改阈值先看 `dm_als.c`。  
11. **sun8iw20-codec**：录音路径改动影响 `check_res` 无关但影响语音链路，改前读 `archive/v1/sun8iw20-codec-quickref.md`。

## 文档 / 协作

12. **双源漂移**：`project_docs/` 是唯一主源；`contest2026_070_arc/docs`、`r528/` 是导出镜像。  
    只许 `sync_docs.sh` / `sync_r528.sh` 单向导出，**手改镜像会被下次同步覆盖**。  
    收尾自检：`bash /data/vela/sync_docs.sh --check && bash /data/vela/sync_r528.sh --check`

13. **`game_demo` / `deskmate_usbtest` 是本地测试专用**  
    Kconfig 注明「不进提交」，已写入 `vendor/allwinnertech/.git/info/exclude`，勿手动 `git add`。

## goldfish 模拟器（V2）

14. **配置头可能跨目标污染（历史报告）**
    曾报告源码树 config.h 优先于 CMake 生成头；P254 核对时源码树该文件已不存在。实际是否污染须查本次编译命令/预处理结果。现有 `DM_SIM_DEFS` 是局部兼容措施，不应推广成“缺什么宏都加 -D”；先保证内核、库、应用读取一致配置。

15. **构建前必须先杀模拟器**  
    `vela_data.bin` 是 CMake 构建期 `dd + mkfs.fat + mcopy` 生成的 FAT 镜像；  
    模拟器仍持有它时重建 → FAT 目录写坏。**现象**：guest 里中文变口口、`/data/data/fonts` 目录乱码。  
    **正确做法**：按 PID/命令核对并停止占用该输出目录的实例后构建。已坏镜像先保存现场，再定点重生成；禁止宽泛杀所有模拟器或直接删除整套输出。

16. **LVGL `bg_img` 不会拉伸**  
    LVGL 9 的背景图只有「平铺 / 居中单张」两种画法（`lv_draw_rect.c`），没有 stretch。  
    **现象**：128×128 磨砂贴图放进 372×220 卡片，只在正中画一小块 → 卡片几乎看不见。  
    **正确做法**：玻璃卡必须 `lv_obj_set_style_bg_img_tiled(obj, true, 0)`（见 `dm_theme.c`）。

17. **模拟器外壳 / 圆角**  
    `cmake_out/<board>/config.ini` 由 board CMake 生成，默认 `skin.name = xiaomi_smart_screen_10`（Xiaomi 平板外壳 + 圆角遮罩）。  
    **去外壳**：清空 `skin.name`、加 `showDeviceFrame = no`；分辨率另用 `-skin 1440x720`。  
    注：该文件每次 CMake configure 会重生成，需在启动脚本里补。

18. **模拟器无输入设备**  
    容器里没有 xdotool/python-xlib，且 NuttX adbd 不支持 `shell`（`adb shell` → `error: closed`）。  
    **可用**：`libXtst` 存在，用 `XTestFakeButtonEvent` 自建点击工具（`/tmp/xclick`），坐标 = UI 坐标 + 模拟器窗口偏移。

## 触摸 / I²C（P286，V2）

19. **`CONFIG_I2C_RESET` 在本树恒失效**  
    `ARCH_CHIP_R528`（`nuttx/arch/dummy/Kconfig`）没有 `select ARCH_HAVE_I2CRESET`，Kconfig 会把
    defconfig 里的 `CONFIG_I2C_RESET=y` 直接丢弃。写进去看似生效，实际 `I2C_RESET` 未定义，
    链接报 `undefined reference`。要用须先给芯片补 `select`，否则只能走 `r528_transfer()` 的恢复+重试。

20. **HX83102B：每笔事务前必须等总线空闲**  
    背靠背事务在前一笔 STOP 未释放 SDA 时发 START，控制器报 `st=00 lcr=2a`；控制台单笔事务
    永远不复现，因此“控制台能读、驱动不能读”不代表电气问题。E19 加 `hal_twi_wait_bus_idle()`
    后才首次出现可用触摸。

21. **HX83102B：全 `00` 帧是校验失败，不是抬手**  
    合法空闲帧是 `ff…ff 37`；全 `00` 应重读一次再计错误（E23）。SDA 被 IC 拉住
    （`lcr=2a`、`st=f9`）时 9 时钟有时解不开，此时**只有器件 RST 脉冲**有效；
    注意 `hx83102_detect_controller()` 不碰 RST（它只重开 AHB 桥 + 读 ID），
    `hx83102_reset_pulse()` 才是真正的复位。

22. **TWI 软复位会关总线时钟，之后 LCR bit-bang 全部无效**  
    `twi_soft_reset()` 置 SRST 会清 `TWI_CTL_REG.BUSEN`。没有总线时钟，`twi_set_scl()`
    写 LCR 不会在引脚上产生任何边沿——9 时钟恢复变成空转，SDA 仍被从机拉住
    （现象：`still stuck lcr=2a` 反复出现，而引擎使能时同一段代码能跑出 `recovered lcr=3a`）。
    **正确次序**：保持引擎使能 → 打 9 时钟 → 发 STOP → 再软复位清 bus-error 锁存 → 验证。
    要“先停引擎”就必须在打时钟前重新 `twi_enable(BUSEN)`。

23. **改 /etc 下的启动脚本后，增量编译不会重生成 romfs**（P327）
    `boards/<产品>/src/etctmp.c` 的规则写在 `nuttx/boards/Board.mk`，但子 make 里
    `BOARD_DIR` 展开为空，prerequisite 变成不存在的路径，因此只有目标文件缺失时才重跑配方。
    改 `start_wifi.sh` / `rcS.nsh` 后直接 `build.sh` 会“成功”但镜像里仍是旧脚本。
    **验证**：解码 `etctmp.c` 的十六进制数组搜改动字符串，或 grep 新 `vela.bin`。
    **强制重生成**：删 `etctmp.c/etctmp.o/libboard.a` 后再编译，打包前必须确认新脚本已进二进制。
