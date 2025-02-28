/*
 * Copyright (c) 2013-2015,2017, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "cpu-boost: " fmt

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/sched.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/time.h>

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
#ifdef CONFIG_UCI
#include <linux/uci/uci.h>
static bool boost_eas = false;
static int boost_eas_level = 1;
static bool boost_eas_level_ext = false;
#endif
#endif
struct cpu_sync {
	int cpu;
	unsigned int input_boost_min;
	unsigned int input_boost_freq;
};

static DEFINE_PER_CPU(struct cpu_sync, sync_info);
static struct workqueue_struct *cpu_boost_wq;

static struct work_struct input_boost_work;

static bool input_boost_enabled;

static unsigned int input_boost_ms = 40;
module_param(input_boost_ms, uint, 0644);

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
static int dynamic_stune_boost;
module_param(dynamic_stune_boost, uint, 0644);
static bool stune_boost_active;
static int boost_slot;
#ifdef CONFIG_UCI
static bool shown_debug_stune = false;
static bool shown_debug_input = false;
static void uci_user_listener(void) {
	boost_eas = uci_get_user_property_int_mm("boost_eas", 1, 0, 1);
	boost_eas_level = uci_get_user_property_int_mm("boost_eas_level", 1, 0, 3);
	boost_eas_level_ext = uci_get_user_property_int_mm("boost_eas_level_ext", 0, 0, 1);
	pr_info("%s [CLEANSLATE] stune uci user listener %d %d %d\n",__func__,boost_eas,boost_eas_level,boost_eas_level_ext);
	shown_debug_stune = false;
	shown_debug_input = false;
}
static int boost_map[4] = { 9, 13, 20, 25 };
static int get_dynamic_stune_boost(void) {
	int ret = 0;
	if (boost_eas_level_ext) return dynamic_stune_boost;
	ret = boost_map[boost_eas_level]; // 9 - 25;
	if (!shown_debug_stune) {
		shown_debug_stune = true;
		pr_info("%s [CLEANSLATE] dynamic stune boost value %d\n",__func__,ret);
	}
	return ret;
}
static int input_ms_map[4] = { 60, 90, 120, 450 };
static int get_input_boost_ms(void) {
	int ret = 0;
	if (!boost_eas || boost_eas_level_ext) return input_boost_ms;
	ret = input_ms_map[boost_eas_level]; // 60 - 450 msec;
	if (!shown_debug_input) {
		shown_debug_input = true;
		pr_info("%s [CLEANSLATE] input boost ms value %d\n",__func__,ret);
	}
	return ret;
}
#endif
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

static unsigned int sched_boost_on_input;
module_param(sched_boost_on_input, uint, 0644);

static bool sched_boost_active;

static struct delayed_work input_boost_rem;
static u64 last_input_time;
#define MIN_INPUT_INTERVAL (150 * USEC_PER_MSEC)

static int set_input_boost_freq(const char *buf, const struct kernel_param *kp)
{
	int i, ntokens = 0;
	unsigned int val, cpu;
	const char *cp = buf;
	bool enabled = false;

	while ((cp = strpbrk(cp + 1, " :")))
		ntokens++;

	/* single number: apply to all CPUs */
	if (!ntokens) {
		if (sscanf(buf, "%u\n", &val) != 1)
			return -EINVAL;
		for_each_possible_cpu(i)
			per_cpu(sync_info, i).input_boost_freq = val;
		goto check_enable;
	}

	/* CPU:value pair */
	if (!(ntokens % 2))
		return -EINVAL;

	cp = buf;
	for (i = 0; i < ntokens; i += 2) {
		if (sscanf(cp, "%u:%u", &cpu, &val) != 2)
			return -EINVAL;
		if (cpu >= num_possible_cpus())
			return -EINVAL;

		per_cpu(sync_info, cpu).input_boost_freq = val;
		cp = strchr(cp, ' ');
		cp++;
	}

