# 宠物架构方案 — PNG 序列帧 + 帧缓存（P5）

> 状态：**已实现**（2026-10-14）三项已定：G2D 翻转 / 预解码 .bin / PET_SIZE 放大 DM(140)  
> 前置：`PET_SPRITE_SPEC.md`（美术规格）· `PET_RES_PIPELINE.md`（进 res 流程）  
> 原则：左栏时间+健康优先，宠物不做主角；换图不重编逻辑。

## 0. 一句话

**res 分区存 PNG 序列帧 → 运行时按动作预解码到 RAM 帧缓存 → LVGL/G2D blit 播帧**。  
删除 144 个 `pet_cat_*.c` 内嵌 C 数组（约 59 万行）。

## 1. 现状问题

| 现状 | 问题 |
|------|------|
| 144 × `pet_cat_*.c`（4114 行/文件） | 固件 .text/.rodata 巨大；换图必须重编 |
| `pet_frame_table.c` 硬编码指针表 | 与图源强耦合 |
| `pet_view.c` 直接 `lv_image_set_src(&pet_cat_xx)` | 无加载/缓存抽象 |
| 128×128 ARGB8888 已解码常驻 | 内存随帧数线性涨，无法裁剪 |

## 2. 帧格式（定稿）

### 2.1 res 磁盘格式

| 项 | 值 | 理由 |
|----|-----|------|
| 路径 | `/resource/pet/pet_cat_<action>_<NN>.png` | 与 `PET_RES_PIPELINE.md` 命名一致 |
| 动作 | idle/walk/groom/sleep/eat/play/happy/sad/hungry/curious/celebrate/touch（12） | 对齐 `dm_pet.h` 状态序 |
| 帧号 | `NN` = 00..11（每动作 12 帧） | 对齐 `PET_FRAMES_PER_STATE` |
| 画布 | **128×128**，8-bit RGBA，背景 Alpha=0 | 1:1 blit 免缩放；G2D 友好 |
| 源稿 | 美术 256×256 → 管线缩到 128 入 res | 保质量又省 res/缓存 |
| 压缩 | PNG（lodepng/项目既有解码器） | res 体积小；换皮只扔文件 |

**不做** LVGL C 数组、不把 144 帧全部解码常驻。

### 2.2 运行时帧（缓存槽内）

```c
typedef struct {
    lv_image_dsc_t dsc;     /* ARGB8888，指向 buf */
    uint8_t       *buf;     /* 128*128*4 = 64 KiB/帧 */
} pet_frame_t;
```

- 内存序 **B,G,R,A**（LVGL 9 LE ARGB8888），与 `dm_glass_frost` 相同
- `dsc.header` 填 magic/cf/w/h/stride，供 `lv_image_set_src` 或自绘

## 3. 帧缓存（定稿）

### 3.1 容量

| 缓存策略 | 容量 | 说明 |
|----------|------|------|
| **单动作槽**（默认） | 12 × 64 KiB = **768 KiB** | 当前状态 12 帧全解码，播帧零 IO |
| 预取槽（可选） | 再 +768 KiB | 状态切换前预载下个动作，消灭切换卡顿 |
| **不缓存全库** | 144 × 64 KiB = 9 MiB | 否则无意义膨胀 |

R528 SDRAM 充足（当前约 25 MiB / 961 MiB），768 KiB～1.5 MiB 可接受。

### 3.2 生命周期

```
pet_frames_init()           开机：打开 res 目录，建空槽
pet_frames_load(action)     状态切换：解码 12 帧入槽（旧槽释放）
pet_frames_get(idx)         播放：取 dsc 指针（无分配）
pet_frames_prefetch(action) 可选：后台预载下一动作
pet_frames_release()        关宠/退出：释放槽
```

- 同一动作重复 `load` → 直接命中，不重复解码
- 解码失败：该帧指针回退 idle 帧，打日志，不崩
- 线程：LVGL 主线程解码；若将来异步，槽需 refcount

