/****************************************************************************
 * apps/vendor/allwinnertech/apps/luncher_dm/dm_als.h
 * LTR553 ALS 环境光服务层（P153：锁屏亮度显示 + 暗光开灯提醒）
 *
 * 数据流：LTR553 驱动（CONFIG_SENSORS_LTR553=y）→ sensor_register
 * (SENSOR_TYPE_LIGHT) → uORB sensor_light 主题 → 本模块
 * orb_subscribe_multi + LVGL 1s timer 轮询 → 全局 lux 缓存 + 暗光状态机。
 *
 * 暗光状态机：lux < 阈值 持续 防抖时长 → 触发 dark 回调（冷却期内不重复）。
 * 回调由 UI 层注册（ui_home.c 弹窗）；阈值/防抖/冷却为宏，可按需配置化。
 ****************************************************************************/
#ifndef DM_ALS_H
#define DM_ALS_H

#ifdef __cplusplus
extern "C" {
#endif

/* 暗光参数（2026-09-14 降误报：仅 0 lux + 真人 + 更长防抖 + 退出迟滞） */
#define DM_ALS_DARK_LUX        1       /* 暗光阈值 lux（<1 即 ==0 lux 才触发） */
#define DM_ALS_DARK_EXIT_LUX   8       /* 退出迟滞：lux ≥ 此值才判"光线恢复"并复位
                                        * 状态机；1~7 lux 灰区保持当前状态，
                                        * 避免传感器在阈值边缘抖动导致反复重弹 */
#define DM_ALS_DARK_DEBOUNCE_MS 120000 /* 连续低于阈值 2 分钟才触发 */
#define DM_ALS_DARK_COOLDOWN_MS 0       /* 无冷却：每次恢复再暗都弹窗 */
#define DM_ALS_POLL_MS         1000    /* 轮询周期（与驱动 ALS 出数节奏匹配） */
#define DM_ALS_STARTUP_MS      20000   /* 开机稳定期：此期间不触发暗光弹窗
                                        * （P156：驱动首个有效转换前可能推 0，
                                        *  开机即弹"知道了"是误报） */
#define DM_ALS_CONFIRM_LUX     200     /* P157：确认阈值 lux——只有见过真实亮度
                                        * （≥200，室内正常照明档）后，低亮度读数
                                        * 才可信 → 暗光检测才启用；传感器若从头
                                        * 到尾只报 0（坏数据/未就绪/遮光），永远
                                        * 无法确认是"真暗"，不弹窗 */

typedef void (*dm_als_dark_cb_t)(void);

/* P180：自动亮度 cfg 键（/data/dm_health.cfg，0/1） */
#define DM_ALS_AUTO_BRIGHT_CFG "auto_brightness"

/* 初始化：订阅 sensor_light + 启动轮询 timer + 读自动亮度开关。失败返回 -1（无传感器不打扰）。 */
int  dm_als_init(void);

/* 当前环境光 lux（int 取整；未初始化/无数据返回 -1） */
int  dm_als_get_lux(void);

/* P180：自动亮度查询/设置（UI 开关回调调用；设置会落内存 + 引擎即刻响应） */
bool dm_als_auto_enabled(void);
void dm_als_set_auto_bright(bool on);

/* 注册暗光触发回调（仅一个，UI 层注册；不注册则只统计不弹窗） */
void dm_als_set_dark_cb(dm_als_dark_cb_t cb);

/* 释放 timer 和 uORB 订阅 fd */
void dm_als_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* DM_ALS_H */
