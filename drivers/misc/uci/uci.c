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

#define DRIVER_AUTHOR "illes pal <illespal@gmail.com>"
#define DRIVER_DESCRIPTION "uci driver"
#define DRIVER_VERSION "1.2"

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESCRIPTION);
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL");

#if defined(CONFIG_FB)
struct notifier_block *fb_notifier;
#elif defined(CONFIG_MSM_RDM_NOTIFY)
struct notifier_block *uci_msm_drm_notif;
#endif


// file operations
int uci_fwrite(struct file* file, loff_t pos, unsigned char* data, unsigned int size) {
    int ret;
    ret = kernel_write(file, data, size, pos);
    return ret;
}

int uci_read(struct file* file, unsigned long long offset, unsigned char* data, unsigned int size) {
    int ret;
    ret = kernel_read(file, offset, data, size);
    return ret;
}

void uci_fclose(struct file* file) {
    fput(file);
}

struct file* uci_fopen(const char* path, int flags, int rights) {
    struct file* filp = NULL;
    int err = 0;

    filp = filp_open(path, flags, rights);

    if(IS_ERR(filp)) {
        err = PTR_ERR(filp);
	pr_err("[uci]File Open Error:%s %d\n",path, err);
        return NULL;
    }
    if(!filp->f_op){
        pr_err("[uci]File Operation Method Error!!\n");
        return NULL;
    }

    return filp;
}

#define MAX_PARAMS 100
#define MAX_STR_LEN 100
#define MAX_FILE_SIZE 2500

char *user_cfg_keys[MAX_PARAMS];
char *user_cfg_values[MAX_PARAMS];

char *sys_cfg_keys[MAX_PARAMS];
char *sys_cfg_values[MAX_PARAMS];

static int should_not_parse_next_close = 0;

static DEFINE_SPINLOCK(cfg_rw_lock);
static DEFINE_SPINLOCK(cfg_w_lock);

// --- write start
char *krnl_cfg_queue[MAX_PARAMS];
static int queue_length = 0;

static int stamp = 0;
static char stamps[10][3] = {"0\n","1\n","2\n","3\n","4\n","5\n","6\n","7\n","8\n","9\n"};

void write_uci_krnl_cfg_file(void) {
	// locking
	struct file*fp = NULL;
	int rc = 0;
	int i = 0;
	loff_t pos = 0;
	unsigned char to_write[1000] = "";

	spin_lock(&cfg_w_lock);
	strcat(to_write, "#cleanslate kernel out\n");
	for (i=0; i<queue_length;i++) {
		strcat(to_write, krnl_cfg_queue[i]);
		strcat(to_write, "\n");
	}
	strcat(to_write,stamps[stamp++]);
	if (stamp==10) stamp = 0;
	queue_length = 0;
	spin_unlock(&cfg_w_lock); // must unlock here, fopen may sleep
	// UNLOCK

	pr_info("%s [CLEANSLATE] uci writing file kernel out...\n",__func__);
	fp=uci_fopen (UCI_KERNEL_FILE, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fp) {
		rc = uci_fwrite(fp,pos,to_write,strlen(to_write));
		if (rc) pr_info("%s [CLEANSLATE] uci error file kernel out...%d\n",__func__,rc);
		vfs_fsync(fp,1);
		uci_fclose(fp);
		pr_info("%s [CLEANSLATE] uci closed file kernel out...\n",__func__);
	}
}

static void write_uci_out_work_func(struct work_struct * write_uci_out_work)
{
	write_uci_krnl_cfg_file();
}
static DECLARE_WORK(write_uci_out_work, write_uci_out_work_func);

void write_uci_out(char *message) {
	spin_lock(&cfg_w_lock);
	krnl_cfg_queue[queue_length] = message;
	queue_length++;
	spin_unlock(&cfg_w_lock);
	schedule_work(&write_uci_out_work);
}
EXPORT_SYMBOL(write_uci_out);

// --- write end

