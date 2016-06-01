#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/slab.h>
#include <linux/fb.h>
#include <linux/vibtrig.h>

#define VIB_STRENGTH	20
static DEFINE_MUTEX(hb_lock);
extern struct vib_trigger *vib_trigger;

struct homebutton_data {
	struct input_dev *hb_dev;
	struct workqueue_struct *hb_input_wq;
	struct work_struct hb_input_work;
	struct notifier_block notif;
	bool key_down;
	bool scr_suspended;
	int vib_strength;
} hb_data = {
	.vib_strength = VIB_STRENGTH
};

static void hb_input_callback(struct work_struct *unused) {
	if (!mutex_trylock(&hb_lock))
		return;

	if (hb_data.key_down)
		vib_trigger_event(vib_trigger, hb_data.vib_strength);

	input_event(hb_data.hb_dev, EV_KEY, KEY_HOME, hb_data.key_down);
	input_event(hb_data.hb_dev, EV_SYN, 0, 0);

	mutex_unlock(&hb_lock);

	return;
}

static int input_dev_filter(struct input_dev *dev) {
	if (strstr(dev->name, "fpc1020")) {
		return 0;
	} else {
		return 1;
	}
}

static int hb_input_connect(struct input_handler *handler,
				struct input_dev *dev, const struct input_device_id *id) {
	int rc;
	struct input_handle *handle;

	if (input_dev_filter(dev))
		return -ENODEV;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "hb";

	rc = input_register_handle(handle);
	if (rc)
		goto err_input_register_handle;
	
	rc = input_open_device(handle);
	if (rc)
		goto err_input_open_device;

	return 0;

err_input_open_device:
	input_unregister_handle(handle);
err_input_register_handle:
	kfree(handle);
	return rc;
}

static bool hb_input_filter(struct input_handle *handle, unsigned int type, 
						unsigned int code, int value)
{
	pr_debug("hb: code: %u, val: %i\n", code, value);

	if (type != EV_KEY)
		return false;

	if (hb_data.scr_suspended) {
		pr_debug("hb - wakeup %d %d \n",code,value);
		return false;
	}      

	if (value > 0)
		hb_data.key_down = true;
	else
		hb_data.key_down = false;

	schedule_work(&hb_data.hb_input_work);

	return true;
}

static void hb_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id hb_ids[] = {
	{ .driver_info = 1 },
	{ },
};

static struct input_handler hb_input_handler = {
	.filter		= hb_input_filter,
	.connect	= hb_input_connect,
	.disconnect	= hb_input_disconnect,
	.name		= "hb_inputreq",
	.id_table	= hb_ids,
};

static int fb_notifier_callback(struct notifier_block *this,
				unsigned long event, void *data)
{
	struct fb_event *evdata = data;
	int *blank;

	if (evdata && evdata->data && event == FB_EVENT_BLANK) {
		blank = evdata->data;
		switch (*blank) {
			case FB_BLANK_UNBLANK:
				//display on
				hb_data.scr_suspended = false;
				break;
			case FB_BLANK_POWERDOWN:
			case FB_BLANK_HSYNC_SUSPEND:
			case FB_BLANK_VSYNC_SUSPEND:
			case FB_BLANK_NORMAL:
				//display off
				hb_data.scr_suspended = true;
				break;
		}
	}

	return NOTIFY_OK;
}

static int __init hb_init(void)
{
	int rc = 0;

	hb_data.hb_dev = input_allocate_device();
	if (!hb_data.hb_dev) {
		pr_err("Failed to allocate hb_dev\n");
		goto err_alloc_dev;
	}

	input_set_capability(hb_data.hb_dev, EV_KEY, KEY_HOME);
	set_bit(EV_KEY, hb_data.hb_dev->evbit);
	set_bit(KEY_HOME, hb_data.hb_dev->keybit);
	hb_data.hb_dev->name = "qwerty";
	hb_data.hb_dev->phys = "qwerty/input0";

	rc = input_register_device(hb_data.hb_dev);
	if (rc) {
		pr_err("%s: input_register_device err=%d\n", __func__, rc);
		goto err_input_dev;
	}

	rc = input_register_handler(&hb_input_handler);
	if (rc)
		pr_err("%s: Failed to register hb_input_handler\n", __func__);

	hb_data.hb_input_wq = create_workqueue("hb_wq");
	if (!hb_data.hb_input_wq) {
		pr_err("%s: Failed to create workqueue\n", __func__);
		return -EFAULT;
	}
	INIT_WORK(&hb_data.hb_input_work, hb_input_callback);

	hb_data.notif.notifier_call = fb_notifier_callback;
	if (fb_register_client(&hb_data.notif)) {
		rc = -EINVAL;
		goto err_alloc_dev;
	}	

err_input_dev:
	input_free_device(hb_data.hb_dev);

err_alloc_dev:
	pr_info("%s hb done\n", __func__);

	return 0;
}

static void __exit hb_exit(void)
{
	destroy_workqueue(hb_data.hb_input_wq);
	input_unregister_handler(&hb_input_handler);
	input_unregister_device(hb_data.hb_dev);
	input_free_device(hb_data.hb_dev);

	return;
}

module_init(hb_init);
module_exit(hb_exit);
