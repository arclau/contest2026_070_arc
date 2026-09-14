/****************************************************************************
 * apps/vendor/allwinnertech/apps/luncher_dm/dm_weather.h
 * Daily weather data (Open-Meteo) for Desktop Mate UI
 *
 * Works on BOTH the R528 board (NuttX BSD socket) and the PC simulator
 * (POSIX socket) — the HTTP fetch + JSON extraction is plain C.
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 ****************************************************************************/

#ifndef __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_DM_WEATHER_H
#define __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_DM_WEATHER_H

#define DM_WEATHER_DAYS 5       /* today + next 4 days (fills card width) */

typedef struct
{
    int     tmax;               /* daily max temperature °C */
    int     tmin;               /* daily min temperature °C */
    int     code;               /* WMO weather code */
    int     wday;               /* weekday: 0=Sun .. 6=Sat */
    char    date[16];           /* "2026-08-04" */
} dm_daily_t;

typedef struct
{
    int         cur_temp;       /* current temperature °C */
    int         cur_humid;      /* current relative humidity % */
    int         cur_code;       /* current WMO weather code */
    int         n_days;         /* days actually filled (≤ DM_WEATHER_DAYS) */
    dm_daily_t  days[DM_WEATHER_DAYS];
} dm_weather_t;

/* Fetch current + daily weather from Open-Meteo. Returns 0 on success,
 * -1 on network/DNS/parse failure. Caller keeps the result until next call. */
int dm_weather_fetch(dm_weather_t *w);

/* WMO code → 中文天气文本（UI 天气卡显示），如 0→"晴", 61→"雨" */
const char *dm_wmo_text(int code);

/* 2026-08-10：最近一次 fetch 的 API 对接 debug 信息（状态/耗时/错误）。
 * 仅串口日志使用（2026-08-23 起 UI 不再显示时延信息）。线程安全：
 * 由 worker 线程写、UI 线程读，均为整串拷贝的静态缓冲。 */
const char *dm_weather_debug(void);
/* 2026-09-10 P211：自动对时总闸（日期与时间子页开关，默认开） */
int dm_time_auto_enabled(void);
void dm_time_set_auto(int on);
/* 上次自动同步的 UTC epoch（0=从未同步；仅成功 settimeofday 后更新） */
long dm_time_last_sync(void);
void dm_time_note_synced(long utc_sec);
/* 开机早期调用：系统时间未对时时用上次落盘时间兜底（T1） */
int dm_time_restore_at_boot(void);

#endif /* __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_DM_WEATHER_H */
