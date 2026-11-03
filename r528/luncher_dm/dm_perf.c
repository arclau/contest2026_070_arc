/****************************************************************************
 * apps/vendor/allwinnertech/apps/luncher_dm/dm_perf.c
 * DesktopMate V1 — LVGL 刷新量测埋点（B1，2026-10-28）
 *
 * 依据：project_docs/specs/ui/v1-lvgl-render-direction.md §3/§5-B1
 *   B0 结论（同目录文档 §6）：板端实际路径为
 *   lv_nuttx_fbdev(/dev/fb0, DIRECT 单缓冲) → LVGL 直接画进 fb mmap →
 *   flush_cb 只做 FBIO_UPDATE（脏区 dcache + G2D 旋转）。
 *   故用 display 事件分段计时，不碰 LVGL/驱动源码：
 *     REFR_START → REFR_READY   一轮刷新总耗时
 *     RENDER_START → RENDER_READY  绘制提交耗时（G2D 为异步，见文档口径）
 *     FLUSH_START → FLUSH_FINISH   flush ioctl 耗时（参数为该次脏区）
 *
 * 输出：每 60s 一行汇总到串口（与驱动 [fb] upd avg 同风格），不逐帧打印。
 * 关闭：DM_PERF_ENABLE=0（dm_perf.h）。
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

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <lvgl/lvgl.h>

#include "dm_perf.h"

#if DM_PERF_ENABLE

/* 窗口 60s、每类最多 1024 样本（超出计数 drop，不覆盖已有样本） */

#define PERF_WINDOW_MS   60000u
#define PERF_SAMPLE_MAX  1024u

static struct
{
  bool     inited;
  uint32_t win_start_ms;

  uint32_t round_us[PERF_SAMPLE_MAX];
  uint32_t draw_us[PERF_SAMPLE_MAX];
  uint32_t flush_us[PERF_SAMPLE_MAX];
  uint32_t n_round;
  uint32_t n_draw;
  uint32_t n_flush;
  uint32_t drop;

  uint64_t dirty_px;
  uint32_t flush_cnt;
  uint32_t round_cnt;

  uint32_t last_round_ms;
  uint64_t interval_sum_ms;
  uint32_t interval_cnt;

  size_t   heap_used;
  size_t   heap_peak;
  size_t   heap_total;

  struct timespec t_round;
  struct timespec t_draw;
  struct timespec t_flush;
  bool     round_active;
  bool     draw_active;
  bool     flush_active;
} g_perf;

/* 2026-11-01 P336：失效归因（诊断用）
 * LVGL 的 lv_obj_invalidate_area() 通过弱符号回调到这里，记录窗口内面积
 * 最大的 3 次失效：对象类名 + 对象几何 + 实际失效区。只读，不参与绘制。
 * 背景：打开「AI 语音」子页后每轮刷新多刷 ~300k px，需确认是谁在失效。 */

typedef struct
{
  uint32_t    px;
  char        tag[28];   /* "SCR" 或 "<类名>/<父类名>" */
  lv_area_t   obj;
  lv_area_t   area;
} perf_inv_t;

static perf_inv_t g_inv_top[3];
static uint32_t   g_inv_cnt;
static uint64_t   g_inv_px;

void dm_perf_note_invalidate(const lv_obj_t *obj, const lv_area_t *area)
{
  int32_t  w = lv_area_get_width(area);
  int32_t  h = lv_area_get_height(area);
  uint32_t px;
  int      i;

  if (w <= 0 || h <= 0)
    {
      return;
    }

  px = (uint32_t)w * (uint32_t)h;
  g_inv_cnt++;
  g_inv_px += px;

  if (px <= g_inv_top[2].px)
    {
      return;
    }

  for (i = 2; i > 0 && px > g_inv_top[i - 1].px; i--)
    {
      g_inv_top[i] = g_inv_top[i - 1];
    }

  g_inv_top[i].px   = px;
  lv_obj_get_coords(obj, &g_inv_top[i].obj);
  g_inv_top[i].area = *area;

  /* 只给类名分不清"屏幕 / 全屏 overlay / content"——三者都叫 obj 且
   * 都是 1920x1200。补一个父类名，并用 SCR 标出真正的屏幕对象。 */
  if (obj == lv_screen_active())
    {
      strlcpy(g_inv_top[i].tag, "SCR", sizeof(g_inv_top[i].tag));
    }
  else
    {
      const lv_obj_t *par = lv_obj_get_parent(obj);
      const char     *cn  = lv_obj_get_class(obj) ? lv_obj_get_class(obj)->name : "?";
      const char     *pn  = (par && lv_obj_get_class(par))
                            ? lv_obj_get_class(par)->name : "-";

      snprintf(g_inv_top[i].tag, sizeof(g_inv_top[i].tag), "%s/%s", cn, pn);
    }
}

static uint32_t perf_now_us(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)ts.tv_sec * 1000000u + (uint32_t)(ts.tv_nsec / 1000);
}

