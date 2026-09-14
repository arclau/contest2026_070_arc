# DevLog 2026-08-05 — 全 APP 圆角统一 40px + 子页 iOS 大标题

> 工程：openvela (R528) — nsh 配置
> 工作目录：`/data/dm`
> 工具链：SDK 自带 prebuilts/gcc/linux-x86_64/arm-none-eabi (GCC 13.4.0)
> 打包流程：`lichee` 下 `source envsetup.sh && lunch_nuttx 2 && pack`，镜像输出至 `lichee/out/r528s3/gemini-s1_nand/`
> **AI 交接：新会话请先读 `AGENTS.md`，本文件是当日归档，仅按需查阅。**

---

## 一、任务背景

用户在 web 预览上反馈两件事：
1. **圆角不统一**：子 APP 里的卡片、插件圆角与 Settings 卡片不一致，要求全部改成 Settings 卡片圆角（= `RAD_CARD` 40px）。
2. **子页缺标题**：Games/Books/Files/AI 返回键下方没有当前子 APP 名称大标题，要求同步 Settings 的设计风格（FONT_TITLE 大标题），但**保留各自功能形态**（书仍是书架、文件仍是文件管理器），不要照抄 Settings 的分组列表结构。

## 二、全 APP 圆角统一 = 40px

**确认基准**：Settings 卡片圆角即 `RAD_CARD`（40px），全 APP 所有白卡（首页插件、Settings 分组卡、各子 APP 卡片）此前已统一 40px。

**用户决策（问卷确认）**：范围 = **除圆形按钮/进度条外全部统一 40px**。

**改动明细（`apps/luncher_dm/deskmate_ui.c`）**：

| 文件 | 改动 |
|------|------|
| `deskmate_ui.c` | 13 处 `lv_obj_set_style_radius(x, RAD_*, 0)` 统一改为 `RAD_CARD`：Settings/子页图标徽章 ×6（原 20）、天气卡每日小格 ×5（原 20）、专辑封面（原 36）、书籍封面（原 20）、迷你封面（原 12）、歌单行（原 12）、Files 文件徽章（原 12）、dock 底（原 28）+ dock 图标按钮（原 20） |
| `deskmate_ui.c` | 删除冗余宏 `RAD_BADGE(20)/RAD_SMALL(12)/RAD_COVER(36)/RAD_DOCK(28)`，仅留 `RAD_CARD 40`，注释更新为"统一 40px + 例外项说明" |

**保留原值（有意为之）**：圆形按钮/触点圆点 `LV_RADIUS_CIRCLE`、进度条/slider `DM(2)/DM(3)`、分隔线/全屏遮罩 `0`。

**收益**：后续调圆角只需改 `RAD_CARD` 一处，全 APP 生效。

## 三、子页 iOS 大标题 + 顶部对齐

**设计**：复用 Settings 的大标题写法，抽出通用 helper；四个子页从"整页垂直居中"改为"顶部对齐 + 大标题"，内容形态不变。

| 文件 | 改动 |
|------|------|
| `deskmate_ui.c` | 新增 `subpage_big_title()` helper（FONT_TITLE + COL_TEXT + pad_bottom 12，与 Settings big_title 同款） |
| `deskmate_ui.c` | Games：flex 改 COLUMN + 顶部对齐，大标题 "Games"，2×2 卡片网格包进 `grid` 容器（ROW_WRAP 移到 grid 上） |
| `deskmate_ui.c` | Books：顶部对齐 + 大标题 "Books"，hero + 书架网格不动 |
| `deskmate_ui.c` | Files：顶部对齐 + 大标题 "Files"，Storage 卡 + 文件列表不动 |
| `deskmate_ui.c` | AI：顶部对齐 + 大标题 "AI"，hero + 动作网格不动 |
| `deskmate_ui.c` | Music：保持居中播放器形态，不加标题（播放器不需要） |

## 四、BUG 修复：大标题横向居中

| 现象 | 根因 | 修复 |
|------|------|------|
| 用户反馈 Books/Files/AI 大标题横向居中，而 Settings 在左边 | 首次实现给三个子页加了 `lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER)` → **交叉轴（水平）= CENTER**，导致子对象（含标题）横向居中；Settings 与 Games 都未设置该行，故靠左 | **删掉三处 `lv_obj_set_flex_align` 调用**（共 6 行），只保留 `flex_flow + pad_row`，与 Settings 写法完全一致；代码加注释提醒"勿额外设置 flex_align 交叉轴" |

**经验教训**：子页布局对齐一律参照 Settings 写法，不要额外设置 flex_align 交叉轴为 CENTER。

## 五、验证

```bash
# 每轮改动均执行
cd /data/dm && ./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh -j$(nproc)   # 0 error 0 warning
cd /data/dm/vendor/allwinnertech/lichee && source envsetup.sh && lunch_nuttx 2 && pack
ls -la .../image/nsh.fex /data/dm/nuttx/vela.bin   # 字节一致
bash /data/vela/sync_deskmate.sh -r                # 板端→模拟器 + 重建重启
```

- 最终产物：`nsh.fex == vela.bin`（5311936 bytes，md5 `4ab3d72d58b933f0e546709a68b7f905`）
- 模拟器：lvglsim 重建 OK，web 预览已刷新

## 六、遗留事项 / 下一步

1. **模拟器预览确认**：浏览器需强刷（Ctrl+F5）看新 UI；子页大标题 + 顶部对齐 + 全 APP 40px 圆角观感待用户确认（需微调只改 `RAD_CARD` / 标题样式）
2. **模拟器字体观感**：模拟器 montserrat 48 替代板端 MiSans 110，大标题观感偏"扁平"（已知差异，dock 图标已两端一致）；后续如要两端字体一致，可在 `deskmate_main.c` 用 freetype 加载 MiSans ttf 或调整字号/字重（不动板端固件）
3. Text Size 保持演示态（延续）
4. 上板验证：WiFi 联网取真实天气、dock PNG 渲染、内存/刷新率表现（延续）
5. 背景图 320x240 旧图待换 1920x1200（延续）
6. AI 卡仍为占位；传感器/LED/触摸测试入口未挂 Dock（延续）
7. 工具链改进：`sync_deskmate.sh` 构建失败检测建议加时间戳/日志校验（延续）

---

*DevLog by AtomCode (deepseek-v4-flash)*
