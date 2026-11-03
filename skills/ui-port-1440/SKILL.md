---
name: ui-port-1440
description: Desktop Mate UI 迁到 1440×720（5.7" 横屏）专用流程。R528 + NuttX + LVGL 9 + G2D。含 1920×1200→1440×720 缩放 token 表、逐子 APP 迁移清单、复用现有 Theme/Material；性能优化另按任务验证。Trigger：迁新屏、缩 UI、1440、720、换屏适配、逐 APP 迁移、ui-port、分辨率缩小、DM_SCALE、子 APP 改造到新屏。
---

# ui-port-1440 — 逐子 APP 迁 1440×720

旧屏 1920×1200（16:10，scale 2.4）**不再支持**。新屏 **1440×720（2:1，DM_SCALE=2.0，逻辑画布 720×360）**。
**不是均匀缩 0.75**（高度会溢出）：保 widget 树与子 APP 结构，只重调几何 token。

布局定稿：`project_docs/specs/ui/home-layout-1440x720.md`（主屏唯一依据）。
Token 落地：`vendor/allwinnertech/apps/luncher_dm/dm_theme.h`（换屏唯一缩放源）。

## 铁律

1. **改任一屏前，整段读完对应构建函数**（AGENTS §〇.3）。
2. **只调 token / 几何，不改功能结构**：子 APP 页面树、事件、导航保持原样。
3. **禁止散落裸像素**：间距/字号/圆角一律走 `dm_theme.h` 的 `DM()` 与 token。
4. **左栏（时间+健康）最重要，宠物不做主角**。
5. **迁移只处理本轮尺寸问题**：复用现有 `dm_theme_*`；材质/G2D 性能优化另有需求和证据再做，不混入崩溃排查。
6. **按目标验证**：统一使用 `project_docs/specs/misc/development-workflow.md`。模拟器迭代不逐轮切板端；共享代码最终交付须板端打包校验。

## 缩放怎么算

| | 旧 1920×1200 | 新 1440×720 |
|---|---|---|
| 宽高比 | 16:10 | **2:1**（更扁） |
| DM_SCALE | 2.4 | **2.0** |
| 逻辑画布 | 800×500 | **720×360** |
| 高度比 | — | 720/1200 = **0.6**（按高收） |
| 宽度比 | — | 1440/1920 = 0.75（按高收后有余量→三栏） |

结论：以 **逻辑 720×360 + scale 2.0** 为唯一基准（见 `home-layout-1440x720.md` §1）。
旧常量→新 token 对照见 [references/tokens.md](references/tokens.md)。

## 逐 APP 迁移循环（每个子 APP 走完再下一个）

```
① 读    整段读完该页构建函数 + 事件回调（禁止只改局部）
② 盘点  控件树 / 硬编码像素 / 字体档 / 玻璃面（列进迁移笔记）
③ 换 token  布局值 → dm_theme.h 的 DM()/token（对照 tokens.md）
④ Theme  玻璃面 → dm_theme_frost/glass/card/bar/btn（删 COL_CARD+bg_opa）
⑤ 材质  保留现有实现；像素格式以目标配置/资源为准，不夹带加速改造
⑥ 保结构 功能/导航/事件不丢；子页仍 overlay，不用 lv_scr_load
⑦ 边界  在布局完成后核对父容器内容区、滚动与裁切，勿混用局部/全局坐标
⑧ 自证  按现行开发流程记录模拟器、板端编译打包与硬件验证状态
```

**顺序建议**：`shared(deskmate_ui 弹窗/子页框) → home → settings → wifi/bt → music → files/books → calendar/datetime/alarm → uart_dbg → pet`。
主屏 home 以布局稿为准，见 [references/apps.md](references/apps.md)。

## Theme / G2D 参考（有对应需求时才用）

| 项 | 做法 |
|----|------|
| 玻璃卡/条/钮 | `dm_theme_card_apply` / `bar` / `btn`，勿手写 opa |
| 磨砂底 | `dm_theme_frost_bg[_opa]` → 预渲染 PNG → `lv_draw_g2d_img` |
| 贴图 | 用 `tools/gen_frost_texture.py` 同款管线；C 数组 ARGB8888 |
| 规避 | 实时 blur、每帧 CPU 拉伸、小面积 <100×100 的 G2D 交叠 |
| 圆角 | 统一 `RAD_CARD`（token，勿散落） |

## 完成定义（单 APP）

- [ ] 构建函数整段读过，结构未删功能
- [ ] 无裸像素布局残留（token 化）
- [ ] 玻璃面全走 `dm_theme_*`
- [ ] 目标页模拟器显示/操作验证；共享代码交付前板端 `pack.sh` + check_res 通过
- [ ] 上板/模拟器确认：正文按 `FONT_BODY_PX` 等当前 token；小字/图标按各自规格，父容器内无意外裁切

## 相关

- `deskmate-ui`：LVGL 通用规范（图标/铁律/弹窗）
- `project_docs/specs/ui/home-layout-1440x720.md`：主屏定稿
- `dm_theme.h` / `dm_theme.c`：token + Material API
