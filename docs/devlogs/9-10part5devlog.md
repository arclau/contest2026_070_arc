# 9-10 part5 devlog — P211：日期与时间独立子页（ok-44）

> 日期：2026-09-10 ｜ 节点：P211 ✅ ｜ 基线 ok-20260910-44

## 一、动机（用户原话转需求）
- Settings 通用区“日期与时间”行点开是光秃滚轮面板，“很难堪”→ 宠物同款嵌套子页。
- 子页结构（iOS 同款）：hero 大时钟（FONT_CLOCK 56px，1s 活刷）置顶；
  自动设置区在上（总闸开关/上次同步/立即同步/说明小字），手动设置区在下
  （日期/时间两行 → 同一滚轮面板）。

## 二、真开关（非假 UI）
- 总闸 `dm_time_auto_enabled/set`（dm_weather.c，落盘 `time_auto_sync` 默认开）；
  关则天气链路跳过 settimeofday（只取天气），手动长期有效；开则踢一轮即时同步。
- 上次同步 `dm_time_last_sync`（成功 settimeofday 才记，UTC epoch，0=从未）。

## 三、滚轮面板升级（旧代码搬家+修）
- 年份 2024~2035 → 2000~2099（旧 RTC 没电回 1970/跨 2035 直接崩选中）。
- 修 2 月 31 日：年月动即重建日选项+钳选中（旧 mktime 静默滑到 3 月）。
- 加星期预览（所见即所得）；保存兜底钳位；RTC 野值钳进范围。
- Settings 行值=自动/手动（返回本页不重建，宠物改名同款惯例，已注明）。

## 四、接线
- `show_subpage("Datetime")` dispatch + close_subpage 清理 + Makefile 加
  `ui/ui_datetime.c`；旧面板代码从 ui_settings.c 整段删（cleanup 留空壳）。
- LV_SYMBOL_CALENDAR/CLOCK 不存在（查 symbol 表），用 LIST/EDIT 代。

## 待办
1. 烧 ok-44：L 清单（总闸关→手动保持/开→同步覆盖/面板星期/2月联动/hero活刷）。
2. 脱敏 push → fork 重搬（+ui_datetime.c）→ PR+CLA；视频 + 报告收尾（9.20）。
