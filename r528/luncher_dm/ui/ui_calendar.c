/*
 * ui_calendar.c — 日历子页【2026-09-10 P206 建 + P210 升级 + 09-11 重构】
 *
 * 顶：当前视图年月（双击回今天）；底：选中日详情（月日/周几/农历/纪念日）。
 * 中间原生滚动容器装「前/本/后」三页 6×7 格（周一起始），
 * SCROLL_SNAP_START + SCROLL_ONE + MOMENTUM → 抄 Settings 的
 * 拖拽跟手 + 惯性 + 松手吸附整月手感；滚过一页 SCROLL_END 归位换月，
 * 无缝无限翻月（上滑→下月）。格子纵向堆叠：日期(36px)/农历(30px)。
 * 极简配色：日期默认深色，仅今日蓝底白字 / 选中淡蓝底蓝字 / 传统节日红字；
 * 纪念日格右上角小橙点。点邻月灰格=滚到那页，长按本月格增删纪念日。
 * 午夜跨天 30s timer 自刷（close_subpage 经 ui_calendar_close_cleanup 停）。
 *
 * 时间源与状态栏时钟同源（gmtime + 手动 +8，东八区，见 ui_home.c）。
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <lvgl/lvgl.h>

#include "deskmate_ui.h"

/* ================================================================
 * 日期逻辑（纯函数，与 UI 无关）
 * ================================================================ */

static int cal_year, cal_mon;      /* 当前视图年月 */
static int cal_sel_y, cal_sel_m, cal_sel_d;  /* 选中日完整日期（d=0 未选） */
static int cal_today_y, cal_today_m, cal_today_d;

/* 视图年份钳位（农历/节气只支持 1900~2099，翻过界钳住） */
static void cal_clamp(void)
{
    if (cal_year < 1900) { cal_year = 1900; cal_mon = 1; }
    if (cal_year > 2099) { cal_year = 2099; cal_mon = 12; }
}

static bool cal_leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int cal_dim(int y, int m)
{
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m == 2 && cal_leap(y))
        return 29;
    return d[m - 1];
}

/* 某月1号周几（0=周日），mktime 归一化（与 ui_home 时钟同款 +8 已在外层做） */
static int cal_first_wday(int y, int m)
{
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = y - 1900;
    tm.tm_mon  = m - 1;
    tm.tm_mday = 1;
    tm.tm_hour = 12;
    mktime(&tm);
    return tm.tm_wday;
}

/* 板端今天（东八区）：gmtime + 8h + mktime 归一化（ui_home.c 同款） */
static void cal_today(int *y, int *m, int *d)
{
    time_t now;
    struct tm tm;
    time(&now);
    {
        struct tm *utc = gmtime(&now);
        if (utc == NULL) {
            *y = 2026; *m = 9; *d = 10;
            return;
        }
        tm = *utc;
        tm.tm_hour += 8;
        mktime(&tm);
    }
    *y = tm.tm_year + 1900;
    *m = tm.tm_mon + 1;
    *d = tm.tm_mday;
}

/* ================================================================
 * 农历（公有天文数据表 1900-2099 + 自写换算，与 status/weather 无关，
 *  纯显示；超范围隐藏农历不崩）
 * ================================================================ */

static const unsigned LUNAR_INFO[] = {
    0x04bd8,0x04ae0,0x0a570,0x054d5,0x0d260,0x0d950,0x16554,0x056a0,0x09ad0,0x055d2,
    0x04ae0,0x0a5b6,0x0a4d0,0x0d250,0x1d255,0x0b540,0x0d6a0,0x0ada2,0x095b0,0x14977,
    0x04970,0x0a4b0,0x0b4b5,0x06a50,0x06d40,0x1ab54,0x02b60,0x09570,0x052f2,0x04970,
    0x06566,0x0d4a0,0x0ea50,0x06e95,0x05ad0,0x02b60,0x186e3,0x092e0,0x1c8d7,0x0c950,
    0x0d4a0,0x1d8a6,0x0b550,0x056a0,0x1a5b4,0x025d0,0x092d0,0x0d2b2,0x0a950,0x0b557,
    0x06ca0,0x0b550,0x15355,0x04da0,0x0a5d0,0x14573,0x052b0,0x0a9a8,0x0e950,0x06aa0,
    0x0aea6,0x0ab50,0x04b60,0x0aae4,0x0a570,0x05260,0x0f263,0x0d950,0x05b57,0x056a0,
    0x096d0,0x04dd5,0x04ad0,0x0a4d0,0x0d4d4,0x0d250,0x0d558,0x0b540,0x0b5a0,0x195a6,
    0x095b0,0x049b0,0x0a974,0x0a4b0,0x0b27a,0x06a50,0x06d40,0x0af46,0x0ab60,0x09570,
    0x04af5,0x04970,0x064b0,0x074a3,0x0ea50,0x06b58,0x055c0,0x0ab60,0x096d5,0x092e0,
    0x0c960,0x0d954,0x0d4a0,0x0da50,0x07552,0x056a0,0x0abb7,0x025d0,0x092d0,0x0cab5,
    0x0a950,0x0b4a0,0x0baa4,0x0ad50,0x055d9,0x04ba0,0x0a5b0,0x15176,0x052b0,0x0a930,
    0x07954,0x06aa0,0x0ad50,0x05b52,0x04b60,0x0a6e6,0x0a4e0,0x0d260,0x0ea65,0x0d530,
    0x05aa0,0x076a3,0x096d0,0x04afb,0x04ad0,0x0a4d0,0x1d0b6,0x0d250,0x0d520,0x0dd45,
    0x0b5a0,0x056d0,0x055b2,0x049b0,0x0a577,0x0a4b0,0x0aa50,0x1b255,0x06d20,0x0ada0,
    0x14b63,0x09370,0x049f8,0x04970,0x064b0,0x168a6,0x0ea50,0x06aa0,0x1a6c4,0x0aae0,
    0x092e0,0x0d2e3,0x0c960,0x0d557,0x0d4a0,0x0da50,0x05d55,0x056a0,0x0a6d0,0x055d4,
    0x052d0,0x0a9b8,0x0a950,0x0b4a0,0x0b6a6,0x0ad50,0x055a0,0x0aba4,0x0a5b0,0x052b0,
    0x0b273,0x06930,0x07337,0x06aa0,0x0ad50,0x14b55,0x04b60,0x0a570,0x054e4,0x0d160,
    0x0e968,0x0d520,0x0daa0,0x16aa6,0x056d0,0x04ae0,0x0a9d4,0x0a2d0,0x0d150,0x0f252,
    0x0d520,
};
#define LUNAR_BASE_Y   1900

