/****************************************************************************
 * apps/vendor/allwinnertech/apps/luncher_dm/dm_alarm.h
 * 闹钟引擎（2026-09-10 P206：桌面 OS 补闹钟，Apple 范）
 *
 * 存 /data/dm_alarm.list 纯文本（"HH MM repeat enabled"，repeat 0=一次
 * 1=每天），最多 8 个。1s timer 到分即响：锁屏上直接弹覆盖层 + 提示音，
 * 一次性响过即关，每天的每天响。再响5分钟走独立 snooze。
 * AI 定闹走 dm_cmd_alarm（见 deskmate_ui.c 本地命令表）→ dm_alarm_add。
 ****************************************************************************/

#ifndef __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_DM_ALARM_H
#define __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_DM_ALARM_H

#define DM_ALARM_MAX   8

typedef struct {
    int hh;        /* 0..23 */
    int mm;        /* 0..59 */
    int repeat;    /* 0=一次，1=每天 */
    int enabled;   /* 0/1 */
    int tone;      /* 铃声序号（dm_alarm_tone_name） */
} dm_alarm_t;

int dm_alarm_count(void);
const dm_alarm_t *dm_alarm_get(int idx);
/* 加闹钟：参数钳位；满返回 -1，否则返回序号 */
int dm_alarm_add(int hh, int mm, int repeat, int tone);
void dm_alarm_del(int idx);
void dm_alarm_set_enabled(int idx, int on);
/* 铃声表（UI/引擎共用，序号即索引） */
/* 开机调一次：读盘 + 起 1s 检查 timer（常驻，无需清理） */
void dm_alarm_init(void);
/* 响铃弹窗按"再响5分钟" */
void dm_alarm_snooze(void);

#endif /* __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_DM_ALARM_H */