check_enable:
	for_each_possible_cpu(i) {
		if (per_cpu(sync_info, i).input_boost_freq) {
			enabled = true;
			break;
		}
	}
	input_boost_enabled = enabled;

	return 0;
}

static int get_input_boost_freq(char *buf, const struct kernel_param *kp)
{
	int cnt = 0, cpu;
	struct cpu_sync *s;

	for_each_possible_cpu(cpu) {
		s = &per_cpu(sync_info, cpu);
		cnt += snprintf(buf + cnt, PAGE_SIZE - cnt,
				"%d:%u ", cpu, s->input_boost_freq);
	}
	cnt += snprintf(buf + cnt, PAGE_SIZE - cnt, "\n");
	return cnt;
}

static const struct kernel_param_ops param_ops_input_boost_freq = {
	.set = set_input_boost_freq,
	.get = get_input_boost_freq,
};
module_param_cb(input_boost_freq, &param_ops_input_boost_freq, NULL, 0644);

/*
 * The CPUFREQ_ADJUST notifier is used to override the current policy min to
 * make sure policy min >= boost_min. The cpufreq framework then does the job
 * of enforcing the new policy.
 */
static int boost_adjust_notify(struct notifier_block *nb, unsigned long val,
				void *data)
{
	struct cpufreq_policy *policy = data;
	unsigned int cpu = policy->cpu;
	struct cpu_sync *s = &per_cpu(sync_info, cpu);
	unsigned int ib_min = s->input_boost_min;

	switch (val) {
	case CPUFREQ_ADJUST:
		if (!ib_min)
			break;

		pr_debug("CPU%u policy min before boost: %u kHz\n",
			 cpu, policy->min);
		pr_debug("CPU%u boost min: %u kHz\n", cpu, ib_min);

		cpufreq_verify_within_limits(policy, ib_min, UINT_MAX);

		pr_debug("CPU%u policy min after boost: %u kHz\n",
			 cpu, policy->min);
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block boost_adjust_nb = {
	.notifier_call = boost_adjust_notify,
};

static void update_policy_online(void)
{
	unsigned int i;

	/* Re-evaluate policy to trigger adjust notifier for online CPUs */
	get_online_cpus();
	for_each_online_cpu(i) {
		pr_debug("Updating policy for CPU%d\n", i);
		cpufreq_update_policy(i);
	}
	put_online_cpus();
}

static void do_input_boost_rem(struct work_struct *work)
{
	unsigned int i, ret;
	struct cpu_sync *i_sync_info;

	/* Reset the input_boost_min for all CPUs in the system */
	pr_debug("Resetting input boost min for all CPUs\n");
	for_each_possible_cpu(i) {
		i_sync_info = &per_cpu(sync_info, i);
		i_sync_info->input_boost_min = 0;
	}

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	/* Reset dynamic stune boost value to the default value */
	if (stune_boost_active) {
		reset_stune_boost("top-app", boost_slot);
		stune_boost_active = false;
	}
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

	/* Update policies for all online CPUs */
	update_policy_online();

	if (sched_boost_active) {
		ret = sched_set_boost(0);
		if (ret)
			pr_err("cpu-boost: HMP boost disable failed\n");
		sched_boost_active = false;
	}
}

static void do_input_boost(struct work_struct *work)
{
	unsigned int i, ret;
	struct cpu_sync *i_sync_info;

	cancel_delayed_work_sync(&input_boost_rem);
	if (sched_boost_active) {
		sched_set_boost(0);
		sched_boost_active = false;
	}

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	if (stune_boost_active) {
		reset_stune_boost("top-app", boost_slot);
		stune_boost_active = false;
	}
#endif
	/* Set the input_boost_min for all CPUs in the system */
	pr_debug("Setting input boost min for all CPUs\n");
	for_each_possible_cpu(i) {
		i_sync_info = &per_cpu(sync_info, i);
		i_sync_info->input_boost_min = i_sync_info->input_boost_freq;
	}

	/* Update policies for all online CPUs */
	update_policy_online();

	/* Enable scheduler boost to migrate tasks to big cluster */
	if (sched_boost_on_input > 0) {
		ret = sched_set_boost(sched_boost_on_input);
		if (ret)
			pr_err("cpu-boost: HMP boost enable failed\n");
		else
			sched_boost_active = true;
	}

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	/* Set dynamic stune boost value */
#ifdef CONFIG_UCI
	if (boost_eas || boost_eas_level_ext) {
		ret = do_stune_boost("top-app", get_dynamic_stune_boost(), &boost_slot);
		if (!ret)
			stune_boost_active = true;
	}
#else
	ret = do_stune_boost("top-app", dynamic_stune_boost, &boost_slot);
	if (!ret)
		stune_boost_active = true;
#endif
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
#ifdef CONFIG_UCI
	queue_delayed_work(cpu_boost_wq, &input_boost_rem,
					msecs_to_jiffies(get_input_boost_ms()));
#else
	queue_delayed_work(cpu_boost_wq, &input_boost_rem,
					msecs_to_jiffies(input_boost_ms));
#endif
#else
	queue_delayed_work(cpu_boost_wq, &input_boost_rem,
					msecs_to_jiffies(input_boost_ms));
#endif

}

static void cpuboost_input_event(struct input_handle *handle,
		unsigned int type, unsigned int code, int value)
{
	u64 now;

	if (!input_boost_enabled)
		return;

	now = ktime_to_us(ktime_get());
	if (now - last_input_time < MIN_INPUT_INTERVAL)
		return;

	if (work_pending(&input_boost_work))
		return;

	queue_work(cpu_boost_wq, &input_boost_work);
	last_input_time = ktime_to_us(ktime_get());
}

static int cpuboost_input_connect(struct input_handler *handler,
		struct input_dev *dev, const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "cpufreq";

	error = input_register_handle(handle);
	if (error)
		goto err2;

	error = input_open_device(handle);
	if (error)
		goto err1;

	return 0;
err1:
	input_unregister_handle(handle);
err2:
	kfree(handle);
	return error;
}

static void cpuboost_input_disconnect(struct input_handle *handle)
{
#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	/* Reset dynamic stune boost value to the default value */
	reset_stune_boost("top-app", boost_slot);
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id cpuboost_ids[] = {
	/* multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			BIT_MASK(ABS_MT_POSITION_X) |
			BIT_MASK(ABS_MT_POSITION_Y) },
	},
	/* touchpad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) },
	},
	/* Keypad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) },
	},
	{ },
};

static struct input_handler cpuboost_input_handler = {
	.event          = cpuboost_input_event,
	.connect        = cpuboost_input_connect,
	.disconnect     = cpuboost_input_disconnect,
	.name           = "cpu-boost",
	.id_table       = cpuboost_ids,
};

static int cpu_boost_init(void)
{
	int cpu, ret;
	struct cpu_sync *s;

	cpu_boost_wq = alloc_workqueue("cpuboost_wq", WQ_HIGHPRI, 0);
	if (!cpu_boost_wq)
		return -EFAULT;

	INIT_WORK(&input_boost_work, do_input_boost);
	INIT_DELAYED_WORK(&input_boost_rem, do_input_boost_rem);

	for_each_possible_cpu(cpu) {
		s = &per_cpu(sync_info, cpu);
		s->cpu = cpu;
	}
	cpufreq_register_notifier(&boost_adjust_nb, CPUFREQ_POLICY_NOTIFIER);

	ret = input_register_handler(&cpuboost_input_handler);
#ifdef CONFIG_DYNAMIC_STUNE_BOOST
#ifdef CONFIG_UCI
        uci_add_user_listener(uci_user_listener);
#endif
#endif
	return 0;
}
late_initcall(cpu_boost_init);
