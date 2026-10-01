/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
 * CONFIG_RTW89_LEDS / CONFIG_RTW89_LEDS_MC are off; led.h provides inline stubs.
 * core.h still embeds these structs in rtw89_dev.
 */
#ifndef _RTW89_COMPAT_LED_CLASS_MULTICOLOR_H
#define _RTW89_COMPAT_LED_CLASS_MULTICOLOR_H
#include "../../compat/linux/leds.h"

struct mc_subled {
    unsigned int color_index;
    unsigned int brightness;
    unsigned int intensity;
    unsigned int channel;
};

struct led_classdev_mc {
    struct led_classdev led_cdev;
    unsigned int num_colors;
    struct mc_subled *subled_info;
};

#endif
