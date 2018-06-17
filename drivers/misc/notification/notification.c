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
#define DRIVER_VERSION "1.0"

#define NTF_D_LOG

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESCRIPTION);
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL");

#if defined(CONFIG_FB)
struct notifier_block *uci_ntf_fb_notifier;
#elif defined(CONFIG_MSM_RDM_NOTIFY)
struct notifier_block *uci_ntf_msm_drm_notif;
#endif

static bool screen_on = false, screen_on_early = false, screen_off_early = false;

// ======= SCREEN ON/OFF

bool ntf_is_screen_on(void) {
	return screen_on;
}
EXPORT_SYMBOL(ntf_is_screen_on);
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
void ntf_set_charge_state(bool on) {
#ifdef NTF_D_LOG
	pr_info("%s [cleanslate] charge state = %d\n",__func__,on);
#endif
	if (on!=is_charging) {
// change handle
	}
	is_charging = on;
}
EXPORT_SYMBOL(ntf_set_charge_state);
int charge_level = -1;
void ntf_set_charge_level(int level) {
#ifdef NTF_D_LOG
	pr_info("%s [cleanslate] level = %d\n",__func__,level);
#endif
	if (level!=charge_level) {
// change handle
	}
	charge_level = level;
}
EXPORT_SYMBOL(ntf_set_charge_level);


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
		screen_on = true;
		screen_on_early = true;
		screen_off_early = false;
            break;

        case FB_BLANK_POWERDOWN:
        case FB_BLANK_HSYNC_SUSPEND:
        case FB_BLANK_VSYNC_SUSPEND:
        case FB_BLANK_NORMAL:
		pr_info("ntf uci screen off\n");
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
	    break;
	case MSM_DRM_BLANK_UNBLANK:
		pr_info("ntf uci screen oh\n");
		if (first_unblank) {
			first_unblank = 0;
		}
		screen_on = true;
		screen_on_early = true;
		screen_off_early = false;
	    break;
	default:
	    pr_info("%s default\n", __func__);
	    break;
	}
    }
    return NOTIFY_OK;
}
#endif

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
	return rc;
}

static void __exit ntf_exit(void)
{
	pr_info("uci ntf - exit\n");
}

module_init(ntf_init);
module_exit(ntf_exit);

