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

#define NTF_EVENT_NOTIFICATION "notification"
#define NTF_EVENT_RINGING "ringing"
#define NTF_EVENT_CHARGE_STATE "charge_state"
#define NTF_EVENT_CHARGE_LEVEL "charge_level"
#define NTF_WAKE_BY_USER "wake_by_user"

#define NTF_EVENT_NOTIFICATION_ARG_HAPTIC "haptic"

#define MIN_TD_VALUE_NOTIFICATION 100
// sense framework based values, 1000 for call, 500 for alarm
#define MIN_TD_VALUE_NOTIFICATION_CALL 1000
#define MIN_TD_VALUE_NOTIFICATION_ALARM 500

extern void smart_set_last_user_activity_time(void);
extern int smart_get_notification_level(int notif_type);

// screen state queries
extern bool ntf_is_screen_on(void);
extern bool ntf_is_screen_early_on(void);
extern bool ntf_is_screen_early_off(void);

// charge callbacks to notify ntf - call it from battery/policy drivers
extern void ntf_set_charge_state(bool on);
extern void ntf_set_charge_level(int level);

// flashlight
extern void ntf_set_cam_flashlight(bool on);

// was the screen wake by user input...
extern bool ntf_wake_by_user(void);
// signal a user input (use it in touchscreen, input drivers..)
extern void ntf_input_event(const char* caller, const char *param);
// vibration events
extern void ntf_vibration(int val);

/** add change listener */
extern void ntf_add_listener(void (*f)(char* event, int num_param, char* str_param));

#endif /* __NOTIFICATION_H__ */
