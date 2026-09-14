/****************************************************************************
 * apps/vendor/allwinnertech/apps/luncher_dm/pet_view.h
 * 电子宠物 — 视图层（View）
 *
 * 拥有：全部 lv_obj（容器/身体/气泡/三种弹窗）+ 输入（点按/长按/连点/拖拽）
 *       + 漫游位移 + 镜像 + 动画渲染 + 气泡倒计时。
 * 数据一律经 pet_core.h 只读 getter 取；输入一律经 dm_pet_on_event() 进队列。
 * 本层不做任何状态转移/属性修改决定（那是 core 的事）。
 ****************************************************************************/

#ifndef __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_PET_VIEW_H
#define __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_PET_VIEW_H

#include <lvgl/lvgl.h>

/* 构建/销毁（含 ops 注册/注销 + 停靠右下角） */
void pet_view_create(lv_obj_t *parent);
void pet_view_destroy(void);

/* 已存在时复用：锁屏重入停靠 + 置顶（幂等 create 语义，门面调） */
bool pet_view_reuse(void);

/* 节拍（门面 timer 驱动，主线程） */
void pet_view_anim_tick(void);    /* 120ms：帧推进 + 位移 + 呼吸 */
void pet_view_bubble_tick(void);  /* 500ms：气泡隐藏倒计时 */

/* 气泡（与 ops 注册的是同一实现；门面用于长离线欢迎） */
void pet_view_show_bubble(const char *txt, int ticks);

/* 类型切换后刷新当前帧（门面调） */
void pet_view_refresh_sprite(void);

/* 漫游区覆盖（屏幕坐标；NULL/调 clear 恢复默认启发式） */
void pet_view_set_roam(int x1, int y1, int x2, int y2);
void pet_view_clear_roam(void);

#endif /* __APPS_VENDOR_ALLWINNERTECH_APPS_LUNCHER_DM_PET_VIEW_H */
