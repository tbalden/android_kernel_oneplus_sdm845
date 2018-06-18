#ifndef __NOTIFICATION_H__
#define __NOTIFICATION_H__

enum notif_type {
        NOTIF_KAD,
        NOTIF_FLASHLIGHT,
        NOTIF_VIB_REMINDER,
        NOTIF_VIB_BOOSTER,
        NOTIF_BUTTON_LIGHT,
        NOTIF_PULSE_LIGHT
};

enum notif_smart_level_type {
        NOTIF_DEFAULT, // keep as is
        NOTIF_TRIM, // trim, make less often, shorter, weaker
        NOTIF_DIM, // dim the light
        NOTIF_STOP // stop overall
};

extern void smart_set_last_user_activity_time(void);
extern int smart_get_notification_level(int notif_type);

// screen state queries
extern bool ntf_is_screen_on(void);
extern bool ntf_is_screen_early_on(void);
extern bool ntf_is_screen_early_off(void);

// charge callbacks to notify ntf - call it from battery/policy drivers
extern void ntf_set_charge_state(bool on);
extern void ntf_set_charge_level(int level);

/** add change listener */
extern void ntf_add_listener(void (*f)(char* event, int num_param, char* str_param));

#endif /* __NOTIFICATION_H__ */
