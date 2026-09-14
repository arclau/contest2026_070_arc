# 9-08 part1 — P197 精灵网格对齐 + 删 PET_STATE_SLEEPY

> 宠物子系统收尾：P191~P196 全部代码已在 ok-20260908-1（luncher_dm 单一提交 0ed5f815）。

## P197 内容
- **精灵网格对齐**：旧 `cut_sprite.py` 硬编码 `(110,35)` 起始 + gutter 硬编码 → 行错位、一帧混入邻帧。重写自动 gutter 网格检测：实测 band 为 **12 行 × 8 列**，home 段 6 列、walk 段 2 列（gap 分隔）；按 canvas 逐行求 bbox、取行高众数、直角对齐；idle_00 bbox 由 (44,53)-(211,202)，中心一致。
- **95 帧说明**：首背景帧跳过（与 bbox 求并集/均匀采样逻辑处理），实际切出 96 帧并全部与源比对验证；8 个 `pet_cat_sleepy_*.c`（粉色小精灵）删除，Makefile 同步清链。
- **删 PET_STATE_SLEEPY**（枚举 11 态 = 帧表 11 行 = 素材集，状态机保持 11 入口）：
  - `dm_pet.h` 枚举删 SLEEPY；`pet_frame_table.c` 帧表删 sleepy 行 + 独立 walk 组行。
  - `dm_pet.c`：行为选择 switch 完整（HAPPY/SAD/CELEBRATE/HUNGRY 恢复）；`PET_SLEEP_ENERGY_MIN=15` 池删；`state_names` 11 项。
- **scale/翻转并为单一体系**：负 `scale_x` 实现镜像翻转（`lv_image_set_scale_x(g_body,(uint32_t)sx)`，内部存 int32，align 默认< `_LV_IMAGE_ALIGN_AUTO_TRANSFORM` setter 生效），删除原双 image 翻转残砾（防 4-帧/8-帧花屏根源）。
- **can_walk 位移闸门**：非可走态（EAT/SLEEP/PLAY/TOUCH/GROOM/CELEBRATE）冻结位移，只留 IDLE/HAPPY/CURIOUS 走动；状态切回可走态先蹲坐 `16+rand()%20` tick；拖拽中整段位移跳过；删除 `pet_pick_new_target`/`g_target_x/y` 死代码。
- **精灵占画布**：旧内容 ~93px/256（36%）太小；新 cut `fit=72%` 主长边（`min(fit,4.0)`）→ ≈168×150，符合 SPEC 60~70%。

## 固化
- 编译 + pack + check_res.sh 通过，固件 33.3MB
- snapshot **ok-20260908-1**
- 遗留：上板验证新精灵质感/踱步/蹲坐/翻转/拖拽手感（与 P198 一并烧录验证）。