static uint32_t perf_now_ms(void)
{
  return perf_now_us() / 1000u;
}

static uint32_t perf_elapsed_us(const struct timespec *t0)
{
  struct timespec t1;

  clock_gettime(CLOCK_MONOTONIC, &t1);
  return (uint32_t)(t1.tv_sec - t0->tv_sec) * 1000000u +
         (uint32_t)((t1.tv_nsec - t0->tv_nsec) / 1000);
}

static void perf_push(uint32_t *arr, uint32_t *n, uint32_t us)
{
  if (*n < PERF_SAMPLE_MAX)
    {
      arr[(*n)++] = us;
    }
  else
    {
      g_perf.drop++;
    }
}

static int perf_cmp_u32(const void *a, const void *b)
{
  uint32_t va = *(const uint32_t *)a;
  uint32_t vb = *(const uint32_t *)b;

  return (va > vb) - (va < vb);
}

/* 最近秩分位数；n=0 返回 0（窗口内无该事件） */

static uint32_t perf_pct(const uint32_t *src, uint32_t n, uint32_t pct)
{
  static uint32_t tmp[PERF_SAMPLE_MAX];
  uint32_t idx;

  if (n == 0)
    {
      return 0;
    }

  for (idx = 0; idx < n; idx++)
    {
      tmp[idx] = src[idx];
    }

  qsort(tmp, n, sizeof(uint32_t), perf_cmp_u32);

  idx = (uint32_t)(((uint64_t)n * pct) / 100u);
  if (idx >= n)
    {
      idx = n - 1;
    }

  return tmp[idx];
}

static uint32_t perf_max(const uint32_t *src, uint32_t n)
{
  uint32_t i;
  uint32_t m = 0;

  for (i = 0; i < n; i++)
    {
      if (src[i] > m)
        {
          m = src[i];
        }
    }

  return m;
}

static void perf_heap_sample(void)
{
  lv_mem_monitor_t mon;

  lv_mem_monitor(&mon);
  g_perf.heap_used  = mon.total_size - mon.free_size;
  g_perf.heap_total = mon.total_size;
  if (g_perf.heap_used > g_perf.heap_peak)
    {
      g_perf.heap_peak = g_perf.heap_used;
    }
}

static void perf_reset(void)
{
  g_perf.n_round      = 0;
  g_perf.n_draw       = 0;
  g_perf.n_flush      = 0;
  g_perf.drop         = 0;
  g_perf.dirty_px     = 0;
  g_perf.flush_cnt    = 0;
  g_perf.round_cnt    = 0;
  g_perf.interval_sum_ms = 0;
  g_perf.interval_cnt = 0;
  g_perf.heap_peak    = g_perf.heap_used;
  g_perf.win_start_ms = perf_now_ms();

  memset(g_inv_top, 0, sizeof(g_inv_top));
  g_inv_cnt = 0;
  g_inv_px  = 0;
}

static void perf_report(void)
{
  uint32_t r_p50 = perf_pct(g_perf.round_us, g_perf.n_round, 50);
  uint32_t r_p95 = perf_pct(g_perf.round_us, g_perf.n_round, 95);
  uint32_t d_p50 = perf_pct(g_perf.draw_us,  g_perf.n_draw,  50);
  uint32_t d_p95 = perf_pct(g_perf.draw_us,  g_perf.n_draw,  95);
  uint32_t f_p50 = perf_pct(g_perf.flush_us, g_perf.n_flush, 50);
  uint32_t f_p95 = perf_pct(g_perf.flush_us, g_perf.n_flush, 95);
  uint32_t r_max = perf_max(g_perf.round_us, g_perf.n_round);
  uint32_t f_max = perf_max(g_perf.flush_us, g_perf.n_flush);
  uint32_t interval_avg = g_perf.interval_cnt ?
                          (uint32_t)(g_perf.interval_sum_ms / g_perf.interval_cnt) : 0;

  /* 单位 us（tick=1000Hz，实际量化到 ~1ms）；无刷新窗口各计数为 0 */

  printf("[perf] 60s rounds=%u flush=%u dirty=%llupx "
         "round p50/p95/max=%u/%u/%uus draw p50/p95=%u/%uus "
         "flush p50/p95/max=%u/%u/%uus interval=%ums heap=%u/%u/%u drop=%u\n",
         (unsigned)g_perf.round_cnt, (unsigned)g_perf.flush_cnt,
         (unsigned long long)g_perf.dirty_px,
         (unsigned)r_p50, (unsigned)r_p95, (unsigned)r_max,
         (unsigned)d_p50, (unsigned)d_p95,
         (unsigned)f_p50, (unsigned)f_p95, (unsigned)f_max,
         (unsigned)interval_avg,
         (unsigned)g_perf.heap_used, (unsigned)g_perf.heap_peak,
         (unsigned)g_perf.heap_total, (unsigned)g_perf.drop);

  if (g_inv_cnt > 0)
    {
      /* 行要短：串口 1.5M 下超长行会被切成多段 RX，反而不好读。 */
      printf("[perf] inv n=%u px=%llu | "
             "#1 %s %dx%d@%d,%d a=%dx%d | "
             "#2 %s %dx%d@%d,%d a=%dx%d | "
             "#3 %s %dx%d@%d,%d a=%dx%d\n",
             (unsigned)g_inv_cnt, (unsigned long long)g_inv_px,
             g_inv_top[0].tag,
             (int)lv_area_get_width(&g_inv_top[0].obj),
             (int)lv_area_get_height(&g_inv_top[0].obj),
             (int)g_inv_top[0].obj.x1, (int)g_inv_top[0].obj.y1,
             (int)lv_area_get_width(&g_inv_top[0].area),
             (int)lv_area_get_height(&g_inv_top[0].area),
             g_inv_top[1].tag,
             (int)lv_area_get_width(&g_inv_top[1].obj),
             (int)lv_area_get_height(&g_inv_top[1].obj),
             (int)g_inv_top[1].obj.x1, (int)g_inv_top[1].obj.y1,
             (int)lv_area_get_width(&g_inv_top[1].area),
             (int)lv_area_get_height(&g_inv_top[1].area),
             g_inv_top[2].tag,
             (int)lv_area_get_width(&g_inv_top[2].obj),
             (int)lv_area_get_height(&g_inv_top[2].obj),
             (int)g_inv_top[2].obj.x1, (int)g_inv_top[2].obj.y1,
             (int)lv_area_get_width(&g_inv_top[2].area),
             (int)lv_area_get_height(&g_inv_top[2].area));
    }

  perf_reset();
}

