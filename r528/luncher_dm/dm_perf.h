/****************************************************************************
 * apps/vendor/allwinnertech/apps/luncher_dm/dm_perf.h
 * DesktopMate V1 — LVGL 刷新量测埋点（B1，2026-10-28）
 *
 * 目的：按 project_docs/specs/ui/v1-lvgl-render-direction.md 的 §3 口径，
 * 在板端同一场景下取得绘制/flush 耗时 p50/p95、脏区像素、flush 次数与
 * LVGL 堆占用，用真实数据决定 E1 优化方向。
 *
 * 只读 LVGL 显示事件，不改 LVGL、驱动与页面构建；窗口汇总，不逐帧打印。
 * 关闭方式：把 DM_PERF_ENABLE 改成 0 重新编译，即回到无埋点基线。
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
 *
 ****************************************************************************/

#ifndef __APPS_LUNCHER_DM_DM_PERF_H
#define __APPS_LUNCHER_DM_DM_PERF_H

/* 1=启用埋点（量测固件）；0=关闭（性能基线/交付固件） */

#ifndef DM_PERF_ENABLE
#  define DM_PERF_ENABLE 1
#endif

/* 挂到默认 display 的 LVGL 事件上；无 display 时返回 -1，不重试。 */

int dm_perf_init(void);

#endif /* __APPS_LUNCHER_DM_DM_PERF_H */
