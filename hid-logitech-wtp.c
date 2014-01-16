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

#define WTP_MANUAL_RESOLUTION				1000

#define WTP_QUIRK_MANUAL_RESOLUTION			BIT(0)

struct wtp_data {
	struct input_dev *input;
	char *name;
	u16 x_size, y_size;
	u8 p_range, area_range;
	u8 finger_count;
	u8 mt_feature_index;
	u8 button_feature_index;
	u8 maxcontacts;
	bool flip_y;
	unsigned int resolution;
	unsigned int quirks;
};

static int wtp_create_input(struct hidpp_device *hidpp_dev)
{
	struct hid_device *hdev = hidpp_get_hiddev(hidpp_dev);
	struct wtp_data *wd = hidpp_get_drvdata(hidpp_dev);
	struct input_dev *input_dev = devm_input_allocate_device(&hdev->dev);

	if (!input_dev) {
		hid_err(hdev, "Out of memory during %s\n", __func__);
		return -ENOMEM;
	}

	input_dev->name = wd->name;
	input_dev->phys = hdev->phys;
	input_dev->uniq = hdev->uniq;
	input_dev->id.bustype = hdev->bus;
	input_dev->id.vendor  = hdev->vendor;
	input_dev->id.product = hdev->product;
	input_dev->id.version = 0;

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
	input_abs_set_res(input_dev, ABS_MT_POSITION_X, wd->resolution);
	input_set_abs_params(input_dev, ABS_MT_POSITION_Y, 0, wd->y_size, 0, 0);
	input_abs_set_res(input_dev, ABS_MT_POSITION_Y, wd->resolution);
	input_set_abs_params(input_dev, ABS_X, 0, wd->x_size, 0, 0);
	input_set_abs_params(input_dev, ABS_Y, 0, wd->y_size, 0, 0);

	wd->input = input_dev;

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
				wd->flip_y ? wd->y_size - touch_report->y :
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

static int wtp_touchpad_raw_xy_event(struct hidpp_device *hidpp_dev, u8 *data)
{
	struct hidpp_touchpad_raw_xy raw;
	struct wtp_data *wd = hidpp_get_drvdata(hidpp_dev);

	if (!wd->input)
		return 0;

	hidpp_touchpad_raw_xy_event(hidpp_dev, data, &raw);

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

static int wtp_raw_event(struct hid_device *hdev, struct hid_report *hreport,
			 u8 *data, int size)
{
	struct hidpp_device *hidpp_dev = hid_get_drvdata(hdev);
	struct wtp_data *wd = hidpp_get_drvdata(hidpp_dev);
	struct hidpp_report *report = (struct hidpp_report *)data;

	if (hidpp_raw_event(hidpp_dev, data, size))
		return 1;

	if (!wd->input)
		return 1;

	if ((data[0] == REPORT_ID_HIDPP_LONG) &&
	    (report->fap.feature_index == wd->mt_feature_index) &&
	    ((report->fap.funcindex_clientid == EVENT_TOUCHPAD_RAW_XY) ||
	     (report->fap.funcindex_clientid == EVENT_TOUCHPAD_RAW_XY_))) {
		return wtp_touchpad_raw_xy_event(hidpp_dev, report->fap.params);
	}

	if (data[0] == 0x02) {
		input_event(wd->input, EV_KEY, BTN_LEFT, !!(data[1] & 0x01));
		input_event(wd->input, EV_KEY, BTN_RIGHT, !!(data[1] & 0x02));
		input_sync(wd->input);
	}

	return 0;
}

static int wtp_init(struct hidpp_device *hidpp_dev)
{
	struct hidpp_touchpad_raw_info raw_info;
	struct wtp_data *wd = hidpp_get_drvdata(hidpp_dev);
	struct hid_device *hdev = hidpp_get_hiddev(hidpp_dev);
	u8 name_length;
	u8 feature_type;
	char *name;
	int ret;

	ret = hidpp_root_get_feature(hidpp_dev, HIDPP_PAGE_TOUCHPAD_RAW_XY,
		&wd->mt_feature_index, &feature_type);
	if (ret) {
		pr_err("%s  %s:%d\n", __func__, __FILE__, __LINE__);
		/* means that the device is not powered up */
		return ret;
	}

	name = hidpp_get_device_name(hidpp_dev, &name_length);
	if (name) {
		wd->name = devm_kzalloc(&hdev->dev, name_length, GFP_KERNEL);
		if (wd->name)
			memcpy(wd->name, name, name_length);
		kfree(name);
	} else {
		wd->name = "Logitech Wireless Touchpad";
	}

	hidpp_touchpad_set_raw_report_state(hidpp_dev, wd->mt_feature_index,
		true, true, true);
	hidpp_touchpad_get_raw_info(hidpp_dev, wd->mt_feature_index,
		&raw_info);

	wd->x_size = raw_info.x_size;
	wd->y_size = raw_info.y_size;
	wd->maxcontacts = raw_info.maxcontacts;
	wd->flip_y = raw_info.origin == TOUCHPAD_RAW_XY_ORIGIN_LOWER_LEFT;
	wd->resolution = raw_info.res;
	if (wd->quirks & WTP_QUIRK_MANUAL_RESOLUTION)
		wd->resolution = WTP_MANUAL_RESOLUTION;

	return wtp_create_input(hidpp_dev);
};

static void wtp_device_connect(struct hidpp_device *hidpp_dev, bool connected)
{
	struct wtp_data *wd = hidpp_get_drvdata(hidpp_dev);

	pr_err("%s connected: %d %s:%d\n", __func__, connected, __FILE__, __LINE__);

	if (wd->input || !connected)
		return;

	wtp_init(hidpp_dev);
}

static int wtp_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct wtp_data *wd;
	struct hidpp_device *hidpp_dev;
	int ret;
	/* hunk for backport only ---> */

	u16 product_id = id->product;
	struct dj_device *dj_dev = hdev->driver_data;

	/* <--- hunk for backport only */

	hidpp_dev = devm_hidpp_allocate(hdev);
	if (!hidpp_dev) {
		hid_err(hdev, "cannot allocate hidpp_dev\n");
		return -ENOMEM;
	}

	wd = devm_kzalloc(&hdev->dev, sizeof(struct wtp_data), GFP_KERNEL);
	if (!wd) {
		hid_err(hdev, "cannot allocate wtp Touch data\n");
		return -ENOMEM;
	}

	wd->quirks = id->driver_data;

	/* hunk for backport only ---> */

	if (dj_dev)
		product_id = dj_dev->wpid;

	switch (product_id) {
	case DJ_DEVICE_ID_WIRELESS_TOUCHPAD:
		wd->quirks = WTP_QUIRK_MANUAL_RESOLUTION;
		break;
	};

	/* <--- hunk for backport only */

	hidpp_set_drvdata(hidpp_dev, wd);

	hid_set_drvdata(hdev, hidpp_dev);

	hidpp_dev->device_connect = wtp_device_connect;

	ret = hid_parse(hdev);
	if (!ret)
		ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);

	/* try init */
	hid_device_io_start(hdev);
	if (wtp_init(hidpp_dev))
		hid_dbg(hdev, "wtp_init returned an error, postponing the input creation until the device connects.");

	return ret;
}

static const struct hid_device_id wtp_devices[] = {
	{ HID_DEVICE(BUS_USB, HID_GROUP_LOGITECH_DJ_DEVICE_WTP,
		USB_VENDOR_ID_LOGITECH, DJ_DEVICE_ID_WIRELESS_TOUCHPAD),
		.driver_data = WTP_QUIRK_MANUAL_RESOLUTION},
	{ HID_DEVICE(BUS_USB, HID_GROUP_LOGITECH_DJ_DEVICE_WTP,
		USB_VENDOR_ID_LOGITECH, DJ_DEVICE_ID_WIRELESS_TOUCHPAD_T650)},
	/* hunk for backport only ---> */

	{ HID_DEVICE(BUS_USB, HID_GROUP_LOGITECH_DJ_DEVICE_WTP,
		USB_VENDOR_ID_LOGITECH, HID_ANY_ID)},

	/* <--- hunk for backport only */
	{ }
};
MODULE_DEVICE_TABLE(hid, wtp_devices);

static struct hid_driver wtp_driver = {
	.name = "wtp-touch",
	.id_table = wtp_devices,
	.probe = wtp_probe,
	.raw_event = wtp_raw_event,
};

module_hid_driver(wtp_driver)