static void perf_event_cb(lv_event_t *e)
{
  lv_event_code_t code = lv_event_get_code(e);
  uint32_t t_us;
  uint32_t t_ms;

  switch (code)
    {
      case LV_EVENT_REFR_START:
        t_ms = perf_now_ms();
        if (g_perf.last_round_ms != 0)
          {
            g_perf.interval_sum_ms += t_ms - g_perf.last_round_ms;
            g_perf.interval_cnt++;
          }

        g_perf.last_round_ms = t_ms;
        clock_gettime(CLOCK_MONOTONIC, &g_perf.t_round);
        g_perf.round_active = true;
        break;

      case LV_EVENT_RENDER_START:
        clock_gettime(CLOCK_MONOTONIC, &g_perf.t_draw);
        g_perf.draw_active = true;
        break;

      case LV_EVENT_RENDER_READY:
        if (g_perf.draw_active)
          {
            t_us = perf_elapsed_us(&g_perf.t_draw);
            perf_push(g_perf.draw_us, &g_perf.n_draw, t_us);
            g_perf.draw_active = false;
          }
        break;

      case LV_EVENT_FLUSH_START:
        clock_gettime(CLOCK_MONOTONIC, &g_perf.t_flush);
        g_perf.flush_active = true;
        break;

      case LV_EVENT_FLUSH_FINISH:
        if (g_perf.flush_active)
          {
            const lv_area_t *area = (const lv_area_t *)lv_event_get_param(e);

            t_us = perf_elapsed_us(&g_perf.t_flush);
            perf_push(g_perf.flush_us, &g_perf.n_flush, t_us);
            g_perf.flush_cnt++;
            if (area != NULL)
              {
                g_perf.dirty_px +=
                  (uint64_t)lv_area_get_width(area) * lv_area_get_height(area);
              }

            g_perf.flush_active = false;
          }
        break;

      case LV_EVENT_REFR_READY:
        if (g_perf.round_active)
          {
            t_us = perf_elapsed_us(&g_perf.t_round);
            perf_push(g_perf.round_us, &g_perf.n_round, t_us);
            g_perf.round_cnt++;
            g_perf.round_active = false;
          }

        perf_heap_sample();

        if ((uint32_t)(perf_now_ms() - g_perf.win_start_ms) >= PERF_WINDOW_MS)
          {
            perf_report();
          }
        break;

      default:
        break;
    }
}

int dm_perf_init(void)
{
  lv_display_t *disp;

  if (g_perf.inited)
    {
      return 0;
    }

  disp = lv_display_get_default();
  if (disp == NULL)
    {
      printf("[perf] no default display, instrumentation skipped\n");
      return -1;
    }

  lv_display_add_event_cb(disp, perf_event_cb, LV_EVENT_ALL, NULL);
  g_perf.inited = true;

  perf_heap_sample();
  perf_reset();

  printf("[perf] enabled: window=%us samples=%u\n",
         (unsigned)(PERF_WINDOW_MS / 1000u), (unsigned)PERF_SAMPLE_MAX);

  return 0;
}

#else /* !DM_PERF_ENABLE */

int dm_perf_init(void)
{
  return 0;
}

#endif /* DM_PERF_ENABLE */
