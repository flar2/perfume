/*
 * drivers/base/power/wakeup.c - System wakeup events framework
 *
 * Copyright (c) 2010 Rafael J. Wysocki <rjw@sisk.pl>, Novell Inc.
 *
 * This file is released under the GPLv2.
 */

#include <linux/device.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/capability.h>
#include <linux/export.h>
#include <linux/suspend.h>
#include <linux/seq_file.h>
#include <linux/debugfs.h>
#include <linux/types.h>
#include <trace/events/power.h>
#include <linux/moduleparam.h>

#include "power.h"
#include <soc/qcom/htc_util.h>

static bool enable_ipa_ws = true;
module_param(enable_ipa_ws, bool, 0644);
static bool enable_wlan_wake = true;
module_param(enable_wlan_wake, bool, 0644);
static bool enable_timerfd_ws = true;
module_param(enable_timerfd_ws, bool, 0644);
static bool enable_netlink_ws = true;
module_param(enable_netlink_ws, bool, 0644);
static bool enable_netmgr_wl_ws = true;
module_param(enable_netmgr_wl_ws, bool, 0644);

bool events_check_enabled __read_mostly;

static bool pm_abort_suspend __read_mostly;

static atomic_t combined_event_count = ATOMIC_INIT(0);

#define IN_PROGRESS_BITS	(sizeof(int) * 4)
#define MAX_IN_PROGRESS		((1 << IN_PROGRESS_BITS) - 1)

static void split_counters(unsigned int *cnt, unsigned int *inpr)
{
	unsigned int comb = atomic_read(&combined_event_count);

	*cnt = (comb >> IN_PROGRESS_BITS);
	*inpr = comb & MAX_IN_PROGRESS;
}

static unsigned int saved_count;

static DEFINE_SPINLOCK(events_lock);

static void pm_wakeup_timer_fn(unsigned long data);

static LIST_HEAD(wakeup_sources);

static DECLARE_WAIT_QUEUE_HEAD(wakeup_count_wait_queue);

void wakeup_source_prepare(struct wakeup_source *ws, const char *name)
{
	if (ws) {
		memset(ws, 0, sizeof(*ws));
		ws->name = name;
	}
}
EXPORT_SYMBOL_GPL(wakeup_source_prepare);

struct wakeup_source *wakeup_source_create(const char *name)
{
	struct wakeup_source *ws;

	ws = kmalloc(sizeof(*ws), GFP_KERNEL);
	if (!ws)
		return NULL;

	wakeup_source_prepare(ws, name ? kstrdup(name, GFP_KERNEL) : NULL);
	return ws;
}
EXPORT_SYMBOL_GPL(wakeup_source_create);

void wakeup_source_drop(struct wakeup_source *ws)
{
	if (!ws)
		return;

	del_timer_sync(&ws->timer);
	__pm_relax(ws);
}
EXPORT_SYMBOL_GPL(wakeup_source_drop);

void wakeup_source_destroy(struct wakeup_source *ws)
{
	if (!ws)
		return;

	wakeup_source_drop(ws);
	kfree(ws->name);
	kfree(ws);
}
EXPORT_SYMBOL_GPL(wakeup_source_destroy);

static void wakeup_source_destroy_cb(struct rcu_head *head)
{
	wakeup_source_destroy(container_of(head, struct wakeup_source, rcu));
}

void wakeup_source_add(struct wakeup_source *ws)
{
	unsigned long flags;

	if (WARN_ON(!ws))
		return;

	spin_lock_init(&ws->lock);
	setup_timer(&ws->timer, pm_wakeup_timer_fn, (unsigned long)ws);
	ws->active = false;
	ws->last_time = ktime_get();

	spin_lock_irqsave(&events_lock, flags);
	list_add_rcu(&ws->entry, &wakeup_sources);
	spin_unlock_irqrestore(&events_lock, flags);
}
EXPORT_SYMBOL_GPL(wakeup_source_add);

void wakeup_source_remove(struct wakeup_source *ws)
{
	unsigned long flags;

	if (WARN_ON(!ws))
		return;

	spin_lock_irqsave(&events_lock, flags);
	list_del_rcu(&ws->entry);
	spin_unlock_irqrestore(&events_lock, flags);
	synchronize_rcu();
}
EXPORT_SYMBOL_GPL(wakeup_source_remove);

