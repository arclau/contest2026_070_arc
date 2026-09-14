/****************************************************************************
 * apps/vendor/allwinnertech/apps/luncher_dm/dm_screenshot.h
 * 截图模块——物理按键触发全屏截图并存 TF 卡
 *
 * 2026-09-11 新增：
 *  - 触发：板载 LRADC 按键（/dev/input/event1，BUTTON_1~5），按下任一键截屏
 *  - 输出：/sdcard/screenshots/shot_YYYYMMDD-HHMMSS-N.png（PNG，满分辨率）
 *  - 线程：按键轮询跑在 LVGL 线程的 lv_timer 内，截图/写盘同线程，无线程安全问题
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.
 ****************************************************************************/

#ifndef __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_DM_SCREENSHOT_H
#define __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_DM_SCREENSHOT_H

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* 打开按键设备 + 创建轮询定时器（LVGL 线程内）。返回 0=成功/-1=失败 */
int dm_screenshot_init(void);

/* 立即截当前屏并存盘，返回 0=成功/-1=失败；成功后路径见 dm_screenshot_last_path */
int dm_screenshot_capture(void);

/* 最近一次成功保存的文件路径（无则 NULL） */
const char *dm_screenshot_last_path(void);

#endif /* __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_DM_SCREENSHOT_H */
