# P260 — goldfish 资源挂载修复 + 宠物朝向消失修复

> 日期：2026-10-16。平台：SIM（goldfish-arm64-v8a-ap）。状态：**已修复并抓屏验证**（中文/宠物均显示）。
> 承接 [P259](10-16-emu-init-stack-rootcause.md) 的“中文/宠物未显示”待办。

## 1. 结论

1. **资源未挂载**：goldfish 配置 `CONFIG_INIT_ENTRYPOINT="luncher_dm_main"` 直接进 app，**不跑 NSH**，
   而 `/dev/virtblk0→/system`（romfs）、`/dev/virtblk1→/data`（fatfs）**只在 `rc.sysinit` 里挂**（rc.sysinit 仅 NSH 执行）。
   故 guest 读不到 `/data/data/{fonts,pet,tones}` → 中文豆腐块、宠物空、提示音 open fail。
2. **宠物面朝右整帧消失**：`pet_view.c` 呼吸块无条件对朝右帧设 `scale_x` 负值（注释写“→ G2D 翻转”），
   但本配置 `LV_USE_DRAW_G2D=n`，**软件渲染器不画负 scale** → 整帧不渲染。G2D 关闭时朝右翻转本应由
   `pet_set_body_src()` 的 CPU 行翻转完成，负 scale_x 属多余且致命。

## 2. 证据

- QEMU 命令行（/tmp/emu.log）：`virtio-blk-device,bus=virtio-mmio-bus.0,drive=system`（read-only）、
  `...bus.1,drive=userdata` → 探测序得 `/dev/virtblk0=system(romfs)`、`/dev/virtblk1=userdata(FAT)`（与 rc.sysinit 假设一致）。
- `vela_data.bin`（FAT16，256M）根含 `data/`：`data/fonts/MiSans-Normal.ttf` 等、`data/pet/pet_cat_*.bin`（65552B=16B 头+128²×4）、`data/tones/`。
  挂 `/dev/virtblk1` 到 `/data` → `DM_RES_ROOT="/data/data"` 命中。
- 修复前日志：`[tone] open fail: /data/data/tones/...`；`[pet_view] created ... f0=0`（无帧）。
- 修复后日志：无 tone fail；`f0=0x40a2ca90`（帧已载入）；抓屏见中文全部正常渲染。
- 宠物消失：加临时探针（已还原）得 `faceR=1 → sx=-448`（负），`faceR=0 → sx=448`；
  连拍 6 帧中朝右帧宠物区无橙色像素。修后连拍 8 帧全为橙色（R>G>B）且左右镜像交替。

## 3. 改动

- `vendor/openvela/boards/vela/src/qemu_vela.c`：`board_init_mmio()` 末尾（virtio-blk 探测后）挂载：
  `nx_mount("/dev/virtblk0","/system","romfs",MS_RDONLY,NULL)`、
  `nx_mount("/dev/virtblk1","/data","fatfs",0,NULL)`，失败仅 `fwarn`。挂载点由 `inode_reserve_path` 自动创建。
- `vendor/allwinnertech/apps/luncher_dm/pet_view.c`：呼吸块 `lv_image_set_scale_x` 用 `#if LV_USE_DRAW_G2D` 分流；
  软件路径保持正 scale（翻转交给既有 CPU 行翻转）。

## 4. 验证

- 构建：`./build.sh .../goldfish-arm64-v8a-ap --cmake -j$(nproc)` 成功。
- 运行：`bash /data/vela/run-emu.sh restart`；抓屏见主屏中文、右栏橙色小猫（踱步、左右镜像）；无 assert/Data Abort/KASAN 告警。
- `workspace_check.py` exit=0（target=sim）。

## 5. 待办 / 边界

- `pet_view.c` 为**板端与 goldfish 共享**代码；本次仅 goldfish 构建运行验证，**板端编译/打包未做**（待验）。
- P255 阶段 B：统一 `doctor/build/start/status/stop/logs` 入口与文档，未做。
- 天气图标区仍显示 `-- 加载`（无天气数据），与本次无关。

*证据副本：`/tmp/emu.log`、`/tmp/final_full.png`、`/tmp/fx1_crop.png`、`/tmp/fx5_crop.png`。*
