# 9-09 Part1 DevLog — cat12.png 精灵替换（未闭环）

> 日期：2026-09-09
> 工程：openvela R528 nsh
> 参与：AtomCode (mimo-v2.5-pro)

---

## 一、今日做了什么

### 1.1 cat12.png 自动裁切 → 128×128 PNG

- 输入：`/data/dm/catimg/cat12.png`（1536×1024 RGBA）
- 脚本：`/data/dm/catimg/cut_cat_sprites.py`（已存在，改 INPUT 路径）
- 原理：Alpha 通道连通区域检测 → 自动忽略左侧文字 + 顶部编号 → 按12行×12列排序
- 输出：144 张 128×128 RGBA PNG → `/data/dm/catimg/cat_sprites/{state}/pet_cat_{state}_00~11.png`
- 目录映射：row00=idle, 01=walk, 02=groom, 03=sleep, 04=eat, 05=play, 06=happy, 07=sad, 08=hungry, 09=curious, 10=celebrate, 11=touch

### 1.2 PNG → C 数组（12帧复制补到16帧）

- 每状态12帧 + 复制帧00~03 → 16帧（`/tmp/pet_cat_16frames/`）
- `sprite_postprocess.py` 转 LVGL ARGB8888 C 数组 → 192 个 `.c` 文件
- 输出目录：`/data/dm/vendor/allwinnertech/apps/luncher_dm/pet_cat_*.c`

### 1.3 代码适配

| 文件 | 改动 |
|------|------|
| `dm_pet.h` | `PET_FRAMES_PER_STATE` 8→16, `PET_WALK_FRAMES` 8→16 |
| `pet_frame_table.c` | 11状态帧表 + walk帧表，全部16帧引用 |
| `pet_sprites.h` | 自动生成，192个 LV_IMAGE_DECLARE |
| `Makefile` | CSRCS 120→216项 |
| `dm_pet.c` L1117 | `g_tick / 2` → `g_tick`（修复动画循环周期翻倍BUG） |

### 1.4 编译打包固化

- `distclean` 全量重编 → 28MB，0 error
- `pack` → `rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img`
- `check_res.sh` 通过 ✅
- 固化：`ok-20260909-1`（首次）, `ok-20260909-2`（distclean 后）

---

## 二、验证结果（已做）

| 验证项 | 结果 |
|--------|------|
| C文件 .w 声明 | 全部128，0个旧256残留 ✅ |
| C文件 data_size | 全部65536，0个旧262144 ✅ |
| libapps.a 符号数 | 768个 pet_cat_ 符号，pet_cat_idle_00_map 只出现1次 ✅ |
| 像素级比对 | PNG源 BGRA=0x2C70B601 == C文件第111行首4字节 ✅ |
| 全项目 pet_cat_*.c | 仅 luncher_dm/ 下192个，无第二套 ✅ |
| ar 归档 | distclean 后无重复符号 ✅ |

---

## 三、遗留问题（未闭环 ⚠️）

### 🐛 板上猫"一时大一时小、3种形态切换"

**现象**：烧录后锁屏猫待机时会变来变去（胖/瘦/不同形态），点击后大小跳变。

**已排除**：
- ❌ C文件内容错误 → 像素比对一致，C文件确实是 cat12.png 的新素材
- ❌ ar 归档污染 → distclean 后符号唯一，无重复
- ❌ 旧 C 文件残留 → 全项目仅1套192个文件，无第二套
- ❌ 旧素材目录残留 → `pet_sprites/`, `pet_prompts/`, `cat_256/` 已删

**可能原因（待高阶AI排查）**：

1. **dm_pet.c 中硬编码256的代码**：
   - L1011: `PET_MIRROR_BUF_BYTES (256 * 256 * 4)` — mirror 缓冲按256×256分配，新素材128×128，功能不受影响但注释过时
   - L1020: `if (w > 256 || h > 256) return src` — 阈值检查，128×128通过，功能正确
   - L1103-1105: `nw` 默认256 → scale 计算 `PET_SIZE * 256 / 128 = 2x`，数学正确但注释写"256=1.0x"
   - **这些不影响功能，但需要确认是否有其他隐含的256假设**

2. **cat12.png 精灵本身形态不一致**：
   - 12行精灵图中，不同状态的猫可能体型/画风确实不同（胖橘 vs 瘦猫 vs 像素猫）
   - 需要用户确认：cat12.png 中12行是否是同一只猫的12种动作？还是混入了不同画风的猫？

3. **帧索引边界问题**：
   - `frame_idx = g_tick % 16`，当 `g_tick` 很大时取模正确
   - 但帧12~15是帧0~3的拷贝，动画循环时会有"回头"现象
   - 呼吸脉动 `sinf(g_tick * 0.32f)` 导致 ±1.5% scale 波动

4. **PET_FRAMES_PER_STATE=16 但行为池 duration 未调整**：
   - 旧8帧时 `PET_ANIM_TICK_MS=120ms`，全循环 `8×120=960ms`（`g_tick/2` 时=1920ms）
   - 新16帧时 `16×120=1920ms`（改 `g_tick` 后），循环周期不变
   - 但行为池 duration（ticks @500ms/tick）未变，行为切换时可能在非首帧切换

---

## 四、下一步（交给高阶AI）

### 优先级 P0：定位板上精灵异常的根因

1. **在板端加调试日志**：打印 `frame_idx`, `g_state`, `g_walk_pause`, `g_face_right`, `dsc->header.w`，确认实际显示的是哪张帧、什么尺寸
2. **确认 cat12.png 精灵一致性**：用 PIL 打开 cat12.png，逐行检查每只猫的 bbox 尺寸是否一致（宽高差异>10px = 不正常）
3. **检查 dm_pet.c 中所有对256的硬编码引用**：是否有隐含假设精灵是256×256的逻辑路径
4. **考虑是否需要回退到 PET_FRAMES_PER_STATE=12**（原始12帧，不复制）或保持16但修复其他问题

### 优先级 P1：代码清理

- `dm_pet.c` 注释中的"256×256"全部改为"128×128"
- `PET_MIRROR_BUF_BYTES` 改为 `(128 * 128 * 4)` 节省240KB RAM
- `dm_pet.c` L1020 阈值改为128

### 优先级 P2：动画体验优化

- 呼吸脉动幅度评估（±1.5% 在128px精灵上=±1.9px，可能太明显）
- 帧12~15 重复帧的"回头"感评估，考虑是否改用 ping-pong 模式

---

## 五、文件变更清单

| 文件 | 操作 | 说明 |
|------|------|------|
| `cut_cat_sprites.py` | 修改 | INPUT 改为 cat12.png |
| `dm_pet.c` | 修改 | L1117 `g_tick/2` → `g_tick` |
| `dm_pet.h` | 修改 | PET_FRAMES_PER_STATE 8→16 |
| `pet_frame_table.c` | 重写 | 16帧/状态 |
| `pet_sprites.h` | 重新生成 | 192个声明 |
| `Makefile` | 修改 | CSRCS 120→216项 |
| `pet_cat_*.c` ×192 | 重新生成 | 全部来自 cat12.png 的128×128素材 |

---

*DevLog by AtomCode (mimo-v2.5-pro) — 2026-09-09*
