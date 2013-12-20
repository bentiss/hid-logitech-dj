/*
 *  HID driver for Logitech Wireless Touchpad device
 *
 *  Copyright (c) 2011 Logitech (c)
 */

/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.

 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * Should you need to contact me, the author, you can do so by e-mail send
 * your message to Benjamin Tissoires <benjamin.tissoires at gmail com>
 *
 */

#include <linux/device.h>
#include <linux/hid.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/input/mt.h>

MODULE_AUTHOR("Benjamin Tissoires <Benjamin_Tissoires@logitech.com>");
MODULE_DESCRIPTION("Logitech Wireless Touchpad");
MODULE_LICENSE("GPL");

#include "hid-ids.h"
#include "hid-logitech-dj.h"
#include "hid-logitech-hidpp.h"


struct wtp_data {
	struct input_dev *input;
	char *name;
	u16 x_size, y_size;
	u8 p_range, area_range;
	u8 finger_count;
	u8 mt_feature_index;
	u8 button_feature_index;
	u8 maxcontacts;
};

static int wtp_create_input(struct hidpp_device *hidpp_dev)
{
	struct hid_device *hdev = hidpp_dev->hid_dev;
	struct wtp_data *wd = (struct wtp_data *)hidpp_dev->driver_data;
	struct input_dev *input_dev = devm_input_allocate_device(&hdev->dev);

	if (!input_dev) {
		hid_err(hdev, "Out of memory during %s\n", __func__);
		return -ENOMEM;
	}

	input_dev->name = wd->name;
	input_dev->phys = hdev->phys;
	input_dev->uniq = hdev->uniq;
	input_dev->id.bustype = BUS_USB;
	input_dev->id.vendor  = USB_VENDOR_ID_LOGITECH;
	input_dev->id.product = DJ_DEVICE_ID_WIRELESS_TOUCHPAD;
	input_dev->id.version = 0;

	wd->input = input_dev;

	__set_bit(BTN_TOUCH, input_dev->keybit);
	__set_bit(BTN_TOOL_FINGER, input_dev->keybit);
	__set_bit(BTN_TOOL_DOUBLETAP, input_dev->keybit);
	__set_bit(BTN_TOOL_TRIPLETAP, input_dev->keybit);
	__set_bit(BTN_TOOL_QUADTAP, input_dev->keybit);

	__set_bit(EV_ABS, input_dev->evbit);

	input_mt_init_slots(input_dev, wd->maxcontacts, INPUT_MT_POINTER |
		INPUT_MT_DROP_UNUSED);

	input_set_capability(input_dev, EV_KEY, BTN_TOUCH);
	input_set_capability(input_dev, EV_KEY, BTN_LEFT);
	input_set_capability(input_dev, EV_KEY, BTN_RIGHT);

	input_set_abs_params(input_dev, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_TOUCH_MINOR, 0, 255, 0, 0);

	input_set_abs_params(input_dev, ABS_MT_POSITION_X, 0, wd->x_size, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_POSITION_Y, 0, wd->y_size, 0, 0);
	input_set_abs_params(input_dev, ABS_X, 0, wd->x_size, 0, 0);
	input_set_abs_params(input_dev, ABS_Y, 0, wd->y_size, 0, 0);

	return input_register_device(input_dev);
}

static void wtp_touch_event(struct wtp_data *wd,
	struct hidpp_touchpad_raw_xy_finger *touch_report)
{
	int slot = input_mt_get_slot_by_key(wd->input, touch_report->finger_id);

	input_mt_slot(wd->input, slot);
	input_mt_report_slot_state(wd->input, MT_TOOL_FINGER,
					touch_report->contact_status);
	if (touch_report->contact_status) {
		/* this finger is on the screen */
		/* int wide = (s->w > s->h); */
		/* int major = max(s->w, s->h) >> 1; */
		/* int minor = min(s->w, s->h) >> 1; */

		input_event(wd->input, EV_ABS, ABS_MT_POSITION_X,
				touch_report->x);
		input_event(wd->input, EV_ABS, ABS_MT_POSITION_Y,
				touch_report->y);
		/* input_event(wd->input, EV_ABS, ABS_MT_ORIENTATION, wide); */
		input_event(wd->input, EV_ABS, ABS_MT_PRESSURE,
				touch_report->area);
		/* input_event(wd->input, EV_ABS, ABS_MT_TOUCH_MAJOR, major); */
		/* input_event(wd->input, EV_ABS, ABS_MT_TOUCH_MINOR, minor); */
	}

//	pr_err("touch Type:%d Status:%d X:%d Y:%d Z:%d Area:%d I:%d\n",
//		touch_report->contact_type, touch_report->contact_status,
//		touch_report->x, touch_report->y, touch_report->z,
//		touch_report->area, touch_report->finger_id);
}

static int wtp_touchpad_raw_xy_event(struct hidpp_device *hidpp_dev,
		struct hidpp_report *report)
{
	struct hidpp_touchpad_raw_xy raw;
	struct wtp_data *wd = (struct wtp_data *)hidpp_dev->driver_data;

	if (!hidpp_dev->initialized)
		return 0;