/* 公历转天数（Howard Hinnant days_from_civil 思想，自写） */
static long cal_dayno(int y, int m, int d)
{
    long days = 0;
    int yy;
    static const int dm[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    for (yy = 1900; yy < y; yy++)
        days += 365 + (cal_leap(yy) ? 1 : 0);
    {
        int i;
        for (i = 1; i < m; i++) {
            days += dm[i - 1];
            if (i == 2 && cal_leap(y))
                days++;
        }
    }
    return days + d - 1;   /* 1900-01-01 = 0 */
}

/* 农历月天数（m=1..12 非闰；is_leap=1 取闰月天数） */
static int lunar_mdays(unsigned info, int m, int is_leap)
{
    if (is_leap)
        return (info & 0x10000) ? 30 : 29;
    return (info & (0x10000u >> (unsigned)m)) ? 30 : 29;
}

/* 公历→农历：ly/lm/ld 输出，leap=1 闰月；超范围返回 -1 */
static int solar_to_lunar(int sy, int sm, int sd,
                          int *ly, int *lm, int *ld, int *leap)
{
    long off;
    int y;

    if (sy < 1900 || sy > 2099)
        return -1;
    off = cal_dayno(sy, sm, sd) - cal_dayno(1900, 1, 31);
    if (off < 0)
        return -1;
    for (y = 1900; y <= 2099; y++) {
        unsigned info = LUNAR_INFO[y - 1900];
        int lpm = (int)(info & 0xF);
        int i;
        for (i = 1; i <= 12; i++) {
            int dd = lunar_mdays(info, i, 0);
            if (off < dd) {
                *ly = y; *lm = i; *ld = (int)off + 1; *leap = 0;
                return 0;
            }
            off -= dd;
            if (i == lpm) {
                dd = lunar_mdays(info, i, 1);
                if (off < dd) {
                    *ly = y; *lm = i; *ld = (int)off + 1; *leap = 1;
                    return 0;
                }
                off -= dd;
            }
        }
    }
    return -1;
}

static const char *lunar_month_cn(int m)
{
    static const char *n[] = { "", "正月", "二月", "三月", "四月", "五月",
                               "六月", "七月", "八月", "九月", "十月",
                               "冬月", "腊月" };
    return (m >= 1 && m <= 12) ? n[m] : "";
}

static const char *lunar_day_cn(int d)
{
    static const char *n[] = {
        "", "初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八",
        "初九", "初十", "十一", "十二", "十三", "十四", "十五", "十六",
        "十七", "十八", "十九", "二十", "廿一", "廿二", "廿三", "廿四",
        "廿五", "廿六", "廿七", "廿八", "廿九", "三十" };
    return (d >= 1 && d <= 30) ? n[d] : "";
}

/* 格子农历文案：节日优先，否则 初一显月名，否则日子；闰月加"闰" */
static void lunar_cell_text(int sy, int sm, int sd, char *out, size_t cap,
                            int *is_fest)
{
    int ly, lm, ld, leap;
    const char *fest = NULL;

    *is_fest = 0;
    if (solar_to_lunar(sy, sm, sd, &ly, &lm, &ld, &leap) != 0) {
        out[0] = '\0';
        return;
    }
    /* 公历节日 */
    if (sm == 1 && sd == 1) fest = "元旦";
    else if (sm == 5 && sd == 1) fest = "劳动";
    else if (sm == 10 && sd == 1) fest = "国庆";
    /* 农历节日 */
    else if (!leap && lm == 1 && ld == 1) fest = "春节";
    else if (!leap && lm == 1 && ld == 15) fest = "元宵";
    else if (!leap && lm == 5 && ld == 5) fest = "端午";
    else if (!leap && lm == 7 && ld == 7) fest = "七夕";
    else if (!leap && lm == 8 && ld == 15) fest = "中秋";
    else if (!leap && lm == 9 && ld == 9) fest = "重阳";
    else if (!leap && lm == 12 && ld == 8) fest = "腊八";
    else if (!leap && lm == 12) {
        /* 除夕 = 腊月最后一天 */
        unsigned info = LUNAR_INFO[ly - 1900];
        int lpm = (int)(info & 0xF);
        int last = (lpm == 12) ? lunar_mdays(info, 12, 1)
                               : lunar_mdays(info, 12, 0);
        if (ld == last) fest = "除夕";
    }
    if (fest) {
        snprintf(out, cap, "%s", fest);
        *is_fest = 1;
        return;
    }
    if (ld == 1)
        snprintf(out, cap, "%s%s", leap ? "闰" : "", lunar_month_cn(lm));
    else
        snprintf(out, cap, "%s%s", leap ? "闰" : "", lunar_day_cn(ld));
}

/* ================================================================
 * 节气（低精度太阳黄经法，零数据表）
 * 交节时刻 = 太阳黄经达到 315+15k；取北京日终点（UT16h）黄经
 * 首达目标之日。107 个紫金山真值点全中（1998~2036，含 23:51
 * 交节/闰年偏移/2000年大寒21日）。模型误差约±15分钟，午夜
 * 前后15分钟内交节有±1天极限误差（百年一遇量级）。
 * 一年扫描 365 天三角函数，进页/年切换算一次缓存（A7 <5ms）。
 * ================================================================ */

static const char *g_jieqi_name[24] = {
    "小寒", "大寒", "立春", "雨水", "惊蛰", "春分",
    "清明", "谷雨", "立夏", "小满", "芒种", "夏至",
    "小暑", "大暑", "立秋", "处暑", "白露", "秋分",
    "寒露", "霜降", "立冬", "小雪", "大雪", "冬至",
};
/* 各节气目标黄经（绝对值，小寒 285° 起） */
static const int g_jieqi_abs[24] = {
    285, 300, 315, 330, 345, 360,
    375, 390, 405, 420, 435, 450,
    465, 480, 495, 510, 525, 540,
    555, 570, 585, 600, 615, 630,
};
static int g_jq_year;               /* 缓存年份（0=无效） */
static signed char g_jq_mon[24];    /* 各节气月份 1..12 */
static signed char g_jq_day[24];    /* 各节气日期 1..31 */

/* 某日终点太阳真黄经（0~360°，Schlyter 低精度，C89 double，无库依赖除 libm） */
static double cal_sun_lon(int y, int m, int d, double uth)
{
    double dd = 367.0 * y - 7.0 * ((y + (m + 9) / 12) / 4)
              + 275.0 * m / 9.0 + (double)d - 730530.0 + uth / 24.0;
    double w = 282.9404 + 4.70935e-5 * dd;
    double e = 0.016709 - 1.151e-9 * dd;
    double ms = 356.0470 + 0.9856002585 * dd;
    double E, x, yy, v;
    const double PI = 3.14159265358979323846;

    ms = ms - floor(ms / 360.0) * 360.0;   /* fmod 对负数语义不定，用 floor 版 */
    ms = ms * (PI / 180.0);
    E = ms + e * sin(ms) * (1.0 + e * cos(ms));
    E = ms + e * sin(E) * (1.0 + e * cos(E));
    x = cos(E) - e;
    yy = sin(E) * sqrt(1.0 - e * e);
    v = atan2(yy, x) * (180.0 / PI) + w;
    while (v < 0.0)
        v += 360.0;
    while (v >= 360.0)
        v -= 360.0;
    return v;
}

/* 扫描全年，填某年 24 节气月日缓存 */
static void cal_jieqi_build(int y)
{
    double lons[366];
    unsigned char mons[366], dys[366];
    int n = 0, m, d, i, k;
    double prev = 0.0, base = 0.0;

    for (m = 1; m <= 12; m++) {
        int dim = cal_dim(y, m);
        for (d = 1; d <= dim; d++) {
            /* 北京日终点 = UT16h：终点黄经首达目标之日即交节日 */
            double lon = cal_sun_lon(y, m, d, 16.0);
            if (n > 0 && lon < prev - 180.0)
                base += 360.0;   /* 过春分点解卷绕 */
            prev = lon;
            lons[n] = lon + base;
            mons[n] = (unsigned char)m;
            dys[n] = (unsigned char)d;
            n++;
        }
    }
    k = 0;
    for (i = 0; i < 24; i++) {
        while (k < n && lons[k] < (double)g_jieqi_abs[i])
            k++;
        if (k >= n)
            k = n - 1;
        g_jq_mon[i] = (signed char)mons[k];
        g_jq_day[i] = (signed char)dys[k];
    }
    g_jq_year = y;
}

/* 某公历日是节气返回 0..23（小寒起），否则 -1 */
static int cal_jieqi(int y, int m, int d)
{
    int i;

    if (y < 1900 || y > 2099)
        return -1;
    if (g_jq_year != y)
        cal_jieqi_build(y);
    for (i = 0; i < 24; i++)
        if (g_jq_mon[i] == m && g_jq_day[i] == d)
            return i;
    return -1;
}

/* ================================================================
 * 纪念日（/data/dm_cal.evt 纯文本 "YYYY MM DD 文字"，≤64 条，≤48B/条）
 * 有事的日子小字行首加 •；底部状态行看全文；长按本月日期增删。
 * ================================================================ */

#define CAL_EVT_FILE  "/data/dm_cal.evt"
#define CAL_EVT_TMP   "/data/dm_cal.tmp"
#define CAL_EVT_MAX   64
#define CAL_EVT_TXT   48

typedef struct {
    int y, m, d;
    char txt[CAL_EVT_TXT + 1];
} cal_evt_t;

static cal_evt_t g_evts[CAL_EVT_MAX];
static int g_evt_n;

/* 去首尾 ASCII 空白（原地，memmove 前移） */
static void cal_evt_trim(char *s)
{
    size_t n, off = 0;

    while (s[off] == ' ' || s[off] == '\t')
        off++;
    if (off > 0)
        memmove(s, s + off, strlen(s + off) + 1);
    n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\n' || s[n - 1] == '\r'))
        n--;
    s[n] = '\0';
}

/* 按字节截断不劈 UTF-8（回退到字符边界） */
static void cal_evt_utf8_trunc(char *s, size_t cap)
{
    size_t n = strlen(s);

    if (n < cap)
        return;
    n = cap - 1;
    while (n > 0 && (s[n] & 0xC0) == 0x80)
        n--;
    s[n] = '\0';
}

static void cal_evt_save(void)
{
    FILE *f = fopen(CAL_EVT_TMP, "w");
    int i;

    if (!f)
        return;
    for (i = 0; i < g_evt_n; i++)
        fprintf(f, "%04d %02d %02d %s\n",
                g_evts[i].y, g_evts[i].m, g_evts[i].d, g_evts[i].txt);
    fclose(f);
    rename(CAL_EVT_TMP, CAL_EVT_FILE);   /* 原子替换，掉电不坏旧档 */
}

static void cal_evt_load(void)
{
    FILE *f = fopen(CAL_EVT_FILE, "r");
    char line[128];

    g_evt_n = 0;
    if (!f)
        return;
    while (g_evt_n < CAL_EVT_MAX) {
        char *p = line;
        int v[3], k;

        if (!fgets(line, sizeof(line), f))
            break;
        /* 手工解析 3 个整数（sscanf %n 在部分 libc 缺失，不用） */
        for (k = 0; k < 3; k++) {
            while (*p == ' ' || *p == '\t')
                p++;
            if (*p < '0' || *p > '9')
                break;
            v[k] = 0;
            while (*p >= '0' && *p <= '9')
                v[k] = v[k] * 10 + (*p++ - '0');
        }
        if (k != 3)
            continue;
        while (*p == ' ' || *p == '\t')
            p++;
        {
            size_t tn = strlen(p);
            while (tn > 0 && (p[tn - 1] == '\n' || p[tn - 1] == '\r'))
                p[--tn] = '\0';
        }
        cal_evt_trim(p);
        if (v[0] < 1900 || v[0] > 2099 || v[1] < 1 || v[1] > 12 ||
            v[2] < 1 || v[2] > cal_dim(v[0], v[1]) || p[0] == '\0')
            continue;
        cal_evt_utf8_trunc(p, sizeof(g_evts[0].txt));
        g_evts[g_evt_n].y = v[0];
        g_evts[g_evt_n].m = v[1];
        g_evts[g_evt_n].d = v[2];
        /* p 已截到 <49B，strcpy 安全（snprintf 会报截断误告） */
        strcpy(g_evts[g_evt_n].txt, p);
        g_evt_n++;
    }
    fclose(f);
}

/* 加纪念日：满返回 -1，否则返回序号 */
static int cal_evt_add(int y, int m, int d, const char *txt)
{
    char tmp[CAL_EVT_TXT + 1];

    if (g_evt_n >= CAL_EVT_MAX)
        return -1;
    if (y < 1900 || y > 2099 || m < 1 || m > 12 || d < 1 ||
        d > cal_dim(y, m) || !txt)
        return -1;
    snprintf(tmp, sizeof(tmp), "%s", txt);
    cal_evt_trim(tmp);
    if (tmp[0] == '\0')
        return -1;
    cal_evt_utf8_trunc(tmp, sizeof(tmp));
    g_evts[g_evt_n].y = y;
    g_evts[g_evt_n].m = m;
    g_evts[g_evt_n].d = d;
    snprintf(g_evts[g_evt_n].txt, sizeof(g_evts[g_evt_n].txt), "%s", tmp);
    g_evt_n++;
    cal_evt_save();
    return g_evt_n - 1;
}

static void cal_evt_del(int idx)
{
    int i;

    if (idx < 0 || idx >= g_evt_n)
        return;
    for (i = idx; i + 1 < g_evt_n; i++)
        g_evts[i] = g_evts[i + 1];
    g_evt_n--;
    cal_evt_save();
}

/* 当天条数；first 取首条文字（无事返回 0，first 置空） */
static int cal_evt_day(int y, int m, int d, char *first, size_t cap)
{
    int i, n = 0;

    if (first && cap > 0)
        first[0] = '\0';
    for (i = 0; i < g_evt_n; i++) {
        if (g_evts[i].y == y && g_evts[i].m == m && g_evts[i].d == d) {
            if (n == 0 && first && cap > 0)
                snprintf(first, cap, "%s", g_evts[i].txt);
            n++;
        }
    }
    return n;
}

/* ================================================================
 * UI
 * ================================================================ */

/* 三页吸附滚动：页高 = 6 行格高 + 5 行距 */
#define CAL_CELL_H   DM(42)
#define CAL_ROW_GAP  DM(2)
#define CAL_PAGE_H   (6 * CAL_CELL_H + 5 * CAL_ROW_GAP)

static lv_obj_t *g_cal_title_lbl;              /* 顶：当前视图 "2026年10月" */
static lv_obj_t *g_cal_detail_lbl;             /* 底：选中日详情 */
static lv_obj_t *g_cal_scroll;                 /* 原生滚动容器（三页吸附） */
static lv_obj_t *g_cal_day_lbl[3][42];         /* 格内日期数字 */
static lv_obj_t *g_cal_lunar_lbl[3][42];       /* 格内农历/节日小字 */
static lv_obj_t *g_cal_evt_dot[3][42];         /* 纪念日小橙点 */
static bool      s_recentering;                /* 程序化归位中，忽略 SCROLL_END */

static const char *cal_wday_cn(int w)
{
    static const char *n[] = { "周日", "周一", "周二", "周三",
                               "周四", "周五", "周六" };
    return (w >= 0 && w < 7) ? n[w] : "";
}

/* 某日周几（0=周日） */
static int cal_wday_of(int y, int m, int d)
{
    if (d < 1)
        d = 1;
    return (cal_first_wday(y, m) + d - 1) % 7;
}

/* pi=0/1/2 → 视图前一月 / 当月 / 后一月 */
static void cal_page_ym(int pi, int *y, int *m)
{
    int yy = cal_year, mm = cal_mon + (pi - 1);

    while (mm < 1)  { mm += 12; yy--; }
    while (mm > 12) { mm -= 12; yy++; }
    *y = yy;
    *m = mm;
}

/* 渲染一页（pi 的 42 格文字/样式/小橙点） */
static void cal_render_page(int pi)
{
    int y, m, wd, dim, pdim, j;

    cal_page_ym(pi, &y, &m);
    wd = (cal_first_wday(y, m) + 6) % 7;   /* 周一起始偏移 */
    dim = cal_dim(y, m);
    pdim = cal_dim(m == 1 ? y - 1 : y, m == 1 ? 12 : m - 1);

    for (j = 0; j < 42; j++) {
        int cell = j - wd;
        int other = 0, dnum, sy, sm, sd;
        int is_today = 0, is_sel = 0, is_fest = 0, jq;
        char buf[16], lunar[16];

        if (cell < 0) {
            dnum = pdim + cell + 1;
            other = 1;
            if (m == 1) { sy = y - 1; sm = 12; } else { sy = y; sm = m - 1; }
            sd = dnum;
        } else if (cell >= dim) {
            dnum = cell - dim + 1;
            other = 1;
            if (m == 12) { sy = y + 1; sm = 1; } else { sy = y; sm = m + 1; }
            sd = dnum;
        } else {
            dnum = cell + 1;
            sy = y; sm = m; sd = dnum;
        }
        if (!other && sy == cal_today_y && sm == cal_today_m
            && sd == cal_today_d)
            is_today = 1;
        if (!other && cal_sel_d > 0 && sy == cal_sel_y && sm == cal_sel_m
            && sd == cal_sel_d)
            is_sel = 1;

        snprintf(buf, sizeof(buf), "%d", dnum);
        lv_label_set_text(g_cal_day_lbl[pi][j], buf);

        /* 极简配色：日期默认深色；今日/选中统一固定圆圈 DM(24)，
         * 数字居中（个位十位一样大，iOS 式） */
        lv_obj_set_size(g_cal_day_lbl[pi][j],
                        LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(g_cal_day_lbl[pi][j], LV_OPA_0, 0);
        lv_obj_set_style_pad_all(g_cal_day_lbl[pi][j], 0, 0);
        lv_obj_set_style_radius(g_cal_day_lbl[pi][j], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_text_align(g_cal_day_lbl[pi][j],
                                    LV_TEXT_ALIGN_CENTER, 0);
        if (is_today) {
            lv_obj_set_size(g_cal_day_lbl[pi][j], DM(24), DM(24));
            lv_obj_set_style_bg_color(g_cal_day_lbl[pi][j],
                                      lv_color_hex(COL_BLUE), 0);
            lv_obj_set_style_bg_opa(g_cal_day_lbl[pi][j], LV_OPA_COVER, 0);
            lv_obj_set_style_pad_top(g_cal_day_lbl[pi][j], DM(4), 0);
            lv_obj_set_style_text_color(g_cal_day_lbl[pi][j],
                                        lv_color_hex(0xFFFFFF), 0);
        } else if (is_sel) {
            lv_obj_set_size(g_cal_day_lbl[pi][j], DM(24), DM(24));
            lv_obj_set_style_bg_color(g_cal_day_lbl[pi][j],
                                      lv_color_hex(COL_BLUE), 0);
            lv_obj_set_style_bg_opa(g_cal_day_lbl[pi][j], LV_OPA_20, 0);
            lv_obj_set_style_pad_top(g_cal_day_lbl[pi][j], DM(4), 0);
            lv_obj_set_style_text_color(g_cal_day_lbl[pi][j],
                                        lv_color_hex(COL_BLUE), 0);
        } else {
            lv_obj_set_style_text_color(g_cal_day_lbl[pi][j],
                lv_color_hex(other ? COL_SEC : COL_TEXT), 0);
        }

        /* 小字：节日(红) > 节气(灰) > 农历(灰)；灰格整体降级 */
        lunar_cell_text(sy, sm, sd, lunar, sizeof(lunar), &is_fest);
        if (!is_fest) {
            jq = cal_jieqi(sy, sm, sd);
            if (jq >= 0)
                snprintf(lunar, sizeof(lunar), "%s", g_jieqi_name[jq]);
        }
        lv_label_set_text(g_cal_lunar_lbl[pi][j], lunar);
        {
            int lcol = (is_fest && !other) ? COL_RED : COL_SEC;
            lv_obj_set_style_text_color(g_cal_lunar_lbl[pi][j],
                                        lv_color_hex(lcol), 0);
        }

        /* 纪念日小橙点（格右上角） */
        if (cal_evt_day(sy, sm, sd, NULL, 0) > 0)
            lv_obj_clear_flag(g_cal_evt_dot[pi][j], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(g_cal_evt_dot[pi][j], LV_OBJ_FLAG_HIDDEN);
    }
}

/* 顶（视图年月）+ 底（选中日详情） */
static void cal_refresh_header(void)
{
    char buf[128];

    if (g_cal_title_lbl) {
        snprintf(buf, sizeof(buf), "%d年%d月", cal_year, cal_mon);
        lv_label_set_text(g_cal_title_lbl, buf);
    }
    if (g_cal_detail_lbl) {
        if (cal_sel_d > 0) {
            char lunar[16], daytxt[16], first[CAL_EVT_TXT + 1];
            int dummy = 0, evn, jq, w;

            w = cal_wday_of(cal_sel_y, cal_sel_m, cal_sel_d);
            lunar_cell_text(cal_sel_y, cal_sel_m, cal_sel_d,
                            lunar, sizeof(lunar), &dummy);
            if (dummy) {
                snprintf(daytxt, sizeof(daytxt), "%s", lunar);
            } else if ((jq = cal_jieqi(cal_sel_y, cal_sel_m,
                                       cal_sel_d)) >= 0) {
                snprintf(daytxt, sizeof(daytxt), "%s", g_jieqi_name[jq]);
            } else {
                snprintf(daytxt, sizeof(daytxt), "%s", lunar);
            }
            snprintf(buf, sizeof(buf), "%d月%d日 %s%s%s",
                     cal_sel_m, cal_sel_d, cal_wday_cn(w),
                     daytxt[0] ? " · " : "", daytxt);
            evn = cal_evt_day(cal_sel_y, cal_sel_m, cal_sel_d,
                              first, sizeof(first));
            if (evn == 1) {
                size_t n = strlen(buf);
                snprintf(buf + n, sizeof(buf) - n, " · %s", first);
            } else if (evn > 1) {
                size_t n = strlen(buf);
                snprintf(buf + n, sizeof(buf) - n, " · %s等%d件事",
                         first, evn);
            }
            lv_label_set_text(g_cal_detail_lbl, buf);
        } else {
            lv_label_set_text(g_cal_detail_lbl,
                              "上滑看下月 · 点日期看当天 · 长按添加纪念日");
        }
    }
}

static void cal_refresh(void)
{
    int pi;

    for (pi = 0; pi < 3; pi++)
        cal_render_page(pi);
    cal_refresh_header();
}

/* 前置声明（纪念日浮层开着时点选不跳动） */
static int cal_evt_is_open(void);

/* 整月步进（滚动归位用；选中日由 cal_rotate 按归属清理） */
static void cal_month_math(int delta)
{
    cal_mon += delta;
    while (cal_mon < 1) { cal_mon += 12; cal_year--; }
    while (cal_mon > 12) { cal_mon -= 12; cal_year++; }
    cal_clamp();
}

/* 整页归位：滚过一页后把中页重建为“新月”，瞬间回到中页（无缝）。
 * 顺序关键：先渲染中页（= 用户当前所见那页）再瞬间归位，避免闪帧。 */
static void cal_rotate(int idx)
{
    cal_month_math(idx == 2 ? +1 : -1);
    cal_render_page(1);
    lv_obj_update_layout(g_cal_scroll);   /* 确保目标页坐标已算 */
    lv_obj_scroll_to_y(g_cal_scroll, CAL_PAGE_H, LV_ANIM_OFF);
    cal_render_page(0);
    cal_render_page(2);
    if (cal_sel_d > 0 &&
        !(cal_sel_y == cal_year && cal_sel_m == cal_mon))
        cal_sel_d = 0;      /* 浏览翻月：清掉不在当前视图的选中 */
    cal_refresh_header();
}

/* 原生滚动停止 → 最近页不是中页则归位换月（= Settings 惯性吸附手感）。
 * 注意：带吸附的松手会先发一次 END（吸附动画仍在跑），用 lv_anim_get
 * 过滤，只处理动画真正结束的那次（此时 anim 已从链表移除）。 */
static void cal_scroll_end_cb(lv_event_t *e)
{
    int sy, idx;

    (void)e;
    if (s_recentering || !g_cal_scroll)
        return;
    if (lv_anim_get(g_cal_scroll, NULL) != NULL)
        return;
    reset_idle_timer();
    sy = lv_obj_get_scroll_y(g_cal_scroll);
    idx = (sy + CAL_PAGE_H / 2) / CAL_PAGE_H;
    if (idx == 1)
        return;
    if (idx < 0)
        idx = 0;
    else if (idx > 2)
        idx = 2;
    s_recentering = true;
    cal_rotate(idx);
    s_recentering = false;
}

/* 回到今天 + 本月（顶行双击触发；也可被其它入口复用） */
static void cal_goto_today(void)
{
    int y, m, d;

    cal_today(&y, &m, &d);
    cal_today_y = y; cal_today_m = m; cal_today_d = d;
    cal_year = y; cal_mon = m;
    cal_sel_y = y; cal_sel_m = m; cal_sel_d = d;
    cal_refresh();
    if (g_cal_scroll) {
        lv_obj_update_layout(g_cal_scroll);
        s_recentering = true;
        lv_obj_scroll_to_y(g_cal_scroll, CAL_PAGE_H, LV_ANIM_OFF);
        s_recentering = false;
    }
}

/* 顶行点按：400ms 内两下 = 双击回今天（本版 LVGL 无 DOUBLE_CLICKED 事件） */
static uint32_t s_title_last_ms;

static void cal_title_cb(lv_event_t *e)
{
    uint32_t now;

    (void)e;
    reset_idle_timer();
    now = lv_tick_get();
    if (now - s_title_last_ms < 400) {
        s_title_last_ms = 0;
        cal_goto_today();
    } else {
        s_title_last_ms = now;
    }
}

/* 点格：本月格=选中看详情；邻月灰格=滚到那页（松手后归位换月） */
static void cal_day_cb(lv_event_t *e)
{
    intptr_t v = (intptr_t)lv_event_get_user_data(e);
    int pi = (int)(v / 100), j = (int)(v % 100);
    int y, m, wd, dim, cell, sy = 0, sm = 0, sd = 0, other = 0;

    reset_idle_timer();
    if (cal_evt_is_open())
        return;

    cal_page_ym(pi, &y, &m);
    wd = (cal_first_wday(y, m) + 6) % 7;
    dim = cal_dim(y, m);
    cell = j - wd;

    if (cell < 0) {
        int pdim = cal_dim(m == 1 ? y - 1 : y, m == 1 ? 12 : m - 1);
        sd = pdim + cell + 1;
        if (m == 1) { sy = y - 1; sm = 12; } else { sy = y; sm = m - 1; }
        other = 1;
    } else if (cell >= dim) {
        sd = cell - dim + 1;
        if (m == 12) { sy = y + 1; sm = 1; } else { sy = y; sm = m + 1; }
        other = 1;
    } else {
        sy = y; sm = m; sd = cell + 1;
    }

    cal_sel_y = sy; cal_sel_m = sm; cal_sel_d = sd;
    if (other) {
        /* 邻月灰格：滑到对应页（SCROLL_END 归位换月，选中保留） */
        if (g_cal_scroll) {
            s_recentering = false;
            if (cell < 0)
                lv_obj_scroll_to_y(g_cal_scroll, 0, LV_ANIM_ON);
            else
                lv_obj_scroll_to_y(g_cal_scroll, 2 * CAL_PAGE_H, LV_ANIM_ON);
        }
    } else {
        cal_refresh();   /* 选中高亮 + 底部详情 */
    }
}

/* ================================================================
 * 纪念日浮层（长按本月日期打开：已有列表+删除，输入框+键盘添加）
 * ================================================================ */

static lv_obj_t *g_evt_overlay;
static lv_obj_t *g_evt_ta;
static dm_kb_state_t *g_evt_kb;
static lv_obj_t *g_evt_list;
static lv_obj_t *g_evt_title;
static int g_evt_y, g_evt_m, g_evt_d;

static int cal_evt_is_open(void)
{
    return g_evt_overlay && lv_obj_is_valid(g_evt_overlay);
}

static void cal_evt_close(void)
{
    if (g_evt_kb) {
        dm_kb_destroy(g_evt_kb);
        g_evt_kb = NULL;
    }
    if (cal_evt_is_open())
        lv_obj_del(g_evt_overlay);
    g_evt_overlay = NULL;
    g_evt_ta = NULL;
    g_evt_list = NULL;
    g_evt_title = NULL;
}

static void cal_evt_rebuild_list(void);   /* 先声明（删除回调在前） */

static void cal_evt_del_cb(lv_event_t *e)
{
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);

    reset_idle_timer();
    cal_evt_del((int)idx);
    /* 原地重建列表 + 网格 • 刷新 */
    cal_evt_rebuild_list();
    cal_refresh();
}

static void cal_evt_rebuild_list(void)
{
    int i;

    if (!g_evt_list || !lv_obj_is_valid(g_evt_list))
        return;
    lv_obj_clean(g_evt_list);
    for (i = 0; i < g_evt_n; i++) {
        lv_obj_t *row, *tt, *del, *ic;

        if (g_evts[i].y != g_evt_y || g_evts[i].m != g_evt_m ||
            g_evts[i].d != g_evt_d)
            continue;
        row = lv_obj_create(g_evt_list);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_bg_color(row, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_60, 0);
        lv_obj_set_style_radius(row, DM(12), 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, DM(8), 0);
        lv_obj_set_style_pad_column(row, DM(8), 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        tt = lv_label_create(row);
        lv_label_set_text(tt, g_evts[i].txt);
        lv_obj_set_style_text_font(tt, FONT_BODY, 0);
        lv_obj_set_style_text_color(tt, lv_color_hex(COL_TEXT), 0);
        lv_obj_set_flex_grow(tt, 1);

        del = lv_btn_create(row);
        lv_obj_set_size(del, DM(36), DM(36));
        lv_obj_set_style_radius(del, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(del, LV_OPA_0, 0);
        lv_obj_set_style_border_width(del, 0, 0);
        lv_obj_set_style_shadow_width(del, 0, 0);
        lv_obj_add_event_cb(del, cal_evt_del_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        ic = lv_label_create(del);
        lv_label_set_text(ic, LV_SYMBOL_TRASH);
        lv_obj_set_style_text_font(ic, FONT_BODY, 0);
        lv_obj_set_style_text_color(ic, lv_color_hex(COL_SEC), 0);
        lv_obj_center(ic);
    }
    if (lv_obj_get_child_count(g_evt_list) == 0) {
        lv_obj_t *em = lv_label_create(g_evt_list);
        lv_label_set_text(em, "这天还没记事，在下面添加");
        lv_obj_set_style_text_font(em, FONT_LABEL, 0);
        lv_obj_set_style_text_color(em, lv_color_hex(COL_SEC), 0);
    }
}

/* 输入框内容存一条（保存按钮/键盘回车共用） */
static void cal_evt_do_save(void)
{
    const char *t;

    if (!g_evt_ta || !lv_obj_is_valid(g_evt_ta))
        return;
    t = lv_textarea_get_text(g_evt_ta);
    if (!t || t[0] == '\0') {
        if (g_evt_title && lv_obj_is_valid(g_evt_title))
            lv_label_set_text(g_evt_title, "先写点什么再保存");
        return;
    }
    if (cal_evt_add(g_evt_y, g_evt_m, g_evt_d, t) < 0) {
        if (g_evt_title && lv_obj_is_valid(g_evt_title))
            lv_label_set_text(g_evt_title, "纪念日已满（64个），先删一个");
        return;
    }
    lv_textarea_set_text(g_evt_ta, "");
    cal_evt_rebuild_list();
    cal_refresh();   /* 三页小橙点 + 底部详情即时刷新 */
}

static void cal_evt_ready_cb(lv_obj_t *ta, void *ud)
{
    (void)ta;
    (void)ud;
    reset_idle_timer();
    cal_evt_do_save();
}

static void cal_evt_save_cb(lv_event_t *e)
{
    (void)e;
    reset_idle_timer();
    cal_evt_do_save();
}

static void cal_evt_cancel_cb(lv_event_t *e)
{
    (void)e;
    reset_idle_timer();
    cal_evt_close();
}

static void cal_evt_long_cb(lv_event_t *e)
{
    intptr_t v = (intptr_t)lv_event_get_user_data(e);
    int pi = (int)(v / 100), j = (int)(v % 100);
    int y, m, wd, dim, cell;
    lv_obj_t *subpage, *box, *title, *ta, *brow, *cancel, *save, *l;
    char tbuf[32];

    reset_idle_timer();
    if (cal_evt_is_open())
        return;
    cal_page_ym(pi, &y, &m);
    wd = (cal_first_wday(y, m) + 6) % 7;   /* 周一起始 */
    dim = cal_dim(y, m);
    cell = j - wd;
    if (cell < 0 || cell >= dim)
        return;   /* 灰格不记事 */

    g_evt_y = y;
    g_evt_m = m;
    g_evt_d = cell + 1;
    cal_sel_y = g_evt_y;
    cal_sel_m = g_evt_m;
    cal_sel_d = g_evt_d;

    subpage = lv_obj_get_parent(lv_event_get_current_target(e));
    while (subpage && !lv_obj_has_flag(subpage, LV_OBJ_FLAG_FLOATING))
        subpage = lv_obj_get_parent(subpage);
    if (!subpage)
        return;

    g_evt_overlay = lv_obj_create(subpage);
    lv_obj_set_size(g_evt_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(g_evt_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_evt_overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(g_evt_overlay, 0, 0);
    lv_obj_set_style_pad_all(g_evt_overlay, 0, 0);
    lv_obj_clear_flag(g_evt_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_evt_overlay, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(g_evt_overlay, LV_OBJ_FLAG_CLICKABLE);

    box = lv_obj_create(g_evt_overlay);
    lv_obj_set_size(box, DM(460), LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, DM(16), 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, DM(12), 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, DM(8), 0);

    title = lv_label_create(box);
    snprintf(tbuf, sizeof(tbuf), "%d月%d日 纪念日", g_evt_m, g_evt_d);
    lv_label_set_text(title, tbuf);
    lv_obj_set_style_text_font(title, FONT_BODY, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT), 0);
    g_evt_title = title;

    g_evt_list = lv_obj_create(box);
    lv_obj_set_size(g_evt_list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g_evt_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(g_evt_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_evt_list, 0, 0);
    lv_obj_set_style_pad_all(g_evt_list, 0, 0);
    lv_obj_set_style_pad_row(g_evt_list, DM(6), 0);
    lv_obj_clear_flag(g_evt_list, LV_OBJ_FLAG_SCROLLABLE);
    cal_evt_rebuild_list();

    ta = lv_textarea_create(box);
    lv_obj_set_size(ta, LV_PCT(100), DM(44));
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, 16);   /* 48B ≈ 16 汉字 */
    lv_textarea_set_placeholder_text(ta, "写点什么（如：妈妈生日）");
    lv_obj_set_style_text_font(ta, FONT_BODY, 0);
    lv_obj_add_flag(ta, LV_OBJ_FLAG_CLICKABLE);
    g_evt_ta = ta;

    brow = lv_obj_create(box);
    lv_obj_set_size(brow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_bg_opa(brow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(brow, 0, 0);
    lv_obj_set_style_pad_all(brow, 0, 0);
    lv_obj_set_style_pad_column(brow, DM(8), 0);

    cancel = lv_btn_create(brow);
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_set_height(cancel, DM(40));
    lv_obj_set_style_radius(cancel, DM(12), 0);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0xF0F0F0), 0);
    lv_obj_set_style_border_width(cancel, 0, 0);
    lv_obj_add_event_cb(cancel, cal_evt_cancel_cb, LV_EVENT_CLICKED, NULL);
    l = lv_label_create(cancel);
    lv_label_set_text(l, "关闭");
    lv_obj_set_style_text_font(l, FONT_BODY, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_TEXT), 0);
    lv_obj_center(l);

    save = lv_btn_create(brow);
    lv_obj_set_flex_grow(save, 1);
    lv_obj_set_height(save, DM(40));
    lv_obj_set_style_radius(save, DM(12), 0);
    lv_obj_set_style_bg_color(save, lv_color_hex(COL_ORANGE), 0);
    lv_obj_set_style_border_width(save, 0, 0);
    lv_obj_add_event_cb(save, cal_evt_save_cb, LV_EVENT_CLICKED, NULL);
    l = lv_label_create(save);
    lv_label_set_text(l, "保存");
    lv_obj_set_style_text_font(l, FONT_BODY, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(l);

    g_evt_kb = dm_kb_create(g_evt_overlay, ta, cal_evt_ready_cb, NULL);
    cal_refresh();   /* 长按即选中，状态行同步 */
}

/* 午夜跨天：开页过 0 点自动刷今日高亮（30s 轮询，关页即停） */
static lv_timer_t *g_cal_mid_timer;

static void cal_mid_cb(lv_timer_t *t)
{
    int y, m, d;

    (void)t;
    cal_today(&y, &m, &d);
    if (y != cal_today_y || m != cal_today_m || d != cal_today_d) {
        cal_today_y = y;
        cal_today_m = m;
        cal_today_d = d;
        cal_refresh();
    }
}

void ui_calendar_close_cleanup(void)
{
    int pi, j;

    if (g_cal_mid_timer) {
        lv_timer_del(g_cal_mid_timer);
        g_cal_mid_timer = NULL;
    }
    cal_evt_close();
    /* 对象随 overlay 删除，这里只置空指针防野引用 */
    g_cal_scroll = NULL;
    g_cal_title_lbl = NULL;
    g_cal_detail_lbl = NULL;
    s_recentering = false;
    for (pi = 0; pi < 3; pi++) {
        for (j = 0; j < 42; j++) {
            g_cal_day_lbl[pi][j] = NULL;
            g_cal_lunar_lbl[pi][j] = NULL;
            g_cal_evt_dot[pi][j] = NULL;
        }
    }
}

void ui_calendar_create(lv_obj_t *parent)
{
    /* 一屏预算（横屏 1920x1200）：content 可用高 = 1200 - pad_top(132+115=247)
     * - 底 EDGE_PAD 96 = 857；
     * 顶行~70 + 周首~40 + 滚动页 6×DM(42)+5×DM(2)=644 + 详情~40(含下移)
     * + 间距 ≈ 825 < 857，当月 6 周全显示不滚动。日期 36px / 农历 30px。 */
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(parent, DM(4), 0);
    s_recentering = false;

    /* 顶：当前视图年月（双击回今天） */
    g_cal_title_lbl = lv_label_create(parent);
    lv_obj_set_size(g_cal_title_lbl, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(g_cal_title_lbl, FONT_TITLE, 0);
    lv_obj_set_style_text_color(g_cal_title_lbl, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_align(g_cal_title_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_bottom(g_cal_title_lbl, DM(8), 0);
    lv_obj_add_flag(g_cal_title_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_cal_title_lbl, cal_title_cb, LV_EVENT_CLICKED, NULL);
    s_title_last_ms = 0;

    /* 周首行（固定，不随月滚动；极简：只"星期日"红） */
    {
        static const char *wcn[] = { "星期一", "星期二", "星期三", "星期四",
                                     "星期五", "星期六", "星期日" };
        lv_obj_t *wrow = make_clean_cont(parent);
        lv_obj_set_size(wrow, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(wrow, LV_FLEX_FLOW_ROW);
        for (int i = 0; i < 7; i++) {
            lv_obj_t *w = lv_label_create(wrow);
            lv_label_set_text(w, wcn[i]);
            lv_obj_set_style_text_font(w, FONT_LABEL, 0);
            lv_obj_set_style_text_color(w,
                lv_color_hex(i == 6 ? COL_RED : COL_SEC), 0);
            lv_obj_set_flex_grow(w, 1);
            lv_obj_set_style_text_align(w, LV_TEXT_ALIGN_CENTER, 0);
        }
    }

    /* 原生滚动容器：三页（前/本/后月）吸附，一次一页 + 惯性（Settings 手感） */
    g_cal_scroll = lv_obj_create(parent);
    lv_obj_set_size(g_cal_scroll, lv_pct(100), CAL_PAGE_H);
    lv_obj_set_style_bg_opa(g_cal_scroll, LV_OPA_0, 0);
    lv_obj_set_style_border_width(g_cal_scroll, 0, 0);
    lv_obj_set_style_radius(g_cal_scroll, 0, 0);
    lv_obj_set_style_pad_all(g_cal_scroll, 0, 0);
    lv_obj_set_scrollbar_mode(g_cal_scroll, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(g_cal_scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g_cal_scroll, 0, 0);
    lv_obj_add_flag(g_cal_scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_cal_scroll, LV_OBJ_FLAG_SCROLL_ONE);
    lv_obj_add_flag(g_cal_scroll, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_clear_flag(g_cal_scroll, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_set_scroll_snap_y(g_cal_scroll, LV_SCROLL_SNAP_START);
    lv_obj_add_event_cb(g_cal_scroll, cal_scroll_end_cb,
                        LV_EVENT_SCROLL_END, NULL);

    /* 三页 × 6 行 × 7 格 */
    for (int pi = 0; pi < 3; pi++) {
        lv_obj_t *page = make_clean_cont(g_cal_scroll);
        lv_obj_set_size(page, lv_pct(100), CAL_PAGE_H);
        lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(page, CAL_ROW_GAP, 0);
        for (int r = 0; r < 6; r++) {
            lv_obj_t *row = make_clean_cont(page);
            lv_obj_set_size(row, lv_pct(100), CAL_CELL_H);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            for (int c = 0; c < 7; c++) {
                int j = r * 7 + c;
                /* 格子透明无框（裸字风） */
                lv_obj_t *cell = lv_btn_create(row);
                lv_obj_set_flex_grow(cell, 1);
                lv_obj_set_height(cell, CAL_CELL_H);
                lv_obj_set_style_radius(cell, 0, 0);
                lv_obj_set_style_bg_opa(cell, LV_OPA_0, 0);
                lv_obj_set_style_border_width(cell, 0, 0);
                lv_obj_set_style_shadow_width(cell, 0, 0);
                lv_obj_set_style_pad_all(cell, 0, 0);
                /* 按压态：点下去整格淡灰确认，解决"感觉没按着" */
                lv_obj_set_style_bg_color(cell, lv_color_hex(COL_SEC),
                                          LV_STATE_PRESSED);
                lv_obj_set_style_bg_opa(cell, LV_OPA_20, LV_STATE_PRESSED);
                lv_obj_set_style_radius(cell, DM(8), LV_STATE_PRESSED);
                lv_obj_add_event_cb(cell, cal_day_cb, LV_EVENT_CLICKED,
                                    (void *)(intptr_t)(pi * 100 + j));
                /* 长按本月格记纪念日（灰格由回调内过滤） */
                lv_obj_add_event_cb(cell, cal_evt_long_cb,
                                    LV_EVENT_LONG_PRESSED,
                                    (void *)(intptr_t)(pi * 100 + j));

                /* 日期大字（上）：固定圆圈 DM(24) 留顶部空间 */
                lv_obj_t *dl = lv_label_create(cell);
                lv_obj_set_style_text_font(dl, FONT_ICON, 0);
                lv_obj_align(dl, LV_ALIGN_TOP_MID, 0, DM(2));
                g_cal_day_lbl[pi][j] = dl;

                lv_obj_t *ll = lv_label_create(cell);
                lv_obj_set_style_text_font(ll, FONT_LABEL, 0);
                lv_obj_align(ll, LV_ALIGN_BOTTOM_MID, 0, -DM(1));
                g_cal_lunar_lbl[pi][j] = ll;

                /* 纪念日小橙点（格右上角，默认隐藏） */
                lv_obj_t *dot = lv_label_create(cell);
                lv_label_set_text(dot, LV_SYMBOL_BULLET);
                lv_obj_set_style_text_font(dot, FONT_CAPTION, 0);
                lv_obj_set_style_text_color(dot, lv_color_hex(COL_ORANGE), 0);
                lv_obj_align(dot, LV_ALIGN_TOP_RIGHT, -DM(5), DM(4));
                lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
                g_cal_evt_dot[pi][j] = dot;
            }
        }
    }

    /* 底：选中日详情（字号与周首行同档，下移 DM(8) 留白） */
    g_cal_detail_lbl = lv_label_create(parent);
    lv_obj_set_size(g_cal_detail_lbl, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(g_cal_detail_lbl, FONT_LABEL, 0);
    lv_obj_set_style_text_color(g_cal_detail_lbl, lv_color_hex(COL_SEC), 0);
    lv_obj_set_style_text_align(g_cal_detail_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(g_cal_detail_lbl, DM(8), 0);
    lv_label_set_long_mode(g_cal_detail_lbl, LV_LABEL_LONG_DOT);

    /* 进页回到本月今日（可预期行为） */
    {
        int y, m, d;
        cal_today(&y, &m, &d);
        cal_today_y = y;
        cal_today_m = m;
        cal_today_d = d;
        cal_year = y;
        cal_mon = m;
        cal_sel_y = y;
        cal_sel_m = m;
        cal_sel_d = d;
    }
    cal_evt_load();   /* 纪念日落盘（无文件即空，不过界） */
    if (!g_cal_mid_timer)
        g_cal_mid_timer = lv_timer_create(cal_mid_cb, 30000, NULL);
    cal_refresh();    /* 渲染三页 + 顶/底 */
    /* 初始定位到中页（当月） */
    lv_obj_update_layout(g_cal_scroll);
    s_recentering = true;
    lv_obj_scroll_to_y(g_cal_scroll, CAL_PAGE_H, LV_ANIM_OFF);
    s_recentering = false;
}
