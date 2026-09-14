# 9-6 Part 2 — 电子宠物系统修复（P182~P184）

> 固化：ok-20260906-6 ~ ok-20260906-12

## P182 — 宠物基础修复（口口+透明+布局）

**现象**：锁屏显示"两个口口"，宠物不可见（sprite 全透明），健康卡被挤压。

**根因**：
1. `PET_BUBBLE_FONT = &lv_font_montserrat_24`（纯拉丁字体），气泡中文文字渲染为□□
2. 32个精灵 `pet_cat_*.c` 像素数据全 `0x00`（ARGB alpha=0，完全透明）
3. 宠物容器未加 `LV_OBJ_FLAG_FLOATING`，作为 flex 子元素破坏健康卡布局

**修复**：
- `PET_BUBBLE_FONT` → `FONT_BODY`（MiSans 中文字体）
- `PET_SYMBOL_FONT` → `FONT_ICON`
- Food emoji（🐟🍖🥕🍪）→ `LV_SYMBOL_IMAGE`
- body 恢复占位圆形（lv_obj + LV_RADIUS_CIRCLE + 状态颜色）
- 容器加 `LV_OBJ_FLAG_FLOATING` 脱离 flex 流

固化：ok-20260906-6, ok-20260906-7

## P183 — 像素风精灵生成 + 缩放显示

**过程**：
- `gen_pet_sprites.py` 重新生成真实猫咪精灵（之前只生成空数据就跑了）
- 先 48×48 → 超分区（288KB）→ 缩到 32×32（128KB）→ 固件刚好塞下
- body 改回 `lv_image` + `lv_image_set_src` 精灵帧动画
- `pet_frame_table.c` 脚本重新生成后需手动修3处：include dm_pet.h / 去重复 typedef / 去 static / 补全11状态帧

**用户反馈**：太小看不清 → 改 DM(256) → 还是小 → 发现 LVGL 9 `lv_image` 默认 1:1 渲染不自动放大 → 加 `lv_image_set_scale()` 显式缩放 → 显示大了但太粗糙

**最终方案**：像素风24×24精灵 + `lv_image_set_scale` 放大到 DM(128)≈307px
- 精灵数据 72KB（32帧），固件空间充裕
- `lv_image_set_scale(g_body, (uint32_t)PET_SIZE * 256 / 24)` 放大显示

固化：ok-20260906-8 ~ ok-20260906-11

## P184 — 动画/行为/触摸/喂食全链路

**诊断问题**：
1. 动画只切2帧 — gen_idle 头部仅±2px，在24px下肉眼不可见
2. 宠物不会走动 — wander 状态无位移代码
3. 点击没反应 — 触摸事件触发了但精灵帧全一样
4. 不能喂食 — 无喂食 UI
5. PLAY 弹跳漂移 — `lv_obj_set_y(cont, get_y+off)` 累加偏移

**修复**：
1. **动画帧重做**：8状态×4帧全部加大差异（idle ±8px头转+眨眼、happy ±10px弹跳+大幅摇尾、eat ±6px低头啃、play ±12px跑、sleep ±5px呼吸、celebrate ±18px蹦+双星）
2. **wander 移动**：idle/wander 每20tick随机选屏幕x目标，每tick渐进2px左右闲逛
3. **弹跳不漂移**：记录 `g_wander_base_y` 原始y，PLAY/CELEBRATE 做固定偏移不累加
4. **喂食弹窗**：短按宠物弹出食物菜单（鱼/肉/菜/零食+关闭），5秒自动消失，`feed_btn_cb` 触发 `PET_EVT_FEED_*`
5. **帧率提升**：精灵帧切换 4tick→3tick

固化：ok-20260906-12

## 关键文件改动