### 3.3 播放

- `pet_view` 只拿 `pet_frames_get(pet_core_anim_index())`
- 转向/缩放：`lv_image` scale/翻转 或 G2D transform（大面优先 G2D）
- 帧间隔沿用现有 `PET_ANIM_TICK_MS`（约 240ms，~4fps）

## 4. 模块边界

| 模块 | 职责 | 变化 |
|------|------|------|
| `pet_frames.c/h` **新** | PNG 读 res → 解码 → 缓存槽 → `get()` | 替代 144 个 pet_cat_*.c + pet_frame_table.c |
| `pet_core.*` | 状态机/属性/存档 | **不动**（仍 `pet_core_anim_range()`） |
| `pet_view.*` | LVGL 对象/动画/交互 | 改 `set_src` 来源为 `pet_frames_get()` |
| `dm_pet.*` | 门面（timer/事件） | load 时机挂钩状态切换 |
| `pet_cat_*.c` / `pet_frame_table.c` / `pet_sprites.h` | — | **删除**（编译摘掉） |

### 4.1 API 草案（pet_frames.h）

```c
int  pet_frames_init(const char *res_dir);   /* "/resource/pet" */
int  pet_frames_load(int action);            /* 0..PET_STATE_COUNT-1 + walk 组 */
const lv_image_dsc_t *pet_frames_get(int frame_idx); /* 0..11 */
void pet_frames_release(void);
int  pet_frames_cache_bytes(void);           /* 自检/日志 */
```

walk 可作为独立 action id（与现实现 `g_cat_walk_frames` 一致）。

## 5. res 管线（接 PET_RES_PIPELINE.md）

1. 美术 256×256 PNG → `tools/pet_prompts/`（源）
2. 脚本缩到 128×128 RGBA → `.../res/pet/pet_cat_<action>_<NN>.png`
3. `pack.sh` 打进 res.fex；**改帧必烧整包**
4. 板端 `ls /resource/pet` = 144；`check_res` 不校验帧数（清单补洞）

建议加 `tools/gen_pet_res.py`：缩放 + 命名检查 + md5 表。

## 6. 内存/性能预算

| 项 | 数 |
|----|-----|
| 单帧 | 64 KiB |
| 单动作缓存 | 768 KiB |
| 双槽（预取） | 1.5 MiB |
| 磁盘（PNG ~20KB/帧） | ~3 MiB res |
| 解码 12 帧 | 冷载约 50–150ms（lodepng）；状态切换可接受 |
| 播帧 | 缓存命中，G2D blit，无解码 |

## 7. 实施顺序（动代码时）

1. `pet_frames.c/h` + 单动作 load/get（先 idle）
2. `pet_view` 切到 `pet_frames_get`，删 C 数组引用
3. Makefile 摘 `pet_cat_*.c`；`pet_frame_table` 删除
4. res 脚本 + 144 PNG 入 `res/pet/`
5. `pack.sh` + 烧整包验证帧数/动画
6. 预取槽 / walk 翻转走 G2D（可二期）

## 8. 风险

| 风险 | 缓解 |
|------|------|
| res 漏清单静默丢帧 | 沿用 PET_RES_PIPELINE 检查项 + gen 脚本 |
| 解码慢导致切换卡 | 预取槽；或二期改 .bin 预解码进 res |
| PNG 解码器占用 | 已有 lodepng（LVGL），复用 |
| 大分辨率误用 256 入缓存 | 脚本强制 128；assert header.w==128 |

## 9. 已定决策（2026-10-14）

| 项 | 决策 |
|----|------|
| walk/朝向翻转 | **G2D** 负 `scale_x`；无 G2D 时 CPU 行翻转兜底 |
| res 格式 | **预解码 .bin**（16B 头 + BGRA），缺 bin 回退 PNG |
| PET_SIZE | **放大 DM(140)** ≈ 280px 物理（右栏舞台） |
