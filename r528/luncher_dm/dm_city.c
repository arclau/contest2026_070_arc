/****************************************************************************
 * apps/vendor/allwinnertech/apps/luncher_dm/dm_city.c
 * 城市设置实现（见 dm_city.h）。
 ****************************************************************************/

#include <stdio.h>

#include "dm_city.h"
#include "dm_health_cfg.h"   /* 序号持久化（/data/dm_health.cfg） */

#define DM_CITY_CFG_KEY   "weather_city_idx"

/* 12 城：idx0=惠州默认；只许追加，禁改既有序号（落盘序号兼容） */
static const dm_city_t g_dm_cities[] = {
    { "惠州", "23.11", "114.42" },
    { "广州", "23.13", "113.26" },
    { "深圳", "22.54", "114.06" },
    { "北京", "39.90", "116.40" },
    { "上海", "31.23", "121.47" },
    { "杭州", "30.29", "120.15" },
    { "南京", "32.06", "118.79" },
    { "武汉", "30.59", "114.30" },
    { "成都", "30.57", "104.07" },
    { "西安", "34.34", "108.94" },
    { "重庆", "29.56", "106.55" },
    { "长沙", "28.23", "112.94" },
};
#define DM_CITY_COUNT   ((int)(sizeof(g_dm_cities) / sizeof(g_dm_cities[0])))

int dm_city_count(void)
{
    return DM_CITY_COUNT;
}

const dm_city_t *dm_city_get(int idx)
{
    if (idx < 0 || idx >= DM_CITY_COUNT)
        idx = DM_CITY_DEFAULT_IDX;
    return &g_dm_cities[idx];
}

int dm_city_cur_idx(void)
{
    int idx = dm_health_cfg_get_int(DM_CITY_CFG_KEY, DM_CITY_DEFAULT_IDX);
    if (idx < 0 || idx >= DM_CITY_COUNT)
        idx = DM_CITY_DEFAULT_IDX;
    return idx;
}

const dm_city_t *dm_city_cur(void)
{
    return dm_city_get(dm_city_cur_idx());
}

void dm_city_set_idx(int idx)
{
    if (idx < 0 || idx >= DM_CITY_COUNT)
        idx = DM_CITY_DEFAULT_IDX;
    dm_health_cfg_set_int(DM_CITY_CFG_KEY, idx);
}
