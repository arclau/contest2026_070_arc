# P348 — 1.3 秒级卡顿的真凶：壁纸 PNG 每帧重解码

2026-11-02；V1 ynm3000。用户刷完 P345+P346 镜像后反馈"越改越卡"。

## 证据（`[perf] 60s` 序列，同一份串口日志）

| 时段 | 固件 | rounds/分 | round p50 | drop | decode_png/分 |
|---|---|---|---|---|---|
| 03:02–03:13 | 旧 | 565→746 | 23~24ms | 200~384 | 0 |
| 03:16–03:19 | 新 | **44~135** | **107000~1361000us** | 0 | 639~650 |
| 03:20–03:24 | 新 | 315~319 | 16ms | **0** | 0 |

- 宠物侧确实变好了：`PNG input mismatch` 从 1881 次/分降到 20~50 次/分，`drop` 从 ~300/分降到 **0**。
- 但 03:16–03:19 出现 1.3~1.6s 的单轮耗时，同一窗口 `mismatch` 只有 24~50 次/分、`pet_assets_acquire`
  仅 47 次 —— **和宠物无关**。
- 该窗口 70 秒内 `decode_png: png decode failed` **765 条**、`_lv_draw_image_normal_helper` **757 条**，
  相邻两条间隔约 90~107ms；每分钟 650 次 ≈ 每轮 14.8 次 × 92ms ≈ 1.36s，与 p50 完全对上。

## 根因

`dm_glass.h: dm_glass_apply_wallpaper()` 把 `/resource/wallpaper/blue-hour.png` 设为 `bg_image_src`
（home + standby 两处）。该 PNG 是 **1920×1200、1.5MB**，解成 ARGB8888 需要 **8.8MB**，
而 `CONFIG_LV_CACHE_DEF_SIZE=5242880`（5MB）—— **永远装不进图像缓存**，于是每一帧绘制都要
重新 open/read/inflate 一遍。加上读回字节有损坏（P347 结论），inflate 失败后下一帧再来，形成
"每帧 90ms、每秒 15 次"的稳定卡顿。子页/不透明覆盖层挡住 home 后 LVGL 跳过该区域，卡顿随之消失
——这解释了为什么它是**成簇**的（01:15–01:26、03:16–03:19），而不是全程。

## 改动

- `dm_glass.h`：新增 `DM_GLASS_USE_WALLPAPER`（默认 **0**），关闭时 `dm_glass_apply_wallpaper()`
  直接返回，保留调用方已有的 `dm_glass_apply_wash()` 渐变。注释里写明 8.8MB vs 5MB 的账。
- 没有改动任何与"坏字节"有关的逻辑（AGENTS §五）。

## 验证

- `ui/ui_home.c`、`deskmate_ui.c` 在构建日志里均有 `CC:`；镜像
  `b39ca99f1fcb65b4db0eb4a35ac93426`（51,531,776 B），`check_deskmate.py` PASS。
- `nsh.fex` 里 `blue-hour` / `wallpaper/blue` 命中数 **0**（路径已消失），`PNG input mismatch`、
  `close_subpage`、`校验失败` 仍在。
- **上板实测（03:41 开机后）**：`decode_png` **650 次/分 → 0**；`round p50` **1361000us → 17000~20000us**；
  `round max` **1633000us → 39000~65000us**；`wallpaper` 日志 0 条。1.3s 级卡顿消失。
- 顺带确认语音链路正常：`TURN tts_first_chunk +3489ms`、`speak done: total 2522ms`、`xrun0=0 short0=0`。

## 附：`drop` 不是丢帧（P349 更正）

`drop=308/326` 看着像掉帧，其实是 `dm_perf.c` 里 `PERF_SAMPLE_MAX=1024` 采样数组的**溢出计数**：
`flush=1332→drop=308`、`flush=1350→drop=326`，两条都精确等于 `flush-1024`。它只表示该窗口
刷新次数超过 17 次/秒（=窗口很忙），**不能当卡顿指标**。卡顿只看 `round p50/p95/max`。
当前 `flush p50=0us`（一半刷新是空操作）、`p95=16000us`，`dirty` 约 22 个整屏/分，属正常负载。

## 遗留

- 壁纸本身没删，`blue-hour.png` 还在 res 里。要恢复视觉，得改成"开机解一次 → 缩到 ≤1/4 →
  存成 `lv_image_dsc_t` 常驻 + `lv_image_set_scale`"，不能再走 `bg_image_src` 这条路。
- 读回损坏（P347）仍是 NAND/romfs 读路径问题：镜像内容逐字节正确（82/82 按内容定位成功、
  67/67 crc 与 exp 一致），坏文件在 6.1MB 数据区里均匀分布（每段 62%~100%），且同一文件每次重读
  都拿到不同的错误内容 —— 不是刷写坏，重刷 res 不会改善。