static void wakeup_source_remove_async(struct wakeup_source *ws)
{
	unsigned long flags;

	if (WARN_ON(!ws))
		return;

	spin_lock_irqsave(&events_lock, flags);
	list_del_rcu(&ws->entry);
	spin_unlock_irqrestore(&events_lock, flags);
}

struct wakeup_source *wakeup_source_register(const char *name)
{
	struct wakeup_source *ws;

	ws = wakeup_source_create(name);
	if (ws)
		wakeup_source_add(ws);

	return ws;
}
EXPORT_SYMBOL_GPL(wakeup_source_register);

void wakeup_source_unregister(struct wakeup_source *ws)
{
	if (ws) {
		wakeup_source_remove_async(ws);
		call_rcu(&ws->rcu, wakeup_source_destroy_cb);
	}
}
EXPORT_SYMBOL_GPL(wakeup_source_unregister);

static int device_wakeup_attach(struct device *dev, struct wakeup_source *ws)
{
	spin_lock_irq(&dev->power.lock);
	if (dev->power.wakeup) {
		spin_unlock_irq(&dev->power.lock);
		return -EEXIST;
	}
	dev->power.wakeup = ws;
	spin_unlock_irq(&dev->power.lock);
	return 0;
}

int device_wakeup_enable(struct device *dev)
{
	struct wakeup_source *ws;
	int ret;

	if (!dev || !dev->power.can_wakeup)
		return -EINVAL;

	ws = wakeup_source_register(dev_name(dev));
	if (!ws)
		return -ENOMEM;

	ret = device_wakeup_attach(dev, ws);
	if (ret)
		wakeup_source_unregister(ws);

	return ret;
}
EXPORT_SYMBOL_GPL(device_wakeup_enable);

static struct wakeup_source *device_wakeup_detach(struct device *dev)
{
	struct wakeup_source *ws;

	spin_lock_irq(&dev->power.lock);
	ws = dev->power.wakeup;
	dev->power.wakeup = NULL;
	spin_unlock_irq(&dev->power.lock);
	return ws;
}

int device_wakeup_disable(struct device *dev)
{
	struct wakeup_source *ws;

	if (!dev || !dev->power.can_wakeup)
		return -EINVAL;

	ws = device_wakeup_detach(dev);
	if (ws)
		wakeup_source_unregister(ws);

	return 0;
}
EXPORT_SYMBOL_GPL(device_wakeup_disable);

void device_set_wakeup_capable(struct device *dev, bool capable)
{
	if (!!dev->power.can_wakeup == !!capable)
		return;

	if (device_is_registered(dev) && !list_empty(&dev->power.entry)) {
		if (capable) {
			if (wakeup_sysfs_add(dev))
				return;
		} else {
			wakeup_sysfs_remove(dev);
		}
	}
	dev->power.can_wakeup = capable;
}
EXPORT_SYMBOL_GPL(device_set_wakeup_capable);

