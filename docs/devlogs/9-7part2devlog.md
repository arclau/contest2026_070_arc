# 9-7 Part 2 — 胖橘 96 帧整链 + 黑猫根因排障（P189/P190，✅ 已闭环）

> 日期：2026-09-07
> 目标：胖橘 cat.png Sprite Sheet → 96 帧 C 数组 → 固件；修复行走/触摸时显示黑猫
> 状态：✅ 已闭环（固件全量核验 96/96 胖橘，tag ok-20260907-13/14）

## 一、背景

用户换新萌宠（胖橘猫），AI 生成 1254×1254 Sprite Sheet `catim/cat.png`（12 状态 × 8 帧）。
上一会话已做分区扩容（bootloader 24→50MB，ok-20260907-12）为装载 96 帧铺路。
本会话：切割 → 转 C → 扩帧 → 对接 → 固件核验，途中踩到"黑猫"排障。

## 二、P189 — 胖橘 96 帧整链（✅ ok-20260907-13）

### 2.1 切割（cat_256/，96 张）

`cutim/cut_sprite.py` 按用户任务书切割：

| 项 | 值 |
|----|-----|
| 源图 | `catim/cat.png` 1254×1254 RGBA（裁左 110px/顶 35px 标题区 → 1144×1219） |
| 格网 | 12 行 × 8 列 = 12 状态 × 8 帧（行序 idle/walk/play/eat/sleep/sleepy/happy/sad/hungry/curious/celebrate/touch） |
| 每格 | ≈143×101，alpha bbox 居中 256×256 透明画布，比例不变，未裁耳朵/尾巴/脚 |
| 输出 | `cat_256/<state>/pet_cat_<state>_00~07.png`（12 目录 × 8 = 96 张） |
| 校验 | sprite_check 逐状态通过（8 帧完整、色调一致、四角透明）；96 格全内容、0 空 |

### 2.2 转 C 数组（luncher_dm/pet_cat_*.c）

`png_to_pet_c.py`：256×256 PNG → **LANCZOS 放大 300×300 ARGB8888** → .c。
放大 300 的目的：运行时 `scale = PET_SIZE*256/nw = 300*256/300 = 256 = LV_SCALE_NONE` 无缩放，
**避开 G2D transform bug（P187 教训：非 256 缩放走矩阵 transform → Data Abort）**。

### 2.3 代码扩帧（4 → 8 帧）

| 文件 | 改动 |
|------|------|
| `dm_pet.h` | `PET_FRAMES_PER_STATE 4→8`、`PET_WALK_FRAMES 4→8` |
| `pet_sprites.h` | 重生成：96 个 `LV_IMAGE_DECLARE`；**删除** walk 的 idle 宏别名 |
| `pet_frame_table.c` | 重生成：11 状态 × 8 帧 + `g_cat_walk_frames` 8 真帧 |
| `Makefile` | CSRCS 44 → 96（唯一化无重复） |

walk 逻辑沿用 `dm_pet.c` 现有覆盖：`is_walking` 时切 `g_cat_walk_frames.frames[(g_tick/2)%8]` + LV_SCALE_NONE。

### 2.4 固件核验（P189 首轮）

- vela.bin 24MB → **42.7MB**，nsh.fex=42,734,384B < 50MB 分区 ✓，pack/check_res 通过
- 核验方法：PIL 编码每帧 300×300 BGRA 字节 → 全文件 find
- **⚠️ 首轮发现：residual 核验假阳性**（`.c` 文本正则解析字节遇到 `0x` 字面量错位，误报"96 全进"）

## 三、P190 — 黑猫根因排障（✅ ok-20260907-14）

### 3.1 现象

用户：行走=黑猫、拖拽=胖橘、偶尔黑猫。胖橘只在拖拽出现。

### 3.2 调查

- 拖拽与行走都走 `pet_frames()[g_state]` / `g_cat_walk_frames`，无独立图源 → 排除代码路径
- 96 张 PNG 全胖橘（橙色 56~82%，无黑）→ 排除素材
- **关键**：`nuttx.map` 显示 walk 段已链接（有 VMA），但固件里搜不到 walk 完整 360000B → 数据不对
- 深挖：`libapps.a` 里 **walk/touch_04-07 存在 `_1.o` 和 `_2.o` 双版本**，且 `_1` ≠ `_2`

### 3.3 根因

```
ar 追加式归档污染（NuttX Application.mk 构建机制）：
- 同名 C 文件每次重编追加新成员 `_N.o`，序号递增，旧成员永不替换/删除
- 同符号（pet_cat_walk_00）在 `_1.o`(旧) 和 `_2.o`(新) 各定义一份
- 链接器按成员顺序取第一个定义 → 走到旧 `_1.o`
- 旧 `_1.o` 是 ok-20260907-10 时代从 happy 帧复制的 walk 占位（RGB≈102/90/88 灰黑）
- 于是固件里 walk 8 帧 + touch_04~07（同批旧占位）= 黑猫；其余 88 帧是胖橘
- 拖拽关 g_dragging 时走状态帧(胖橘)，行走 is_walking 切 walk 帧(黑) → 用户看到的正好相反
```

### 3.4 修复

1. `ar d libapps.a` 删除旧 `_1.o`：walk 00~07 + touch 04~07（残留黑占位）→ 8+4=12 个
2. **暴露附加编译错误**：删 `.built` 强制重编后 `ui_settings.c` 报 `PET_TYPE_DOG` undeclared
   —— Settings 宠物类型切换 UI 早已引用，旧归档缓存掩盖至今（增量编译从未真正重编该文件）
   —— 修复：`dm_pet.h` 补 `PET_TYPE_DOG` 枚举（狗精灵待接，当前 set_type 仅存类型，图仍猫）
3. 重链 → vela.bin 42.7MB

### 3.5 终验（P189 复核，PIL 编码核验）

固件全量核验 **96/96 完整 360000B 全部命中**（补查此前假阴性帧 walk 8 / touch 04-07 均 OK@指定偏移）。

## 四、遗留 / 注意

- 🔴 **铁律（写死）**：改精灵后必须**从固件提取完整帧字节核验**（PIL 编码，勿从 `.c` 文本正则）。check 脚本：`/tmp` 内联 python，源 = `cat_256/` PNG
- 🔴 **ar 归档污染是持续风险**：任何同名 C 改动重编都可能留旧成员。以后改精灵后若固件数据不对，先 `ar t libapps.a | grep pet_cat` 查双版本
- 待上板：胖橘 96 帧行走/触摸效果（整包烧录 ok-20260907-14）
- 狗精灵 `PET_TYPE_DOG` 已可编译，真图待接（Settings 切换后图仍猫）

---

*DevLog by AtomCode (big-pickle)*