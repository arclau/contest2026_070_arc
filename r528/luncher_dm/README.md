# DesktopMate 桌面主应用（YNM-3000）

运行于 openvela（NuttX）的桌面 AI 伙伴主应用，板卡为 Allwinner **R528S3-Gemini-S1**，
搭配逆向点亮的 **BOE 1200×1920 MIPI DSI** 屏（横屏 1920×1200 使用）与 **GT9271** 触摸。

- 产品型号：**YNM-3000**
- 软件版本：见 `deskmate_ui.h` 的 `DM_VERSION` / `DM_BUILD_TAG`
- 开源协议：Apache 2.0

## 功能概览

- **桌面 OS**：状态栏 + hero 时钟 + 天气卡 + 健康卡 + 宠物区 + 6 应用 Dock + 锁屏待机。
- **电子宠物**：12 种情绪状态机 × 12 帧精灵（144 帧），触摸/喂食/命名/成长，断电持久化。
- **日历/闹钟/时间**：农历 + 二十四节气 + 纪念日；多闹钟有序插入 + 贪睡；无 RTC 时上次时间落盘兜底。
- **健康提醒**：LD2410B 毫米波在座检测 → 喝水 / 久坐分级提醒 + 每日统计。
- **AI 语音**：纯按钮 PTT，豆包实时语音（WebSocket），流式回复 + 本地播报。
- **多媒体/工具**：音乐播放（XPlayer）、电子书、文件管理、WiFi、蓝牙、UART 调试工具。

## 源码结构

| 文件 | 职责 |
|------|------|
| `luncher_dm.c` | 应用主入口（自启 / 开机切 luncher_dm） |
| `deskmate_ui.c/.h` | 共享构建器（卡片体系 / 子页框架 / 音乐核心）+ 全部宏（含版本号） |
| `ui/ui_home.c` | 主屏（状态栏 + 时钟 + 天气/健康卡 + 音乐 + Dock + 锁屏） |
| `ui/ui_wifi.c` `ui_bt.c` | WiFi / 蓝牙子页（iPad 风格） |
| `ui/ui_music.c` `ui_files.c` `ui_books.c` | 音乐 / 文件 / 电子书 |
| `ui/ui_settings.c` | 设置（WiFi/蓝牙/日期时间/闹钟/宠物/显示/关于） |
| `ui/ui_calendar.c` `ui_alarm.c` `ui_datetime.c` `ui_pet.c` | 日历 / 闹钟 / 日期时间 / 宠物 |
| `ui/ui_uart_dbg.c` | UART 调试工具子页 |
| `dm_pet.c` `pet_core.c` `pet_view.c` | 宠物门面 / 引擎 / 视图，`pet_cat_*_*.c` 为精灵帧 |
| `dm_ai.c` `dm_voice.c` | AI 子页 WebSocket 客户端 / 语音编排 |
| `dm_health.c` `dm_health_cfg.c` `dm_ld2410b.c` `dm_als.c` | 健康状态机 / 配置 / 毫米波传感器 / 光线 |
| `dm_net.c` `dm_weather.c` `dm_city.c` | WiFi 蓝牙对接 / 天气获取 / 城市坐标 |
| `dm_alarm.c` | 闹钟存储与触发 |

## 构建

在 openvela 工程根目录执行（GCC 13.4.0 由 SDK 自动注入）：

```bash
./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/deskmate -j$(nproc)
```

打包与烧录（整包 NAND 镜像，`res` 分区含 WiFi 固件/字体/提示音）见工程 `lichee/pack` 流程。

## 说明

- AI 语音凭证存放于 `packages/ai_agent/include/agent_secrets.h`（已 `.gitignore`，仓内仅提供
  `r528/agent_secrets.h.example` 模板）；评委无 key 时，除 AI 对话外全部功能本地可用。
- 语音交互为纯按钮 PTT（无唤醒词）。