	hidpp_touchpad_raw_xy_event(hidpp_dev, report, &raw);

	if (raw.finger_count) {
		wtp_touch_event(wd, &(raw.fingers[0]));
		if ((raw.end_of_frame && raw.finger_count == 4) ||
			(!raw.end_of_frame && raw.finger_count >= 2))
			wtp_touch_event(wd, &(raw.fingers[1]));
	}

	if (raw.end_of_frame || raw.finger_count <= 2) {
		input_mt_sync_frame(wd->input);
		input_sync(wd->input);
	}

	return 1;
}

static int wtp_raw_event(struct hidpp_device *hidpp_dev,
			 u8 *data, int size)
{
	struct wtp_data *wd = (struct wtp_data *)hidpp_dev->driver_data;
	struct hidpp_report *report = (struct hidpp_report *)data;

	if (!wd->input)
		return 1;

	if ((data[0] == REPORT_ID_HIDPP_LONG) &&
	    (report->fap.feature_index == wd->mt_feature_index) &&
	    ((report->fap.funcindex_clientid == EVENT_TOUCHPAD_RAW_XY) ||
	     (report->fap.funcindex_clientid == EVENT_TOUCHPAD_RAW_XY_))) {
		return wtp_touchpad_raw_xy_event(hidpp_dev, report);
	}

	if (data[0] == 0x02) {
		input_event(wd->input, EV_KEY, BTN_LEFT, !!(data[1] & 0x01));
		input_event(wd->input, EV_KEY, BTN_RIGHT, !!(data[1] & 0x02));
		input_sync(wd->input);
	}

	return 0;
}

static int wtp_device_init(struct hidpp_device *hidpp_device)
{
	struct hidpp_touchpad_raw_info raw_info;
	struct wtp_data *wd = (struct wtp_data *)hidpp_device->driver_data;
	u8 name_length;
	u8 feature_type;
	char *name;

	hidpp_root_get_feature(hidpp_device, HIDPP_PAGE_TOUCHPAD_RAW_XY,
		&wd->mt_feature_index, &feature_type);

	name = hidpp_get_device_name(hidpp_device, &name_length);
	if (name) {
		wd->name = devm_kzalloc(&hidpp_device->hid_dev->dev,
					name_length, GFP_KERNEL);
		if (wd->name)
			memcpy(wd->name, name, name_length);
		kfree(name);
	} else {
		wd->name = "Logitech Wireless Touchpad";
	}

	hidpp_touchpad_set_raw_report_state(hidpp_device, wd->mt_feature_index,
		true, true, true);
	hidpp_touchpad_get_raw_info(hidpp_device, wd->mt_feature_index,
		&raw_info);

	wd->x_size = raw_info.x_size;
	wd->y_size = raw_info.y_size;
	wd->maxcontacts = raw_info.maxcontacts;

	return wtp_create_input(hidpp_device);
}

static int wtp_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct dj_device *dj_dev = hdev->driver_data;
	struct wtp_data *wd;
	struct hidpp_device *hidpp_device;
	int ret;

	if (!is_dj_device(dj_dev))
		return -ENODEV;

	if (dj_dev->pid != DJ_DEVICE_ID_WIRELESS_TOUCHPAD)
		return -ENODEV;

	hidpp_device = devm_kzalloc(&hdev->dev, sizeof(struct hidpp_device),
			GFP_KERNEL);
	if (!hidpp_device) {
		hid_err(hdev, "cannot allocate hidpp_device\n");
		return -ENOMEM;
	}

	wd = devm_kzalloc(&hdev->dev, sizeof(struct wtp_data), GFP_KERNEL);
	if (!wd) {
		hid_err(hdev, "cannot allocate wtp Touch data\n");
		return -ENOMEM;
	}

	hidpp_device->driver_data = (void *)wd;

	hid_set_drvdata(hdev, hidpp_device);

	hidpp_device->device_init = wtp_device_init;
	hidpp_device->raw_event = wtp_raw_event;

	ret = hidpp_init(hidpp_device, hdev);
	if (ret) {
		hid_set_drvdata(hdev, NULL);
		return -ENODEV;
	}

	ret = hid_parse(hdev);
	if (!ret)
		ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);

	return ret;
}

static void wtp_remove(struct hid_device *hdev)
{
	struct hidpp_device *hidpp_device = hid_get_drvdata(hdev);

	hid_hw_stop(hdev);
	hid_set_drvdata(hdev, NULL);
	hidpp_remove(hidpp_device);
}

static const struct hid_device_id wtp_devices[] = {
	{HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH,
		USB_DEVICE_ID_LOGITECH_UNIFYING_RECEIVER)},
	{HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH,
		USB_DEVICE_ID_LOGITECH_UNIFYING_RECEIVER_2)},
	{ }
};
MODULE_DEVICE_TABLE(hid, wtp_devices);

static struct hid_driver wtp_driver = {
	.name = "wtp-touch",
	.id_table = wtp_devices,
	.probe = wtp_probe,
	.remove = wtp_remove,
	.raw_event = hidpp_raw_event,
};

module_hid_driver(wtp_driver)

