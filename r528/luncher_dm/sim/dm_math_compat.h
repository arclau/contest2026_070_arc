/* dm_math_compat.h — goldfish SIM 的数学函数原型补齐
 *
 * 该配置用 CONFIG_LIBM_TOOLCHAIN（工具链 libm），而 NuttX math.h 只在
 * CONFIG_LIBM / CONFIG_ARCH_MATH_H 分支导出函数原型，导致 V1 既有代码
 * 在 arm64 -Werror 下报 implicit declaration。这里只补原型，实现仍由
 * 工具链 libm 提供；板端构建不使用本文件。
 */
#ifndef __DM_MATH_COMPAT_H
#define __DM_MATH_COMPAT_H

extern double sqrt(double x);
extern double sin(double x);
extern double cos(double x);
extern double floor(double x);
extern double atan2(double y, double x);
extern float  sinf(float x);
extern float  cosf(float x);
extern float  tanf(float x);
extern float  asinf(float x);
extern float  acosf(float x);
extern float  atan2f(float y, float x);
extern float  expf(float x);
extern float  fabsf(float x);
extern float  floorf(float x);
extern float  roundf(float x);
extern float  sqrtf(float x);
extern float  powf(float x, float y);
extern float  fmodf(float x, float y);

#endif /* __DM_MATH_COMPAT_H */