// Parsing
int parse_uci_cfg_file(const char *file_name, bool sys) {
//	fileread(file_name);

#if 1
	struct file*fp = NULL;
	fp=uci_fopen (file_name, O_RDONLY, 0);
	if (fp==NULL) {
		pr_info("%s [uci] cannot read file %s\n",__func__,file_name);
		return -1;
	} else {
		off_t fsize;
		char *buf;
		char *buf_o;
		char *line;
		char *side;
		char *dupline;
		struct inode *inode;
		int line_num = 0;
		int token_num = 0;
		int prop_num = 0;

		char *l_cfg_keys[MAX_PARAMS];
		char *l_cfg_values[MAX_PARAMS];

		inode=fp->f_inode;
		fsize=inode->i_size;
		pr_info("%s [uci] file size %d...  %s\n",__func__,(int)fsize,file_name);
		if (fsize> MAX_FILE_SIZE) { 
			pr_err("uci file too big\n"); 
			return -1;
		}
		if (fsize==0) {
			pr_err("uci file being deleted\n"); 
			return -2;
		}

		buf=(char *) kmalloc(fsize+1,GFP_KERNEL);
		buf_o=buf;
		uci_read(fp,0,buf,fsize);
		buf[fsize]='\0';
		if (sys && buf[fsize-1]!='#') {
			pr_err("%s uci sys file incomplete\n",__func__);
			return -2;
		}

		while ((line = strsep(&buf, "\n")) != NULL) {
			pr_info("%s uci %s | %d  |- ",__func__, line, line_num);
			if (line[0] == '#' || line[0]=='\0') continue; // comments, empty lines
			token_num = 0;
			while ((side = strsep(&line, "=")) != NULL) {
				char buf[256];
				sscanf(side, "%s", buf); // trimming on both sides
				dupline = kstrdup(buf, GFP_KERNEL);
				if (token_num==0) {
					l_cfg_keys[prop_num] = (char*)dupline;
					//pr_info("%s uci param key = %s | prop num: %d\n",__func__, dupline, prop_num);
				} else {
					l_cfg_values[prop_num] = (char*)dupline;
					//pr_info("%s uci param value = %s | prop num: %d\n",__func__, dupline, prop_num);
					prop_num++;
				}
				token_num++;
			}
			line_num++;
			if (prop_num==MAX_PARAMS-1) break;
		}
		l_cfg_keys[prop_num] = NULL;
		l_cfg_values[prop_num] = NULL;

		pr_info("\n%s [uci] closing file...  %s\n",__func__,file_name);

		kfree(buf_o);

		should_not_parse_next_close = 1;
		uci_fclose(fp);
		msleep(10);

		should_not_parse_next_close = 0;

		spin_lock(&cfg_rw_lock);
		if (sys) {
			int count = 0;
			while (sys_cfg_values[count]!=NULL) {
				kfree(sys_cfg_values[count++]);
			}
			count = 0;
			while (l_cfg_values[count]!=NULL) {
				sys_cfg_values[count] = l_cfg_values[count];
				count++;
			}
			for (;count<MAX_PARAMS;count++) sys_cfg_values[count] = NULL;

			count = 0;
			while (sys_cfg_keys[count]!=NULL) {
				kfree(sys_cfg_keys[count++]);
			}
			count = 0;
			while (l_cfg_keys[count]!=NULL) {
				sys_cfg_keys[count] = l_cfg_keys[count];
				count++;
			}
			sys_cfg_keys[count] = NULL;
			for (;count<MAX_PARAMS;count++) sys_cfg_keys[count] = NULL;

		} else {
			int count = 0;
			while (user_cfg_values[count]!=NULL) {
				kfree(user_cfg_values[count++]);
			}
			count = 0;
			while (l_cfg_values[count]!=NULL) {
				user_cfg_values[count] = l_cfg_values[count];
				count++;
			}
			for (;count<MAX_PARAMS;count++) user_cfg_values[count] = NULL;

			count = 0;
			while (user_cfg_keys[count]!=NULL) {
				kfree(user_cfg_keys[count++]);
			}
			count = 0;
			while (l_cfg_keys[count]!=NULL) {
				user_cfg_keys[count] = l_cfg_keys[count];
				count++;
			}
			for (;count<MAX_PARAMS;count++) user_cfg_keys[count] = NULL;
		}
		spin_unlock(&cfg_rw_lock);
	}
	return 0;
#endif
}

