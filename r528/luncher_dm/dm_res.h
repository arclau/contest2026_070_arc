/* dm_res.h — 资源根前缀（板端 /resource，goldfish SIM /data/data）
 *
 * SIM 宏由构建系统显式传入（-DCONFIG_LUNCHER_DM_SIM=1）：
 * <nuttx/config.h> 的 -isystem 顺序会命中源码树里的板端配置，
 * 不能依赖它读到 Kconfig 的 CONFIG_LUNCHER_DM_SIM。
 */
#ifndef __DM_RES_H
#define __DM_RES_H

#ifdef CONFIG_LUNCHER_DM_SIM
#  define DM_RES_ROOT "/data/data"
#else
#  define DM_RES_ROOT "/resource"
#endif

#endif /* __DM_RES_H */
