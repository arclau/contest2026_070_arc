# P303 — V1 新猫帧接入 PET APP

2026-10-30。用户要求把已生成帧接到APP，不继续停留在修图；素材首轮齐全，视觉尚未最终通过。

- 目标：18个动作使用实际帧数与逐帧时长；宠物页可逐动作预览，锁屏宠物响应投喂、触摸、睡眠与提醒。
- 平台：V1 ynm3000，1920×1200；goldfish仅编译。按用户要求不启动模拟器、不截图。
- 边界：保留名字、开关、养成与存档；替换旧128px固定C帧/数字帧窗，不改V2。
- 资源：320px透明PNG按需解码，目标像素缓存≤2MiB；实际清单/版本写入产物，不以草稿齐全认定视觉通过。
- 已有未提交改动保留；源码前态、配置与哈希在`output/p303-pet-app/before/`及baseline.json。
- 验证计划：真实播放器/引擎/缓存宿主检查，goldfish构建，ynm3000官方编译打包/资源核验；硬件验收单列。
- 当前状态：**构建已通过**（2026-10-30 07:23）。不会commit/push/tag，不会启动模拟器或自动刷机。

## 构建与测试证据

| 项 | 结果 | 证据 |
|---|---|---|
| 宿主单测 | 7 组全 PASS（18 动作/时长边界、按住释放、睡眠喂食、优先级、80 张真 PNG CRC 与 2MiB 上限） | `output/p303-pet-app/host-tests-r2.log` |
| goldfish 编译 | 2541/2541，`build completed successfully (49 seconds)`，0 FAILED | `output/p303-pet-app/goldfish-build-r3.log` |
| 产物 | `cmake_out/vela_goldfish-arm64-v8a-ap/vela_ap.bin` 59,636,544 B，text 6.12 MiB；`nm` 含 `g_pet_actions`/`pet_core_*`/`pet_assets_*` | 同上 |
| 资源一致性 | manifest 80 张 PNG 与 `prebuilts/data/pet` 逐文件 SHA-256 相同，revision `33da60fdad55b0fd` 与二进制内嵌字符串一致 | 本节点脚本核对 |

### 构建卡点与处理（唯一变量）

1. 07:09 首次构建：`ccache: error: Read-only file system`，31 目标全挂。核对 `/home/<user>/.cache/ccache` 可写、根分区 rw，判定为瞬时故障，未改配置。
2. 07:21 重试：`lvgl/src/libs/gpc/gpc.c` 隐式声明 `fabsf`（`-Werror`）。根因是 07:07 新增的 `sim/dm_math_compat.h` 同时以 `-include` 注入 lvgl 目标，但只补了 `sinf/expf`。
3. 修复：仅在该头文件补齐 lvgl 实际使用的 float 原型（`fabsf/floorf/roundf/sqrtf/powf/fmodf/cosf/tanf/asinf/acosf/atan2f`），不改构建参数、不降 `-Werror`。板端不使用该头文件。
4. 宿主单测原始日志的 `LeakSanitizer fatal error` 是本机 ptrace 限制（LSan 无法在 ptrace 下工作），非测试失败；以 `ASAN_OPTIONS=detect_leaks=0` 复跑得到 7 组 PASS。

### 未验边界

- 模拟器**未启动**（用户要求）。宿主现有 QEMU 实例是 10-29 02:11 启动的旧客体，仍是旧程序；要看到新宠物需重启客体。
- 板端 ynm3000 官方编译打包与上板观感未做。
- 视觉（动作观感、锁屏互动）仍以用户复核为准。