bool is_uci_path(const char *file_name) {
	if (!file_name) return false;
	if (!strcmp(file_name, UCI_USER_FILE)) return true;
	if (!strcmp(file_name, UCI_SYS_FILE)) return true;
	if (!strcmp(file_name, UCI_KERNEL_FILE)) return true;
	if (!strcmp(file_name, UCI_HOSTS_FILE)) return true;
	return false;
}
EXPORT_SYMBOL(is_uci_path);

bool is_uci_file(const char *file_name) {
	if (!file_name) return false;
	if (!strcmp(file_name, UCI_USER_FILE_END)) return true;
	if (!strcmp(file_name, UCI_SYS_FILE_END)) return true;
	if (!strcmp(file_name, UCI_KERNEL_FILE_END)) return true;
	if (!strcmp(file_name, UCI_HOSTS_FILE_END)) return true;
	return false;
}
EXPORT_SYMBOL(is_uci_file);

static bool user_cfg_parsed = false;
static bool sys_cfg_parsed = false;

static bool should_parse_user = true;
static bool should_parse_sys = true;

void (*user_listeners[100])(void);
int user_listener_counter = 0;

void uci_add_user_listener(void (*f)(void)) {
	if (user_listener_counter<100) {
		user_listeners[user_listener_counter++] = f;
	} else {
		// error;
	}
}
EXPORT_SYMBOL(uci_add_user_listener);

void parse_uci_user_cfg_file(void) {
	int rc = parse_uci_cfg_file(UCI_USER_FILE,false);
	if (!rc) { user_cfg_parsed = true; should_parse_user = false; }
	if (!rc) { 
		int i=0;
		user_cfg_parsed = true; should_parse_user = false; 
		for (;i<user_listener_counter;i++) {
			(*user_listeners[i])();
		}
	}
}

void (*sys_listeners[100])(void);
int sys_listener_counter = 0;

void uci_add_sys_listener(void (*f)(void)) {
	if (sys_listener_counter<100) {
		sys_listeners[sys_listener_counter++] = f;
	} else {
		// error;
	}
}
EXPORT_SYMBOL(uci_add_sys_listener);


void parse_uci_sys_cfg_file(void) {
	int rc = parse_uci_cfg_file(UCI_SYS_FILE,true);
	int count = 0;
	while (rc==-2) { // sys file is deleted by companion app...retry!
		msleep(10); // sleep... then retry
		rc = parse_uci_cfg_file(UCI_SYS_FILE,true);
		count++;
		if (count>5) break;
	}
	if (!rc) { 
		int i=0;
		sys_cfg_parsed = true; should_parse_sys = false; 
		for (;i<sys_listener_counter;i++) {
			(*sys_listeners[i])();
		}
	}
}


// Properties

const char* uci_get_user_property_str(const char* property, const char* default_value) {
	const char* ret = NULL;
	//pr_info("%s uci get user str prop %s\n",__func__,property);
	if (user_cfg_parsed) {
		int param_count = 0;

		spin_lock(&cfg_rw_lock);
		while(1) {
			const char *key = user_cfg_keys[param_count];
			if (key==NULL) break;
			if (!strcmp(property,key)) {
				ret = user_cfg_values[param_count];
				//pr_info("%s uci key %s -> value %s\n",__func__,key, ret);
				spin_unlock(&cfg_rw_lock);
				return  ret;
			}
			param_count++;
		}
		spin_unlock(&cfg_rw_lock);
	}
	pr_info("%s uci get user prop *failed* %s\n",__func__, property);
	return default_value;
}
EXPORT_SYMBOL(uci_get_user_property_str);

