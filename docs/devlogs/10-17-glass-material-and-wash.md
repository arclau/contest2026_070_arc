# P271 — 玻璃材质统一与锁屏/首页渐变底

目标：修锁屏与首页两类「边缘」观感问题——按钮/卡片圆角外露方角、卡片边缘弧形接缝；
并修锁屏点空白无法唤醒。
平台：共享 LVGL 源码；goldfish ARM64 编译与运行验收；R528 待板端复验。

## 根因

1. **方角**：`bg_img` 任务不带 radius。`apps/graphics/lvgl/lvgl/src/draw/lv_draw_rect.c`
   `:215-232` 建 `lv_draw_image_dsc_t` 只取 coords，无 radius 字段；仅 fill(`:180`)、
   border(`:264`)、shadow(`:152`) 写 radius。故 frost 贴图恒为方形，圆角容器外露方角，
   128px 平铺还留横向接缝（同 P262 音乐圆钮结论）。
2. **弧形接缝**：LVGL 无 backdrop blur，wash 用硬边椭圆 blob；60% 半透明玻璃卡透出后
   在卡内可见一道弧线。实测 home hero 卡内亮/蓝分界 y=120 处 x≈136–145，与蓝 blob
   椭圆左边界理论 x≈136 吻合。
3. **唤醒失效**：锁屏 `body/mid/pet_col/rings_row/sbar` 由 `make_clean_cont` 创建，
   默认 `LV_OBJ_FLAG_CLICKABLE=true`，吞掉点击、不冒泡到 overlay 的 standby 点击回调。

## 修复

- `dm_theme_card_apply`（dm_theme.c）与 `style_card`（deskmate_ui.c:564+）**去掉 20% frost
  bg_img**，只留 白 60% + 顶白底 #D9E6F5 竖渐变 + 白边高光 + 蓝灰柔影 0x3A4A6B 20%
  （走 fill task，radius 生效）。圆角容器不再露方角。
- 锁屏 AI 圆钮改 `dm_theme_glass_round_opa`（白→#D9E6F5 竖渐变 + 1px 白边 + 对称柔影）；
  锁屏双圈卡、健康卡、欢迎卡改 `dm_theme_card_apply`；顺手去掉健康卡 P103 蓝色光晕（用户确认）。
- 新增 `dm_theme_wash_bg(parent)`（dm_theme.c/.h）：全屏 `lv_pct(100)` 竖向渐变
  （顶 0xDCE7FA → 底 0xF4EDF6，OPA_COVER，FLOATING、非 CLICKABLE）。铺满整屏 → 边界
  在屏外、逐像素插值无接缝，玻璃仍有色彩可折射。home 3 团 wash blob 与锁屏 blob 全部替换。
- **统一全部 frost 容器**：直接重写底层 `dm_theme_frost_bg_opa`——不再铺 `bg_img` 磨砂贴图，
  改走 `bg_color` 顶白→#D9E6F5 竖向渐变（fill task，radius 生效）。该原语是 `dm_theme_glass_opa` /
  `bar_opa` / `btn_opa` 及所有直接调用者的唯一底材，一处改即覆盖 wifi/bt/uart/files/datetime/pet/alarm
  与 AI 对话页（圆钮 circle、面板、输入条、对话框）。函数名保留（历史），语义 = 统一玻璃底填色。
  `ui_home.c` 顶栏自设的 20% frost bg_img 一并删除。
- 锁屏 `body/mid/pet_col/rings_row/sbar` 逐个 `lv_obj_clear_flag(..., LV_OBJ_FLAG_CLICKABLE)`
  修点空白唤醒。

## 验证结果

| 项目 | 结果与证据 |
|---|---|
| goldfish 编译 | `build.sh ...goldfish-arm64-v8a-ap --cmake -j$(nproc)`，成功 |
| 头文件隔离 | 构建前移开 Make 生成头，构建后全部恢复 |
| 卡片方角 | 锁屏双圈/健康/欢迎卡、音乐播放列表卡均无方角外露；见 [锁屏截图](../specs/ui/standby-landscape.png) |
| 弧形接缝 | 全屏渐变后 y=120/260 无 ≥6 阶跃；见 [首页截图](../specs/ui/home-wash.png) |
| 统一 frost 容器 | 设置面板/底栏、AI 对话面板+快问药丸+语音圆钮、[闹钟对话框](../specs/ui/glass-dialog-alarm.png)、[AI 对话页](../specs/ui/ai-glass.png) 圆角均无方角外露、无 tiled 接缝 |
| 点空白唤醒 | 逐容器清 CLICKABLE 后，点锁屏空白处可回到首页 |
| 色深残留 | RGB565（`LV_COLOR_DEPTH=16`，无 `LV_DITHER_GRADIENT`）留 ~3 段柔和横带，远优于硬边弧缝，接受 |
| R528 | 编译打包已交付：`bash /data/vela/pack.sh` 通过，dm_theme.c 重编进 `rtos_nuttx_..._128Mnand.img`，check_res 自证通过；实机触摸/G2D 观感待验 |

## 收尾补充（锁屏/HOME 健康卡底缘暗边）

- **遗留现象**：P271 后用户复看，锁屏与 HOME 喝水/久坐卡（均贴底、半透明白卡）底缘仍有一道
  暗边——`dm_theme_card_apply` 的 `shadow_offset_y=DM(3)`(6px) 下坠阴影透过半透明卡底，
  与 P262 圆钮已规避的「平底假边」同因，矩形卡漏了这条规则。
- **修复**：`dm_theme_card_apply` 改 `shadow_offset_y=0`（对称柔影），与 `dm_theme_glass_round_opa`
  圆钮同规；共享原语，设置/音乐/健康等所有卡片一并统一。
- **验证**：goldfish 重启加载新 ELF，抓 home/lock 屏，底缘亮度扫描无 ≤200 的暗带（仅有内部内容
  微降）；`pack.sh` R528 重编 dm_theme.c 并 check_res 通过（2026-10-17）。

## 代码边界

所属仓 `vendor/allwinnertech`，起点 HEAD `20b1e1dc612d2a8788871cd207820267f53324dd`。
本轮仅动 `dm_theme.c`/`dm_theme.h`（新增 `dm_theme_wash_bg`、`dm_theme_card_apply`、
重写 `dm_theme_frost_bg_opa`）、`deskmate_ui.c`（`style_card`）、`ui/ui_home.c`（wash、
CLICKABLE、锁屏材质、顶栏）、`ui/ui_music.c`（注释更新）。
开始即有 13 个 dirty 文件（前序会话改动）完整保留，非本轮新增。
剩余未处理：无圆角依赖 frost 的容器已随原语统一；`dm_glass_frost` 贴图仍被
`dm_theme_frost_src()` 保留（公共 API），未从构建剔除。