int device_init_wakeup(struct device *dev, bool enable)
{
	int ret = 0;

	if (!dev)
		return -EINVAL;

	if (enable) {
		device_set_wakeup_capable(dev, true);
		ret = device_wakeup_enable(dev);
	} else {
		if (dev->power.can_wakeup)
			device_wakeup_disable(dev);

		device_set_wakeup_capable(dev, false);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(device_init_wakeup);

int device_set_wakeup_enable(struct device *dev, bool enable)
{
	if (!dev || !dev->power.can_wakeup)
		return -EINVAL;

	return enable ? device_wakeup_enable(dev) : device_wakeup_disable(dev);
}
EXPORT_SYMBOL_GPL(device_set_wakeup_enable);

#ifdef CONFIG_PM_AUTOSLEEP
static void update_prevent_sleep_time(struct wakeup_source *ws, ktime_t now)
{
	ktime_t delta = ktime_sub(now, ws->start_prevent_time);
	ws->prevent_sleep_time = ktime_add(ws->prevent_sleep_time, delta);
}
#else
static inline void update_prevent_sleep_time(struct wakeup_source *ws,
					     ktime_t now) {}
#endif

/**
 * wakup_source_deactivate - Mark given wakeup source as inactive.
 * @ws: Wakeup source to handle.
 *
 * Update the @ws' statistics and notify the PM core that the wakeup source has
 * become inactive by decrementing the counter of wakeup events being processed
 * and incrementing the counter of registered wakeup events.
 */
static void wakeup_source_deactivate(struct wakeup_source *ws)
{
	unsigned int cnt, inpr, cec;
	ktime_t duration;
	ktime_t now;

	ws->relax_count++;
	/*
	 * __pm_relax() may be called directly or from a timer function.
	 * If it is called directly right after the timer function has been
	 * started, but before the timer function calls __pm_relax(), it is
	 * possible that __pm_stay_awake() will be called in the meantime and
	 * will set ws->active.  Then, ws->active may be cleared immediately
	 * by the __pm_relax() called from the timer function, but in such a
	 * case ws->relax_count will be different from ws->active_count.
	 */
	if (ws->relax_count != ws->active_count) {
		ws->relax_count--;
		return;
	}

	ws->active = false;

	now = ktime_get();
	duration = ktime_sub(now, ws->last_time);
	ws->total_time = ktime_add(ws->total_time, duration);
	if (ktime_to_ns(duration) > ktime_to_ns(ws->max_time))
		ws->max_time = duration;

	ws->last_time = now;
	del_timer(&ws->timer);
	ws->timer_expires = 0;

	if (ws->autosleep_enabled)
		update_prevent_sleep_time(ws, now);

	/*
	 * Increment the counter of registered wakeup events and decrement the
	 * couter of wakeup events in progress simultaneously.
	 */
	cec = atomic_add_return(MAX_IN_PROGRESS, &combined_event_count);
	trace_wakeup_source_deactivate(ws->name, cec);

	split_counters(&cnt, &inpr);
	if (!inpr && waitqueue_active(&wakeup_count_wait_queue))
		wake_up(&wakeup_count_wait_queue);
}

/*
 * The functions below use the observation that each wakeup event starts a
 * period in which the system should not be suspended.  The moment this period
 * will end depends on how the wakeup event is going to be processed after being
 * detected and all of the possible cases can be divided into two distinct
 * groups.
 *
 * First, a wakeup event may be detected by the same functional unit that will
 * carry out the entire processing of it and possibly will pass it to user space
 * for further processing.  In that case the functional unit that has detected
 * the event may later "close" the "no suspend" period associated with it
 * directly as soon as it has been dealt with.  The pair of pm_stay_awake() and
 * pm_relax(), balanced with each other, is supposed to be used in such
 * situations.
 *
 * Second, a wakeup event may be detected by one functional unit and processed
 * by another one.  In that case the unit that has detected it cannot really
 * "close" the "no suspend" period associated with it, unless it knows in
 * advance what's going to happen to the event during processing.  This
 * knowledge, however, may not be available to it, so it can simply specify time
 * to wait before the system can be suspended and pass it as the second
 * argument of pm_wakeup_event().
 *
 * It is valid to call pm_relax() after pm_wakeup_event(), in which case the
 * "no suspend" period will be ended either by the pm_relax(), or by the timer
 * function executed when the timer expires, whichever comes first.
 */

/**
 * wakup_source_activate - Mark given wakeup source as active.
 * @ws: Wakeup source to handle.
 *
 * Update the @ws' statistics and, if @ws has just been activated, notify the PM
 * core of the event by incrementing the counter of of wakeup events being
 * processed.
 */
static void wakeup_source_activate(struct wakeup_source *ws)
{
	unsigned int cec;

	if ((!enable_ipa_ws && !strncmp(ws->name, "IPA_WS", 6)) ||		
		(!enable_wlan_wake &&
                        !strncmp(ws->name, "wlan_wake", 9)) ||
		(!enable_timerfd_ws &&
                        !strncmp(ws->name, "[timerfd]", 9)) ||
		(!enable_netlink_ws &&
                        !strncmp(ws->name, "NETLINK", 7)) ||
		(!enable_netmgr_wl_ws &&
                        !strncmp(ws->name, "netmgr_wl", 9))) {
		if (ws->active)
			wakeup_source_deactivate(ws);

		return;
}

	/*
	 * active wakeup source should bring the system
	 * out of PM_SUSPEND_FREEZE state
	 */
	freeze_wake();

	ws->active = true;
	ws->active_count++;
	ws->last_time = ktime_get();
	if (ws->autosleep_enabled)
		ws->start_prevent_time = ws->last_time;

	
	cec = atomic_inc_return(&combined_event_count);

	trace_wakeup_source_activate(ws->name, cec);
}

static void wakeup_source_report_event(struct wakeup_source *ws)
{
	ws->event_count++;
	
	if (events_check_enabled)
		ws->wakeup_count++;

	if (!ws->active)
		wakeup_source_activate(ws);
}

#ifdef CONFIG_PM_DEBUG
extern char wakelock_debug_buf[];
#endif

void __pm_stay_awake(struct wakeup_source *ws)
{
	unsigned long flags;

	if (!ws)
		return;

#ifdef CONFIG_PM_DEBUG
	if (!strncmp(ws->name, wakelock_debug_buf, sizeof(ws->name)-1)) {
		pr_err("%s PID: %i requests wakelock %s", current->comm, current->pid, ws->name);
		dump_stack();
	}
#endif

	spin_lock_irqsave(&ws->lock, flags);

	wakeup_source_report_event(ws);
	del_timer(&ws->timer);
	ws->timer_expires = 0;

	spin_unlock_irqrestore(&ws->lock, flags);
}
EXPORT_SYMBOL_GPL(__pm_stay_awake);

void pm_stay_awake(struct device *dev)
{
	unsigned long flags;

	if (!dev)
		return;

	spin_lock_irqsave(&dev->power.lock, flags);
	__pm_stay_awake(dev->power.wakeup);
	spin_unlock_irqrestore(&dev->power.lock, flags);
}
EXPORT_SYMBOL_GPL(pm_stay_awake);

void __pm_relax(struct wakeup_source *ws)
{
	unsigned long flags;

	if (!ws)
		return;

	spin_lock_irqsave(&ws->lock, flags);
	if (ws->active)
		wakeup_source_deactivate(ws);
	spin_unlock_irqrestore(&ws->lock, flags);
}
EXPORT_SYMBOL_GPL(__pm_relax);

void pm_relax(struct device *dev)
{
	unsigned long flags;

	if (!dev)
		return;

	spin_lock_irqsave(&dev->power.lock, flags);
	__pm_relax(dev->power.wakeup);
	spin_unlock_irqrestore(&dev->power.lock, flags);
}
EXPORT_SYMBOL_GPL(pm_relax);

static void pm_wakeup_timer_fn(unsigned long data)
{
	struct wakeup_source *ws = (struct wakeup_source *)data;
	unsigned long flags;

	spin_lock_irqsave(&ws->lock, flags);

	if (ws->active && ws->timer_expires
	    && time_after_eq(jiffies, ws->timer_expires)) {
		wakeup_source_deactivate(ws);
		ws->expire_count++;
	}

	spin_unlock_irqrestore(&ws->lock, flags);
}

void __pm_wakeup_event(struct wakeup_source *ws, unsigned int msec)
{
	unsigned long flags;
	unsigned long expires;

	if (!ws)
		return;

	spin_lock_irqsave(&ws->lock, flags);

	wakeup_source_report_event(ws);

	if (!msec) {
		wakeup_source_deactivate(ws);
		goto unlock;
	}

	expires = jiffies + msecs_to_jiffies(msec);
	if (!expires)
		expires = 1;

	if (!ws->timer_expires || time_after(expires, ws->timer_expires)) {
		mod_timer(&ws->timer, expires);
		ws->timer_expires = expires;
	}

 unlock:
	spin_unlock_irqrestore(&ws->lock, flags);
}
EXPORT_SYMBOL_GPL(__pm_wakeup_event);


void pm_wakeup_event(struct device *dev, unsigned int msec)
{
	unsigned long flags;

	if (!dev)
		return;

	spin_lock_irqsave(&dev->power.lock, flags);
	__pm_wakeup_event(dev->power.wakeup, msec);
	spin_unlock_irqrestore(&dev->power.lock, flags);
}
EXPORT_SYMBOL_GPL(pm_wakeup_event);

void pm_get_active_wakeup_sources(char *pending_wakeup_source, size_t max)
{
	struct wakeup_source *ws, *last_active_ws = NULL;
	int len = 0;
	bool active = false;

	rcu_read_lock();
	list_for_each_entry_rcu(ws, &wakeup_sources, entry) {
		if (ws->active && len < max) {
			if (!active)
				len += scnprintf(pending_wakeup_source, max,
						"Pending Wakeup Sources: ");
			len += scnprintf(pending_wakeup_source + len, max - len,
				"%s ", ws->name);
			active = true;
		} else if (!active &&
			   (!last_active_ws ||
			    ktime_to_ns(ws->last_time) >
			    ktime_to_ns(last_active_ws->last_time))) {
			last_active_ws = ws;
		}
	}
	if (!active && last_active_ws) {
		scnprintf(pending_wakeup_source, max,
				"Last active Wakeup Source: %s",
				last_active_ws->name);
	}
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(pm_get_active_wakeup_sources);

void pm_print_active_wakeup_sources(void)
{
	struct wakeup_source *ws;
	int active = 0;
	struct wakeup_source *last_activity_ws = NULL;

	rcu_read_lock();
	list_for_each_entry_rcu(ws, &wakeup_sources, entry) {
		if (ws->active) {
			pr_info("active wakeup source: %s\n", ws->name);
			active = 1;
		} else if (!active &&
			   (!last_activity_ws ||
			    ktime_to_ns(ws->last_time) >
			    ktime_to_ns(last_activity_ws->last_time))) {
			last_activity_ws = ws;
		}
	}

	if (!active && last_activity_ws)
		pr_info("last active wakeup source: %s\n",
			last_activity_ws->name);
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(pm_print_active_wakeup_sources);

bool pm_wakeup_pending(void)
{
	unsigned long flags;
	bool ret = false;

	spin_lock_irqsave(&events_lock, flags);
	if (events_check_enabled) {
		unsigned int cnt, inpr;

		split_counters(&cnt, &inpr);
		ret = (cnt != saved_count || inpr > 0);
		events_check_enabled = !ret;
	}
	spin_unlock_irqrestore(&events_lock, flags);

	if (ret) {
		pr_info("PM: Wakeup pending, aborting suspend\n");
		pm_print_active_wakeup_sources();
	}

	return ret || pm_abort_suspend;
}

void pm_system_wakeup(void)
{
	pm_abort_suspend = true;
	freeze_wake();
}

void pm_wakeup_clear(void)
{
	pm_abort_suspend = false;
}

bool pm_get_wakeup_count(unsigned int *count, bool block)
{
	unsigned int cnt, inpr;

	if (block) {
		DEFINE_WAIT(wait);

		for (;;) {
			prepare_to_wait(&wakeup_count_wait_queue, &wait,
					TASK_INTERRUPTIBLE);
			split_counters(&cnt, &inpr);
			if (inpr == 0 || signal_pending(current))
				break;

			schedule();
		}
		finish_wait(&wakeup_count_wait_queue, &wait);
	}

	split_counters(&cnt, &inpr);
	*count = cnt;
	return !inpr;
}

bool pm_save_wakeup_count(unsigned int count)
{
	unsigned int cnt, inpr;
	unsigned long flags;

	events_check_enabled = false;
	spin_lock_irqsave(&events_lock, flags);
	split_counters(&cnt, &inpr);
	if (cnt == count && inpr == 0) {
		saved_count = count;
		events_check_enabled = true;
	}
	spin_unlock_irqrestore(&events_lock, flags);
	return events_check_enabled;
}

#ifdef CONFIG_PM_AUTOSLEEP
void pm_wakep_autosleep_enabled(bool set)
{
	struct wakeup_source *ws;
	ktime_t now = ktime_get();

	rcu_read_lock();
	list_for_each_entry_rcu(ws, &wakeup_sources, entry) {
		spin_lock_irq(&ws->lock);
		if (ws->autosleep_enabled != set) {
			ws->autosleep_enabled = set;
			if (ws->active) {
				if (set)
					ws->start_prevent_time = now;
				else
					update_prevent_sleep_time(ws, now);
			}
		}
		spin_unlock_irq(&ws->lock);
	}
	rcu_read_unlock();
}
#endif 

static struct dentry *wakeup_sources_stats_dentry;

static int print_wakeup_source_stats(struct seq_file *m,
				     struct wakeup_source *ws)
{
	unsigned long flags;
	ktime_t total_time;
	ktime_t max_time;
	unsigned long active_count;
	ktime_t active_time;
	ktime_t prevent_sleep_time;
	int ret;

	spin_lock_irqsave(&ws->lock, flags);

	total_time = ws->total_time;
	max_time = ws->max_time;
	prevent_sleep_time = ws->prevent_sleep_time;
	active_count = ws->active_count;
	if (ws->active) {
		ktime_t now = ktime_get();

		active_time = ktime_sub(now, ws->last_time);
		total_time = ktime_add(total_time, active_time);
		if (active_time.tv64 > max_time.tv64)
			max_time = active_time;

		if (ws->autosleep_enabled)
			prevent_sleep_time = ktime_add(prevent_sleep_time,
				ktime_sub(now, ws->start_prevent_time));
	} else {
		active_time = ktime_set(0, 0);
	}

	ret = seq_printf(m, "%-12s\t%lu\t\t%lu\t\t%lu\t\t%lu\t\t"
			"%lld\t\t%lld\t\t%lld\t\t%lld\t\t%lld\n",
			ws->name, active_count, ws->event_count,
			ws->wakeup_count, ws->expire_count,
			ktime_to_ms(active_time), ktime_to_ms(total_time),
			ktime_to_ms(max_time), ktime_to_ms(ws->last_time),
			ktime_to_ms(prevent_sleep_time));

	spin_unlock_irqrestore(&ws->lock, flags);

	return ret;
}

#ifdef CONFIG_HTC_POWER_DEBUG
void htc_print_active_wakeup_sources(bool print_embedded)
{
        struct wakeup_source *ws;
        char output[512];
        char piece[64];

        rcu_read_lock();
        memset(output, 0, sizeof(output));
        list_for_each_entry_rcu(ws, &wakeup_sources, entry) {
            if (ws->active) {
                memset(piece, 0, sizeof(piece));
                if (ws->timer_expires) {
                    long timeout = ws->timer_expires - jiffies;
                    if (timeout > 0) {
                        snprintf(piece, sizeof(piece), " '%s', time left %ld ticks; ", ws->name, timeout);
                        safe_strcat(output, piece);
                    }
                } else {
                    snprintf(piece, sizeof(piece), " '%s' ", ws->name);
                    safe_strcat(output, piece);
                }
            }
        }
        rcu_read_unlock();
        k_pr_embedded_cond(print_embedded, "[K] wakeup sources: %s\n", output);
}
#endif

static int wakeup_sources_stats_show(struct seq_file *m, void *unused)
{
	struct wakeup_source *ws;

	seq_puts(m, "name\t\tactive_count\tevent_count\twakeup_count\t"
		"expire_count\tactive_since\ttotal_time\tmax_time\t"
		"last_change\tprevent_suspend_time\n");

	rcu_read_lock();
	list_for_each_entry_rcu(ws, &wakeup_sources, entry)
		print_wakeup_source_stats(m, ws);
	rcu_read_unlock();

	return 0;
}

static int wakeup_sources_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, wakeup_sources_stats_show, NULL);
}

static const struct file_operations wakeup_sources_stats_fops = {
	.owner = THIS_MODULE,
	.open = wakeup_sources_stats_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int __init wakeup_sources_debugfs_init(void)
{
	wakeup_sources_stats_dentry = debugfs_create_file("wakeup_sources",
			S_IRUGO, NULL, NULL, &wakeup_sources_stats_fops);
	return 0;
}

postcore_initcall(wakeup_sources_debugfs_init);
