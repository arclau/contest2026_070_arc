/* SIM stub: sunxi_hal_ledc.h — goldfish/luncher_dm 用 */
#ifndef __SIM_SUNXI_HAL_LEDC_H
#define __SIM_SUNXI_HAL_LEDC_H

#ifdef __cplusplus
extern "C" {
#endif

int hal_ledc_init(void);
int hal_ledc_deinit(void);
int sunxi_set_led_brightness(int led_num, unsigned int brightness);

#ifdef __cplusplus
}
#endif
#endif