int uci_get_user_property_int(const char* property, int default_value) {
	const char* str = uci_get_user_property_str(property, 0);
	long int ret = 0;
	if (!str) return default_value;
        if (kstrtol(str, 10, &ret) < 0)
                return -EINVAL;
	return (int)ret;
}
EXPORT_SYMBOL(uci_get_user_property_int);

int uci_get_user_property_int_mm(const char* property, int default_value, int min, int max) {
	int ret = uci_get_user_property_int(property, default_value);
	if (ret<min || ret>max) ret = default_value;
	//pr_info("%s uci get user prop %s = %d\n",__func__, property, ret);
	return ret;
}
EXPORT_SYMBOL(uci_get_user_property_int_mm);

const char* uci_get_sys_property_str(const char* property, const char* default_value) {
	const char* ret = NULL;
	//pr_info("%s uci get sys str prop %s\n",__func__,property);
	if (sys_cfg_parsed) {
		int param_count = 0;

		spin_lock(&cfg_rw_lock);
		while(1) {
			const char *key = sys_cfg_keys[param_count];
			if (key==NULL) break;
			if (!strcmp(property,key)) {
				ret = sys_cfg_values[param_count];
				//pr_info("%s uci key %s -> value %s\n",__func__,key, ret);
				spin_unlock(&cfg_rw_lock);
				return  ret;
			}
			param_count++;
		}
		spin_unlock(&cfg_rw_lock);
	}
	pr_info("%s uci get sys prop *failed* %s\n",__func__, property);
	return default_value;
}
EXPORT_SYMBOL(uci_get_sys_property_str);

int uci_get_sys_property_int(const char* property, int default_value) {
	const char* str = uci_get_sys_property_str(property, 0);
	long int ret = 0;
	pr_info("%s uci %s str = %s\n",__func__, property, str?str:"NULL");
	if (!str) return default_value;
        if (kstrtol(str, 10, &ret) < 0)
                return -EINVAL;
	return (int)ret;
}
EXPORT_SYMBOL(uci_get_sys_property_int);

int uci_get_sys_property_int_mm(const char* property, int default_value, int min, int max) {
	int ret = uci_get_sys_property_int(property, default_value);
	if (ret<min || ret>max) ret = default_value;
	pr_info("%s uci get sys prop %s = %d\n",__func__, property, ret);
	return ret;
}
EXPORT_SYMBOL(uci_get_sys_property_int_mm);

static bool first_parse_done = 0;

static void do_reschedule(void);

static void reschedule_work_func(struct work_struct * reschedule_work)
{
	do_reschedule();
}
static DECLARE_WORK(reschedule_work, reschedule_work_func);

static void parse_work_func(struct work_struct * parse_work_func_work)
{
	pr_info("%s uci \n",__func__);
	if (should_parse_user) parse_uci_user_cfg_file();
	if (should_parse_sys) parse_uci_sys_cfg_file();
	if (!first_parse_done) {
		if (user_cfg_parsed) {
			first_parse_done = true;
		} else {
			pr_info("%s uci reschedule till read first \n",__func__);
			schedule_work(&reschedule_work);
		}
	}
}
static DECLARE_DELAYED_WORK(parse_work_func_work, parse_work_func);
static void do_reschedule(void) {
	schedule_delayed_work(&parse_work_func_work, 3 * 100);
}
// alarm timer
static struct alarm parse_user_cfg_rtc;
static enum alarmtimer_restart parse_user_cfg_rtc_callback(struct alarm *al, ktime_t now)
{
	pr_info("%s uci alarm \n",__func__);
	schedule_delayed_work(&parse_work_func_work, 15 * 100);
	return ALARMTIMER_NORESTART;
}

