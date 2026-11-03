# P346 — 返回键"偶尔卡一下"：宠物坏帧每拍重读 NAND

2026-11-02；V1 ynm3000。用户反馈 LVGL UI 优化中，点左上角返回**偶尔**还是会卡一下。

## 证据

串口日志（20 小时）两个维度都指向宠物取帧：`[perf] 60s` 的 `round max` 卡顿窗口 116~135ms、
伴随 `drop=180~490`，上下文几乎都带 `pet_assets_acquire`；宠物 PNG 校验统计 `PNG input mismatch`
**4996 次**、涉及 **67 个 asset**、`unverified` 1170 次（asset 66/7/80/62 各被重读 650/640/582/559 次）。

> **P349 更正**：`drop` 不是丢帧数，是 `dm_perf.c` 里采样数组 `PERF_SAMPLE_MAX=1024` 的溢出计数
> （实测 `flush=1350 → drop=326`，正好 1350-1024）。它只说明该窗口刷新次数 >17 次/秒（窗口很忙），
> 不能当卡顿指标用。判断卡顿只看 `round p50/p95/max`。

## 根因

`pet_anim_cb` 是 **33ms（30Hz）** 定时器 → `pet_renderer_step()`；取帧失败时它把 `r->last_asset`
置 `-1` 强制下一拍重试（`pet_renderer.c:39`）。于是一个"验不过"的素材活跃期间，UI 线程**每秒做
30 次**完整取帧：4 次 `png_read_once`（每次 78KB NAND）+ 4~8 次 `png_crc32`，而 `png_crc32` 原是
**逐位**实现（78KB = 62 万次循环），再加 4 行 `LV_LOG_USER` 走串口。一次失败几十毫秒 × 30Hz =
视觉上的"卡一下"。因为只有部分素材坏，所以是"偶尔"。

## 改动

- `pet_assets.c`：`png_crc32` 改**表驱动**（多项式/初值/取反不变，结果逐位一致）。
- `pet_assets.c`：坏素材**退避**——失败记 `asset_retry_at`，30s 起、每次失败翻倍到 120s，期内
  `pet_assets_acquire()` 直接返回 NULL（沿用上一帧，视觉与现状一致，但不再烧 NAND/CPU）；成功清零，
  `pet_assets_trim()`（资源重载）清空整表，保证有恢复窗口。
- `pet_renderer.c`：`missing frame` 日志限流成"同一 asset 每秒最多一行"（原 30Hz 刷屏）。
- `deskmate_ui.c`：`close_subpage()` 加分段埋点（清理/删树/收尾），总耗时 ≥20ms 才打一行。

## 验证（代码级）

- CRC 等价性：新表驱动 vs 旧逐位 vs `zlib.crc32`，含空串/9B/256B/78504B 实测向量，全部一致。
- 三文件均在构建日志里有 `CC:` 记录；镜像 `2768678e9ed27ffb1e8e39c56dd866d1`（51,531,776 B）；
  `nsh.fex` 里能搜到 `校验失败`、`[ui] close_subpage`、`missing frame`。
- 待上板：`missing frame` 应变成每个坏素材每 1~2 分钟一行；`[ui] close_subpage Nms` 出现时能看出
  卡在清理还是删树。

## 遗留

- 坏字节根因仍是 NAND/romfs 读路径（AGENTS §五：**禁止再在应用层打补丁**）。本次不改结论——帧照样
  拿不到、照样沿用上一帧，只是不再重复做注定失败的事。
- 关全屏子页本身要重绘整屏（约 50ms 量级），这部分是固有的，本次没动。
