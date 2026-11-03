/* SIM stub: sunxi_hal_pwm.h — goldfish/luncher_dm 用 */
#ifndef __SIM_SUNXI_HAL_PWM_H
#define __SIM_SUNXI_HAL_PWM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long pwm_status_t;

typedef enum {
    PWM_POLARITY_NORMAL = 0,
    PWM_POLARITY_INVERTED
} pwm_polarity_t;

struct pwm_config {
    uint32_t period_ns;
    uint32_t duty_ns;
    pwm_polarity_t polarity;
    uint32_t ch;
    int ch_id;
};

typedef struct pwm_config pwm_config_t;

pwm_status_t hal_pwm_control(int channel, struct pwm_config *config_pwm);

#ifdef __cplusplus
}
#endif
#endif