static void start_alarm_parse(int sec) {
	ktime_t wakeup_time;
	ktime_t curr_time = { .tv64 = 0 };
	wakeup_time = ktime_add_us(curr_time,
	    (sec * 1000LL * 1000LL)); // 40 sec to msec to usec
	alarm_cancel(&parse_user_cfg_rtc);
	alarm_start_relative(&parse_user_cfg_rtc, wakeup_time); // start new...
}

void notify_uci_file_closed(const char *file_name) {
	if (should_not_parse_next_close) {
		pr_info("%s uci skipping for now %s\n",__func__, file_name);
		return;
	}
	if (!strcmp(file_name, UCI_USER_FILE_END) && should_parse_user) {
		schedule_delayed_work(&parse_work_func_work,1);
		return;
	}
	if (!strcmp(file_name, UCI_SYS_FILE_END) && should_parse_sys) {
		schedule_delayed_work(&parse_work_func_work,1);
		return;
	}
}
EXPORT_SYMBOL(notify_uci_file_closed);

void notify_uci_file_write_opened(const char *file_name) {
	pr_info("%s uci write opened  %s\n",__func__, file_name);
	if (!strcmp(file_name, UCI_USER_FILE)) { should_parse_user = true; return; }
	if (!strcmp(file_name, UCI_SYS_FILE)) { should_parse_sys = true; return; }
	if (!strcmp(file_name, UCI_USER_FILE_END)) { should_parse_user = true; return; }
	if (!strcmp(file_name, UCI_SYS_FILE_END)) { should_parse_sys = true; return; }
}
EXPORT_SYMBOL(notify_uci_file_write_opened);

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
		pr_info("uci screen on -early\n");
            break;

        case FB_BLANK_POWERDOWN:
        case FB_BLANK_HSYNC_SUSPEND:
        case FB_BLANK_VSYNC_SUSPEND:
        case FB_BLANK_NORMAL:
		pr_info("uci screen off -early\n");
            break;
        }
    }
    if (evdata && evdata->data && event == FB_EVENT_BLANK ) {
        blank = evdata->data;
        switch (*blank) {
        case FB_BLANK_UNBLANK:
		pr_info("uci screen on\n");
		if (first_unblank) {
			start_alarm_parse(20); // start in 40 sec, user cfg parse...
			first_unblank = 0;
		}
            break;

        case FB_BLANK_POWERDOWN:
        case FB_BLANK_HSYNC_SUSPEND:
        case FB_BLANK_VSYNC_SUSPEND:
        case FB_BLANK_NORMAL:
		pr_info("uci screen off\n");
		// start_alarm_parse(1); // TODO vibrate on change!
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
	    break;
	case MSM_DRM_BLANK_UNBLANK:
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
		pr_info("uci screen off\n");
	    break;
	case MSM_DRM_BLANK_UNBLANK:
		pr_info("uci screen on\n");
		if (first_unblank) {
			start_alarm_parse(20); // start in 40 sec, user cfg parse...
			first_unblank = 0;
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

static int __init uci_init(void)
{
	int rc = 0;
	int status = 0;
	pr_info("uci - init\n");
#if defined(CONFIG_FB)
	uci_fb_notifier = kzalloc(sizeof(struct notifier_block), GFP_KERNEL);;
	uci_fb_notifier->notifier_call = fb_notifier_callback;
	fb_register_client(uci_fb_notifier);
#elif defined(CONFIG_MSM_RDM_NOTIFY)
        uci_msm_drm_notif = kzalloc(sizeof(struct notifier_block), GFP_KERNEL);;
        uci_msm_drm_notif->notifier_call = fb_notifier_callback;
        status = msm_drm_register_client(uci_msm_drm_notif);
        if (status)
                pr_err("Unable to register msm_drm_notifier: %d\n", status);
#endif
	alarm_init(&parse_user_cfg_rtc, ALARM_REALTIME,
		parse_user_cfg_rtc_callback);

	return rc;
}

static void __exit uci_exit(void)
{
	pr_info("uci - exit\n");
}

module_init(uci_init);
module_exit(uci_exit);

