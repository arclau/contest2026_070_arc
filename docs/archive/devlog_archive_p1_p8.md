# DevLog 归档 — P1~P8续（8-3 至 8-6）

> 从 `devlog.md` 节点表归档，不再日常参考。需要时按 详情列 读子文档。

| P | 主题（根因/结论加粗） | 固化 | 详情 |
|----|----------------------|------|------|
| 8-6 P8续P7 | 切歌无声：驱动层全线排除 + **音量新线索**（XPlayer 调低） | — | 8-6part7 |
| 8-6 P8续P6 | **切歌无声重大突破：-16 EBUSY**（声卡未释放） | — | 8-6part6 |
| 8-6 P8续P5补 | 卡顿排查链：**提权/CPULOAD/多块读16崩回滚/laojie/PCM能量/音量30%** | ok-20260806-19~32 | 8-6part5 |
| 8-6 P8续P4 | 切歌无声（dapm_state）+ 卡顿缓解（period/buffer 512/2048） | ok-20260806-18 | 8-6part4 |
| 8-6 P8续P3 | **MP3 无声根治：44100→48000 重采样** | ok-20260806-17 | 8-6part3 |
| 8-6 P8续P2 | **无声根治：writei 字节账目 + 24bit**（上板出声） | ok-20260806-16 | 8-6part2 |
| 8-6 P8续P1 | XPlayer 收尾：编译/UI/日志 + 无声问题排查 | ok-20260806-1~14 | 8-6part1 |
| 8-5 P8 | **XPlayer 集成**（libcedarx 替换 nxplayer，parser 源码编译） | — | 8-5part8 |
| 8-5 P7 | **Git 版本管理体系**（git_snapshot.sh 三仓固化）+ WAV 崩溃更正 | f3e10224 | 8-5 |
| 8-5 P6 | **状态栏 WiFi/BT 符号根因=montserrat 字形残缺** → fallback 链 | 9975ca85 | 8-5part4 |
| 8-5 P5 | 渲染环境审计：**CONFIG_LV_THEME_DEFAULT_DARK 删除** | 249ac25a | 8-5part4 |
| 8-5 P4 | 上板差异：符号口口 + 字体过大 → 对齐模拟器字号 | cffc5299 | 8-5part4 |
| 8-5 P3 | 开机切 luncher_dm + **Data Abort 根因：lv_color_t 3B 越界 16KB** | 824a0f63 | 8-5part3 |
| 8-5 P2 | 圆角统一 40px + 子页大标题（subpage_big_title） | — | 8-5part2 |
| 8-5 P1 | 子 APP 统一 + **Music 移植重写** + Settings 重设计（玻璃卡体系） | — | 8-5 |
| 8-4 P4 | **Dock 换 Flyme 高清图标** + 一键锁屏（APK 提取 4066 图标） | — | 8-4part4 |
| 8-4 P3 | UI 视觉修整 + **天气 API（Open-Meteo）** + FontAwesome | — | 8-4part3 |
| 8-4 P2 | **Desktop Mate UI 移植 + 模拟器免刷机**（Xvfb/noVNC，DM×2.4） | — | 8-4part2 |
| 8-4 P1 | luncher 注册 + **1920x1200 横屏**（驱动层 G2D degree0 旋转）+ 触摸子页 | — | 8-4 |
| 8-3 P1 | **BOE 屏 + GT9271 触摸移植**（mipi_config 符号二选一互斥） | — | 8-3 |

---
*归档自 devlog.md · 2026-08-28*
