/****************************************************************************
 * apps/vendor/allwinnertech/apps/luncher_dm/dm_city.h
 * 城市设置（2026-09-10 A 方案：无 GPS 定位的城市级天气解法）
 *
 * 板无 GPS，天气只需城市精度：Settings 手动设城市（默认惠州），
 * 一处设置两处共用——首页天气卡按城市换 open-meteo 坐标，
 * ai_agent 问天气读同一城市的落盘数据（/data/dm_weather.state）。
 * 城市存序号（dm_health_cfg int "weather_city_idx"），表只增不改序号。
 ****************************************************************************/

#ifndef __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_DM_CITY_H
#define __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_DM_CITY_H

typedef struct {
    const char *name;   /* 中文名（Settings 显示 + state 落盘 + AI 播报） */
    const char *lat;    /* open-meteo 纬度字串 */
    const char *lon;    /* open-meteo 经度字串 */
} dm_city_t;

#define DM_CITY_DEFAULT_IDX   0   /* 惠州（用户所在地） */

/* 表只读；越界序号自动钳位 */
int dm_city_count(void);
const dm_city_t *dm_city_get(int idx);

/* 当前城市（cfg 缺失回默认惠州）/ 设置（钳位 + 落盘） */
int dm_city_cur_idx(void);
const dm_city_t *dm_city_cur(void);
void dm_city_set_idx(int idx);

#endif /* __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_DM_CITY_H */
