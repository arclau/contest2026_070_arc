# P359 壁纸常驻内存 + 所有感应播报前置提示音

目标：恢复文件壁纸但不回到"每帧解码"；所有由人体感应触发的播报先响提示音，再出人声。
平台：V1 `ynm3000` / `luncher_dm`。

## 一、壁纸

O：P348 关掉 `blue-hour.png` 后 home 回到 wash 渐变。串口证据是 70 秒内
765 条 `png decode failed`、相邻两条间隔 90~107ms —— 不是"图太大装不进缓存"，
而是**每帧重读 NAND 再解码，且解码经常失败**（读路径偶发坏字节，P312/P355）。
失败结果不进 LVGL 缓存，于是下一帧继续重试，单轮耗时 1.3~1.6s。

H：把"读"和"画"拆开——开机读一次（libpng 自带 chunk CRC，坏字节直接解码失败），
失败重读；成功后像素常驻内存，绘制只用一个 `lv_image_dsc_t`，不再碰 NAND。

改动：
- 新增 `dm_glass.c`：`dm_glass_wallpaper_prepare()` 读整文件（fstat 定长，
  最多 4 次重读/重解，PNG 解码失败即视为坏读），成功则填 1920×1200 的
  `lv_image_dsc_t`（BGRA = LV_COLOR_FORMAT_ARGB8888，8.8MB 常驻）；
  `dm_glass_wallpaper_src()` 返回该 dsc，未就绪返回 NULL。
- `dm_glass.h`：`dm_glass_apply_wallpaper()` 改成取常驻 dsc 设 `bg_image_src`；
  没有/尺寸不符就返回，调用方保留 wash 渐变。删掉 `DM_GLASS_USE_WALLPAPER` 开关。
- `luncher_dm.c`：`init_fonts()` 之后、`deskmate_ui_create()` 之前调用一次。
- `Makefile` / `sources.txt`：加入 `dm_glass.c`。

代价：开机多一次 PNG 解码（约几百 ms）+ 8.8MB 常驻堆（RAM 128MB / 堆 ~74MB）。
失败路径：4 次都失败 → 日志 `[glass] wallpaper unavailable` → 回 wash，不会每帧重试。

> 构建坑：改的是头文件里的 inline，增量构建不跟踪头依赖，必须
> `touch deskmate_ui.c ui/ui_home.c luncher_dm.c`。首次构建没 touch 时
> `dm_glass_wallpaper_src` 被 gc-sections 丢掉（`nm` 里没有该符号），可作判据。

## 一·补：板测失败 → 改成随固件编入 .rodata（同轮）

板上刷 P359 后壁纸仍然没有，而且串口**一条 `[glass]` 日志都没有**。
原因是 `CONFIG_LV_LOG_LEVEL_ERROR=y`：WARN 被编译掉，失败路径全程静默
（USER 级能出，所以宠物日志正常）。补齐证据后确认：

- `/resource/wallpaper/blue-hour.png` 存在、大小 1,611,686 B 正确；
- 堆空闲 48.7MB，9.2MB 解码缓冲不会是 malloc 失败；
- 同一时刻宠物 `PNG input mismatch` 每分钟上百条 → 开机阶段 res 读路径同样在坏，
  4 次重读救不回来。

结论：只要还从 romfs 读这张 1.6MB 图，就一定会失败。改为**不读文件**：
`tools/gen_wallpaper_blob.py` 把 PNG 编成 C 数组（八进制转义，6.9MB 源码），
随内核由 U-Boot 一次性载入内存，运行时零 NAND 读，解码结果确定。

- `dm_glass.c` 改为从 `dm_wallpaper_png[]` 解码；失败日志全部用 `LV_LOG_ERROR`
  （WARN 在本工程是编译掉的）。
- `Makefile`/`sources.txt` 增加 `wallpaper_blob.c`。
- `nsh.fex` 8.43MB → 10.04MB（bootloader 分区 50MB，余量充足）。

## 一·补二：成本模型（为什么以后不会再卡）

| 阶段 | 旧（P348 前） | 现在 |
|---|---|---|
| 每帧 | open/read 1.6MB NAND + inflate，失败则下一帧重来，单帧 90~107ms | 无。dsc 常驻，绘制只是内存拷贝 |
| 开机 | — | 解码一次，日志打 `decode=NNNms`（USER 级，板上可见） |
| 失败 | 每帧重试，永久卡 | 一次失败即回退渐变，不再重试 |

判定标准：上板看 `[glass] wallpaper ready ... decode=NNms` 与 `[perf] round p50`。
若 p50 因全屏重绘明显上升（>30ms），再把壁纸降级成 RGB565 或半分辨率拉伸——
先量再改，不靠猜。

## 二、提示音

O：用户反馈"突然有人在讲话，吓人一跳"。`dm_voice.c:voice_director_on_seated()`
只有 `CAT_GREETING` 走 `dm_tone_play_seq("bg_reward_heart", wav)`，
粒子/关心/调侃三类是 `dm_tone_play(wav)` 裸人声直接开口。
健康弹窗（`health_popup_show`）和暗光弹窗（`dark_popup_trigger_cb`）本来就有前导音。

改动：`dm_voice.c` 全部改为 `dm_tone_play_seq(intro, wav)`，
greeting 用 `bg_reward_heart`，其余用 `bg_reward_std`。

## 验证与产物

| 项 | 结果 |
|---|---|
| 编译/打包 | PASS，121 资源，入口/启动/整包校验（`output/p359-build.log`） |
| 符号 | `nm nuttx.elf` 有 `dm_glass_wallpaper_prepare` 与 `dm_glass_wallpaper_src`；`ui_home.o` 引用后者 |
| 镜像（P359 初版，板上壁纸失败） | `f334dd9d306a4d9b861a53278c4e5182` |
| 镜像（改 .rodata 后） | `473235ff39cc40c52b7671d759d3d8ac`，`nsh.fex` 10,044,880 B |
| 板端 | 未刷 |

## 待验

1. 开机后 home/锁屏有 `blue-hour` 壁纸；串口出现一条 `[glass] wallpaper ready 1920x1200 ...`。
2. `[perf] 60s` 里 `decode_png` 保持 0，`round p50` 仍在 20ms 量级（不能回到 1.3s）。
3. 坐下触发播报时先响一声再说话（粒子/关心类也一样）。
4. 若壁纸没出来，看是否 `[glass] wallpaper ... failed`（4 次）——那说明开机那一刻读坏，
   属于已知读路径问题，不是本次逻辑错。
