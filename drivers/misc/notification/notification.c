/*
 * Copyright (C) 2018 Pal Zoltan Illes
 *
 * Licensed under the GPL-2 or later.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/spinlock.h>


//file operation+
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/vmalloc.h>
#include <asm/segment.h>
#include <asm/uaccess.h>
#include <linux/buffer_head.h>
#include <linux/mm.h>
//file operation-

#define CONFIG_MSM_RDM_NOTIFY
#undef CONFIG_FB

#include <linux/notifier.h>
#include <linux/fb.h>

#if defined(CONFIG_MSM_RDM_NOTIFY)
#include <linux/msm_drm_notify.h>
#endif

#include <linux/alarmtimer.h>
#include <linux/uci/uci.h>
#include <linux/notification/notification.h>

#define DRIVER_AUTHOR "illes pal <illespal@gmail.com>"
#define DRIVER_DESCRIPTION "uci notifications driver"
#define DRIVER_VERSION "1.1"

//#define NTF_D_LOG

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESCRIPTION);
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL");

#if defined(CONFIG_FB)
struct notifier_block *uci_ntf_fb_notifier;
#elif defined(CONFIG_MSM_RDM_NOTIFY)
struct notifier_block *uci_ntf_msm_drm_notif;
#endif

bool ntf_face_down = false;
EXPORT_SYMBOL(ntf_face_down);
bool ntf_proximity = false;
EXPORT_SYMBOL(ntf_proximity);
bool ntf_silent = false;
EXPORT_SYMBOL(ntf_silent);
bool ntf_ringing = false;
EXPORT_SYMBOL(ntf_ringing);
bool ntf_in_call = false;

// listeners

static void (*ntf_listeners[100])(char* event, int num_param, char* str_param);
static int ntf_listener_counter = 0;

static void ntf_notify_listeners(char* event, int num_param, char* str_param) {
	int i =0;
	for (;i<ntf_listener_counter;i++) {
		(*ntf_listeners[i])(event,num_param,str_param);
	}
}

void ntf_add_listener(void (*f)(char* event, int num_param, char* str_param)) {
	if (ntf_listener_counter<100) {
		ntf_listeners[ntf_listener_counter++] = f;
	} else {
		// error;
	}
}
EXPORT_SYMBOL(ntf_add_listener);

static bool screen_on = false, screen_on_early = false, screen_off_early = false;
static unsigned long screen_on_jiffies = 0;

// ======= SCREEN ON/OFF

bool ntf_is_screen_on(void) {
	return screen_on;
}
EXPORT_SYMBOL(ntf_is_screen_on);
bool ntf_is_screen_on_a_while(void) {
	if (screen_on) {
		unsigned int diff_screen_on = jiffies - screen_on_jiffies;
		if (diff_screen_on >= 50) return true; // 500msec
	}
	return false;

}
EXPORT_SYMBOL(ntf_is_screen_on_a_while);
bool ntf_is_screen_early_on(void) {
	return screen_on_early;
}
EXPORT_SYMBOL(ntf_is_screen_early_on);
bool ntf_is_screen_early_off(void) {
	return screen_off_early;
}
EXPORT_SYMBOL(ntf_is_screen_early_off);

// ======= CHARGE

bool is_charging = false;
bool charge_state_changed = true;
void ntf_set_charge_state(bool on) {
#ifdef NTF_D_LOG
	pr_info("%s [cleanslate] charge state = %d\n",__func__,on);
#endif
	if (on!=is_charging) {
// change handle
		ntf_notify_listeners(NTF_EVENT_CHARGE_STATE, on, "");
		charge_state_changed = true;
	}
	is_charging = on;
}
EXPORT_SYMBOL(ntf_set_charge_state);
int charge_level = -1;
void ntf_set_charge_level(int level) {
#ifdef NTF_D_LOG
	pr_info("%s [cleanslate] level = %d\n",__func__,level);
#endif
//	if (level!=charge_level || (charge_state_changed && is_charging))
	{
// change handle
		ntf_notify_listeners(NTF_EVENT_CHARGE_LEVEL, level, "");
		charge_state_changed = false;
	}
	charge_level = level;
}
EXPORT_SYMBOL(ntf_set_charge_level);

static bool wake_by_user = true;
static unsigned long screen_off_jiffies = 0;

#if defined(CONFIG_FB)
static int first_unblank = 1;

static int fb_notifier_callback(struct notifier_block *self,
                                 unsigned long event, void *data)
{
    struct fb_event *evdata = data;
    int *blank;

    if (evdata && evdata->data && event == FB_EARLY_EVENT_BLANK ) {
        blank = evdata->data;
        switch (*blank) {
        case FB_BLANK_UNBLANK:
		screen_on_early = true;
		pr_info("ntf uci screen on -early\n");
            break;

        case FB_BLANK_POWERDOWN:
        case FB_BLANK_HSYNC_SUSPEND:
        case FB_BLANK_VSYNC_SUSPEND:
        case FB_BLANK_NORMAL:
		screen_off_early = true;
		pr_info("ntf uci screen off -early\n");
            break;
        }
    }
    if (evdata && evdata->data && event == FB_EVENT_BLANK ) {
        blank = evdata->data;
        switch (*blank) {
        case FB_BLANK_UNBLANK:
		pr_info("ntf uci screen on\n");
		if (first_unblank) {
			first_unblank = 0;
		}
		screen_on_jiffies = jiffies;
		screen_on = true;
		screen_on_early = true;
		screen_off_early = false;
            break;

        case FB_BLANK_POWERDOWN:
        case FB_BLANK_HSYNC_SUSPEND:
        case FB_BLANK_VSYNC_SUSPEND:
        case FB_BLANK_NORMAL:
		pr_info("ntf uci screen off\n");
		screen_off_jiffies = jiffies;
		screen_on = false;
		screen_on_early = false;
		screen_off_early = true;
            break;
        }
    }
    return 0;
}


#elif defined(CONFIG_MSM_RDM_NOTIFY)
static int first_unblank = 1;

static int fb_notifier_callback(
    struct notifier_block *nb, unsigned long val, void *data)
{
    struct msm_drm_notifier *evdata = data;
    unsigned int blank;

    if (val != MSM_DRM_EARLY_EVENT_BLANK && val != MSM_DRM_EVENT_BLANK)
	return 0;

    if (evdata->id != MSM_DRM_PRIMARY_DISPLAY)
        return 0;

    pr_info("[info] %s go to the msm_drm_notifier_callback value = %d\n",
	    __func__, (int)val);

    if (evdata && evdata->data && val ==
	MSM_DRM_EARLY_EVENT_BLANK) {
	blank = *(int *)(evdata->data);
	switch (blank) {
	case MSM_DRM_BLANK_POWERDOWN:
		screen_off_early = true;
		pr_info("ntf uci screen off\n");
	    break;
	case MSM_DRM_BLANK_UNBLANK:
		screen_on_early = true;
		pr_info("ntf uci screen on\n");
	    break;
	default:
	    pr_info("%s defalut\n", __func__);
	    break;
	}
    }
    if (evdata && evdata->data && val ==
	MSM_DRM_EVENT_BLANK) {
	blank = *(int *)(evdata->data);
	switch (blank) {
	case MSM_DRM_BLANK_POWERDOWN:
		pr_info("ntf uci screen off\n");
		screen_on = false;
		screen_on_early = false;
		screen_off_early = true;
		wake_by_user = false;
		screen_off_jiffies = jiffies;
		ntf_notify_listeners(NTF_SCREEN_OFF,1,"");
	    break;
	case MSM_DRM_BLANK_UNBLANK:
		pr_info("ntf uci screen oh\n");
		if (first_unblank) {
			first_unblank = 0;
		}
		screen_on = true;
		screen_on_early = true;
		screen_off_early = false;
		if (wake_by_user) {
			ntf_notify_listeners(NTF_WAKE_BY_USER,1,"");
		}
	    break;
	default:
	    pr_info("%s default\n", __func__);
	    break;
	}
    }
    return NOTIFY_OK;
}
#endif

bool ntf_wake_by_user(void) {
	return wake_by_user;
}
EXPORT_SYMBOL(ntf_wake_by_user);

void ntf_input_event(const char* caller, const char *param) {
	// input event happened, stop stuff, store timesamp, set wake_by_user
	if (!wake_by_user) {
		wake_by_user = true; // TODO check screen off events
		ntf_notify_listeners(NTF_WAKE_BY_USER,1,"");
	} else { 
		wake_by_user = true; // TODO check screen off events
	}
}
EXPORT_SYMBOL(ntf_input_event);

void ntf_vibration(int length) {
	if (length>=MIN_TD_VALUE_NOTIFICATION) {
#if 1
// op6
		if (length==MIN_TD_VALUE_OP6_FORCED_FP) return;
		if (length==MIN_TD_VALUE_OP6_SILENT_MODE) return;
#endif
		ntf_notify_listeners(NTF_EVENT_NOTIFICATION, 1, NTF_EVENT_NOTIFICATION_ARG_HAPTIC);
	}
}
EXPORT_SYMBOL(ntf_vibration);

void ntf_led_blink(enum notif_led_type led, bool on) {
	// low battery blink RED, don't do a thing...
	if (on && led == NTF_LED_RED && charge_level <= 15) return;
	if (on) {
#if 1
// op6 - if blink starts too close to screen off, don't trigger notification event
		unsigned int diff_screen_off = jiffies - screen_off_jiffies;
		if (diff_screen_off <= 50) return;
#endif
		ntf_notify_listeners(NTF_EVENT_NOTIFICATION,1,"");
	}
}
EXPORT_SYMBOL(ntf_led_blink);

static int last_notification_number = 0;
// registered sys uci listener
static void uci_sys_listener(void) {
        pr_info("%s [CLEANSLATE] sys listener... \n",__func__);
        {
                bool ringing_new = !!uci_get_sys_property_int_mm("ringing", 0, 0, 1);
                bool proximity_new = !!uci_get_sys_property_int_mm("proximity", 0, 0, 1);
		bool in_call = !!uci_get_sys_property_int_mm("in_call", 0, 0, 1);
                ntf_face_down = !!uci_get_sys_property_int_mm("face_down", 0, 0, 1);
                ntf_silent = !!uci_get_sys_property_int_mm("silent", 0, 0, 1);

		if (in_call != ntf_in_call) {
			ntf_in_call = in_call;
			ntf_notify_listeners(NTF_EVENT_IN_CALL, ntf_in_call?1:0, "");
		}
		if (proximity_new != ntf_proximity) {
			ntf_proximity = proximity_new;
			ntf_notify_listeners(NTF_EVENT_PROXIMITY, ntf_proximity?1:0, "");
		}

                if (ringing_new && !ntf_ringing) {
			ntf_notify_listeners(NTF_EVENT_RINGING, 1, "");
                }
                if (!ringing_new && ntf_ringing) {
                        ntf_ringing = false;
			ntf_notify_listeners(NTF_EVENT_RINGING, 0, "");
                }
                ntf_ringing = ringing_new;

                pr_info("%s uci sys face_down %d\n",__func__,ntf_face_down);
                pr_info("%s uci sys proximity %d\n",__func__,ntf_proximity);
                pr_info("%s uci sys silent %d\n",__func__,ntf_silent);
                pr_info("%s uci sys ringing %d\n",__func__,ntf_ringing);
        }
	{
    		int ringing = uci_get_sys_property_int_mm("ringing", 0, 0, 1);
    		pr_info("%s uci sys ringing %d\n",__func__,ringing);
    		if (ringing) {
                        ntf_input_event(__func__,NULL);
    		}
	}
	{
    		int notifications = uci_get_sys_property_int("notifications",0);
    		if (notifications != -EINVAL) {
    			if (notifications>last_notification_number) {
				// send notification event
				ntf_notify_listeners(NTF_EVENT_NOTIFICATION, 1, "");
			}
			last_notification_number = notifications;
		}
	}
}

// registered user uci listener
static void uci_user_listener(void) {
        pr_info("%s [CLEANSLATE] user listener... \n",__func__);
}


static int __init ntf_init(void)
{
	int rc = 0;
	int status = 0;
	pr_info("uci ntf - init\n");
#if defined(CONFIG_FB)
	uci_ntf_fb_notifier = kzalloc(sizeof(struct notifier_block), GFP_KERNEL);;
	uci_ntf_fb_notifier->notifier_call = fb_notifier_callback;
	fb_register_client(uci_ntf_fb_notifier);
#elif defined(CONFIG_MSM_RDM_NOTIFY)
        uci_ntf_msm_drm_notif = kzalloc(sizeof(struct notifier_block), GFP_KERNEL);;
        uci_ntf_msm_drm_notif->notifier_call = fb_notifier_callback;
        status = msm_drm_register_client(uci_ntf_msm_drm_notif);
        if (status)
                pr_err("Unable to register msm_drm_notifier: %d\n", status);
#endif
	uci_add_sys_listener(uci_sys_listener);
	uci_add_user_listener(uci_user_listener);

	return rc;
}

static void __exit ntf_exit(void)
{
	pr_info("uci ntf - exit\n");
}

module_init(ntf_init);
module_exit(ntf_exit);

