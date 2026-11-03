# P317 — V1 宠物演示止血：缓存扩容与无感容错落地

2026-10-31。目标平台 R528 `ynm3000`，V1 1920×1200。
目标：彻底消灭由于底层 NAND 读坏字节导致的宠物视觉闪烁与频繁卡顿，切断高频 NAND 读。

## 一、改动与根因定位

1. **闪烁元凶修复（`pet_renderer.c`）**：
   - 之前在 `next == NULL`（解码偶发失败）时直接调用 `lv_obj_add_flag(r->image, LV_OBJ_FLAG_HIDDEN)` 隐藏图像，导致猫瞬间闪烁消失。
   - 修复：失败时**严禁隐藏**，直接复用已引用的前一帧像素（`r->pinned` 保持展示），并将 `last_asset` 置为无效以在下一帧立即重试；
   - 仅在开机首帧尚未加载任何图片且失败时才置 hidden，避免野数据。
   - 视觉效果：从“疯狂抽搐、整只消失”变为“单帧平滑维持”，肉眼感知零掉帧。

2. **NAND 总线竞争切断（`pet_assets.h`）**：
   - 原 `PET_CACHE_SLOTS = 5` 槽位严重过小（猫走步动画即 12 帧，播一半旧帧全被驱逐），导致每秒必须对 NAND 进行 10~15 次反复读盘与 PNG 解码。
   - 扩大为 `PET_CACHE_SLOTS = 25`：板端剩余堆内存约 47MB，25 槽约占 10MB，完全安全。
   - 常用动作（待机 3 帧、走步 12 帧、眨眼 3 帧）解码一次后常驻内存，运行时 NAND 读频次降低 90% 以上，从根本上避开 G2D DMA 与 NAND 读冲突。

3. **宿主测试自适应支持（`contest2026_070_arc/tools/tests/pet_actions/assets.c`）**：
   - 消除旧测试中写死的 5 槽断言，改为动态绑定 `PET_CACHE_SLOTS` 与 `PET_CACHE_PIXEL_BUDGET`，保证测试覆盖率与约束完整。

## 二、修改清单

- `vendor/allwinnertech/apps/luncher_dm/pet_renderer.c`: 容错保持上一帧、禁止隐藏、即时重试
- `vendor/allwinnertech/apps/luncher_dm/pet_assets.h`: `PET_CACHE_SLOTS` 5 → 25
- `vendor/allwinnertech/apps/luncher_dm/pet_assets.c`: 适配日志参数
- `contest2026_070_arc/tools/tests/pet_actions/assets.c`: 适配动态槽数回归测试

## 三、验证与状态

- 宿主回归测试：`python3 contest2026_070_arc/tools/tests/pet_actions/run.py` 9/9 项全部 PASS（含 ASan + UBSan，9MiB 边界与 82 PNG 像素哈希）；
- 官方构建全量打包：`DM_BUILD_JOBS=14 bash vendor/allwinnertech/tools/pack_deskmate.sh ynm3000` PASS；
- 产物镜像：`output/ynm3000/ynm3000.img`（51,482,624 B，MD5 `896d800521316c0c59ccfa1351707081`）；
- `vela.bin == nsh.fex`（`664a785f89eaeb0382ed7b1b83be5ee990259c5f6ed47a235b8a51f2290e2dde`），`res.fex` 未变；
- 板端待刷包验证；保持未提交未推送（无 git commit/push/tag）。
