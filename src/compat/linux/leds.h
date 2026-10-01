/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW88_COMPAT_LEDS_H
#define _RTW88_COMPAT_LEDS_H

#include "types.h"

enum led_brightness {
    LED_OFF  = 0,
    LED_ON   = 1,
    LED_HALF = 127,
    LED_FULL = 255,
};

struct led_classdev {
    const char       *name;
    const char       *default_trigger;
    enum led_brightness max_brightness;
    enum led_brightness brightness;
    void (*brightness_set)(struct led_classdev *, enum led_brightness);
    int  (*brightness_set_blocking)(struct led_classdev *, enum led_brightness);
};

struct device;
static inline int  led_classdev_register(struct device *dev, struct led_classdev *l) { return 0; }
static inline void led_classdev_unregister(struct led_classdev *l) {}
static inline void led_set_brightness(struct led_classdev *led, enum led_brightness b)
{
    led->brightness = b;
    if (led->brightness_set) led->brightness_set(led, b);
}

/* TPT (throughput) LED trigger — stub for non-LED builds */
struct ieee80211_tpt_blink {
    int throughput;
    int blink_time;
};

#define IEEE80211_TPT_LEDTRIG_FL_RADIO  (1 << 0)

struct ieee80211_hw;
static inline const char *
ieee80211_create_tpt_led_trigger(struct ieee80211_hw *hw, unsigned int flags,
                                  const struct ieee80211_tpt_blink *blink_table,
                                  unsigned int blink_table_len)
{
    return NULL;
}

#define LED_TRIGGER_NOOP_NAME "none"
static inline void led_trigger_event(void *t, enum led_brightness b) {}

#endif /* _RTW88_COMPAT_LEDS_H */