| 文件 | 改动 |
|------|------|
| `dm_pet.c` | 字体×2、FLOATING、body→lv_image+scale、动画回调重做（wander+弹跳）、喂食弹窗、前向声明 |
| `pet_frame_table.c` | 脚本生成+手动修（include/去static/补全11状态） |
| `pet_cat_*.c` ×32 | 24×24像素风真实猫咪精灵（gen_pet_sprites.py） |
| `gen_pet_sprites.py` | OUT_SIZE 120→24、动画帧差异加大 |

## P185 — 宠物代码质量修复（6项闭环）

**固化**：ok-20260906-22

**改动清单**：

| # | 项目 | 改动 |
|---|------|------|
| 1 | MUSIC_ON 路由错误 | `PET_EVT_MUSIC_ON` → `HAPPY`（原 `CURIOUS`），`MUSIC_OFF` → `IDLE` |
| 2 | SAD 无退出机制 | behavior_cb 加 `happy≥30 → IDLE` 自动回落 |
| 3 | EAT/PLAY 卡死 | 加固定持续时间（EAT 8tick/4s，PLAY 12tick/6s）+ behavior_cb 自动回 IDLE |
| 4 | 行为池膨胀 | IDLE 猫狗各 10→5 个，SLEEPY 5→3 个，对齐 spec §四 |
| 5 | 死字段清理 | 删 `pet_behavior_t.symbol/color`，全部行为池改为 4 字段 |
| 6 | 编译 warning | 删 `interact_btn_cb` 里 `unused food_idx` |

**验证**：编译通过，无 error/warning。

---

## P186 — 精灵质量评估 + AI 生图方案

**问题诊断**：24×24 精灵被 `lv_image_set_scale` 放大 12.8 倍到 307px，几何图元缩放后模糊不清，猫狗傻傻分不清。

**评估结论**：
- 根因：120px 画布几何图元→缩到 24px 这条路走不通
- 需要：直接在高分辨率上逐像素绘制真正的像素画
- 搜索了免费 AI 生图网站（Perchance.org / Free.ai / PromptSpace），确定白嫖方案
- 写好 44 条猫精灵 prompt（`tools/pet_prompts/cat_prompts.md`）
- 写好 PNG→LVGL C 数组转换脚本（`tools/png_to_lvgl.py`）
- 写好一键集成脚本（`tools/integrate_pet_sprites.sh`）
- 修改 dm_pet.c 缩放公式从 24→128

**最终决策**：不依赖外部网站，用 PIL/Pillow 自己写像素画生成器。

---

## P187 — PIL 像素画生成器 + 128×128 精灵集成

**固化**：ok-20260906-23

**改动**：

1. **新生成器** `tools/gen_pet_sprites_v2.py`：
   - PIL 逐像素绘制 128×128（不做缩放）
   - 黑色描边轮廓保证清晰
   - 大头大眼小身体 chibi 风格
   - 表情系统：6 种眼睛（open/happy/closed/half/sad/wide）+ 7 种嘴巴（neutral/happy/open/yawn/o/sad/chew）
   - 正弦波做呼吸/摇摆/弹跳动画
   - 粒子效果：爱心/星星/食物/ZZZ/闪光/眼泪
   - 贝塞尔曲线做尾巴弧线
   - 每帧差异≥4px

2. **44 张猫精灵**：11 态 × 4 帧，每态有专属表情+动作+粒子

3. **分区扩容**：bootloader 分区 16384→24576 扇区（8MB→12MB），usrdata 相应缩减（128×128 精灵比 24×24 大 27 倍）

4. **缩放适配**：dm_pet.c `lv_image_set_scale` 公式 24→128（2.4x 放大，原 12.8x）

**编译打包**：build → pack → check_res ✅ → git_snapshot ok-20260906-23

---

## 待续

- [ ] 狗精灵 44 张（生成器已就绪，改配色+耳朵+尾巴参数即可）
- [ ] 上板验证 128×128 精灵清晰度+动画效果
- [ ] 宠物属性栏常驻显示
- [ ] 生命周期影响外观（baby/juvenile/adult）
- [ ] 猫狗性格差异深化
