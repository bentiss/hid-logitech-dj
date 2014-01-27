/*
 *  HIDPP protocol for Logitech Unifying receivers
 *
 *  Copyright (c) 2011 Logitech (c)
 *  Copyright (c) 2012-2013 Google (c)
 *  Copyright (c) 2013 Red Hat Inc.
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
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/device.h>
#include <linux/hid.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include "hid-logitech-hidpp.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Benjamin Tissoires <benjamin.tissoires@gmail.com>");
MODULE_AUTHOR("Nestor Lopez Casado <nlopezcasad@logitech.com>");

	/* hunk for backport only ---> */

#define hid_hw_request hidpp_hid_hw_request

static void hidpp_hid_hw_request(struct hid_device *hdev, struct hid_report *rep,
		int reqtype)
{
	char *buf;
	int len = ((rep->size - 1) >> 3) + 2;

	if (hdev->ll_driver->request) {
		hdev->ll_driver->request(hdev, rep, reqtype);
		return;
	}

	if (!hdev->hid_output_raw_report)
		return;

	buf = kzalloc(len, GFP_KERNEL);
	if (!buf)
		return;

	switch (reqtype) {
	case HID_REQ_GET_REPORT:
		break;
	case HID_REQ_SET_REPORT:
		hid_output_report(rep, buf);
		hdev->hid_output_raw_report(hdev, buf, len, rep->type);
		break;
	}

	kfree(buf);
}

	/* <--- hunk for backport only */

static int __hidpp_send_report(struct hid_device *hdev,
				struct hidpp_report *hidpp_report)
{
	struct hid_report* report;
	struct hid_report_enum *output_report_enum;
	int i, fields_count;

	switch (hidpp_report->report_id) {
	case REPORT_ID_HIDPP_SHORT:
		fields_count = HIDPP_REPORT_SHORT_LENGTH - 2;
		break;
	case REPORT_ID_HIDPP_LONG:
		fields_count = HIDPP_REPORT_LONG_LENGTH - 2;
		break;
	default:
		return -ENODEV;
	}

	/*
	 * set the device_index as the receiver, it will be overwritten by
	 * hid_hw_request if needed
	 */
	hidpp_report->device_index = 0xff;

	output_report_enum = &hdev->report_enum[HID_OUTPUT_REPORT];
	report = output_report_enum->report_id_hash[hidpp_report->report_id];

	hid_set_field(report->field[0], 0, hidpp_report->device_index);
	for (i = 0; i < fields_count; i++)
		hid_set_field(report->field[0], i+1, hidpp_report->rawbytes[i]);

	hid_hw_request(hdev, report, HID_REQ_SET_REPORT);

	return 0;
}

static int hidpp_send_message_sync(struct hidpp_device *hidpp_dev,
	struct hidpp_report *message,
	struct hidpp_report *response)
{
	int ret;

	mutex_lock(&hidpp_dev->send_mutex);

	hidpp_dev->send_receive_buf = response;
	hidpp_dev->answer_available = false;

	/*
	 * So that we can later validate the answer when it arrives
	 * in hidpp_raw_event
	 */
	*response = *message;

	ret = __hidpp_send_report(hidpp_dev->hid_dev, message);

	if (ret) {
		dbg_hid("__hidpp_send_report returned err: %d\n", ret);
		memset(response, 0, sizeof(struct hidpp_report));
		goto exit;
	}

	if (!wait_event_timeout(hidpp_dev->wait, hidpp_dev->answer_available,
				5*HZ)) {
		dbg_hid("%s:timeout waiting for response\n", __func__);
		memset(response, 0, sizeof(struct hidpp_report));
		ret = -ETIMEDOUT;
	}

	if (response->report_id == REPORT_ID_HIDPP_SHORT &&
	    response->fap.feature_index == HIDPP_ERROR) {
		ret = response->fap.params[1];
		dbg_hid("__hidpp_send_report got hidpp error %02X\n", ret);
		goto exit;
	}

exit:
	mutex_unlock(&hidpp_dev->send_mutex);
	return ret;

}

int hidpp_send_fap_command_sync(struct hidpp_device *hidpp_dev,
	u8 feat_index, u8 funcindex_clientid, u8 *params, int param_count,
	struct hidpp_report *response)
{
	struct hidpp_report message;

	if (param_count > sizeof(message.fap.params))
		return -EINVAL;

	memset(&message, 0, sizeof(message));
	message.report_id = REPORT_ID_HIDPP_LONG;
	message.fap.feature_index = feat_index;
	message.fap.funcindex_clientid = funcindex_clientid;
	memcpy(&message.fap.params, params, param_count);

	return hidpp_send_message_sync(hidpp_dev, &message, response);
}
EXPORT_SYMBOL_GPL(hidpp_send_fap_command_sync);

int hidpp_send_rap_command_sync(struct hidpp_device *hidpp_dev,
	u8 report_id, u8 sub_id, u8 reg_address, u8 *params, int param_count,
	struct hidpp_report *response)
{
	struct hidpp_report message;

	if ((report_id != REPORT_ID_HIDPP_SHORT) &&
	    (report_id != REPORT_ID_HIDPP_LONG))
		return -EINVAL;

	if (param_count > sizeof(message.rap.params))
		return -EINVAL;

	memset(&message, 0, sizeof(message));
	message.report_id = report_id;
	message.rap.sub_id = sub_id;
	message.rap.reg_address = reg_address;
	memcpy(&message.rap.params, params, param_count);

	return hidpp_send_message_sync(hidpp_dev, &message, response);
}
EXPORT_SYMBOL_GPL(hidpp_send_rap_command_sync);

static void schedule_delayed_hidpp_connect(struct hidpp_device *hidpp_dev,
		bool connected)
{
	kfifo_in(&hidpp_dev->delayed_work_fifo, &connected, sizeof(bool));

	if (schedule_work(&hidpp_dev->work) == 0) {
		dbg_hid("%s: did not schedule the work item, was already queued\n",
			__func__);
	}
}

static void delayed_work_cb(struct work_struct *work)
{
	struct hidpp_device *hidpp_dev = container_of(work, struct hidpp_device,
							work);
	unsigned long flags;
	int count;
	bool connected;

	spin_lock_irqsave(&hidpp_dev->delayed_work_lock, flags);

	count = kfifo_out(&hidpp_dev->delayed_work_fifo,
			  &connected, sizeof(bool));

	if (count != sizeof(bool)) {
		dev_err(&hidpp_dev->hid_dev->dev,
			"%s: workitem triggered without notifications available\n",
			__func__);
		spin_unlock_irqrestore(&hidpp_dev->delayed_work_lock, flags);
		return;
	}

	if (!kfifo_is_empty(&hidpp_dev->delayed_work_fifo)) {
		if (schedule_work(&hidpp_dev->work) == 0)
			dbg_hid("%s: did not schedule the work item, was already queued\n",
				__func__);
	}

	spin_unlock_irqrestore(&hidpp_dev->delayed_work_lock, flags);

	hidpp_dev->device_connect(hidpp_dev, connected);
}

static int hidpp_init(struct hidpp_device *hidpp_dev, struct hid_device *hid_dev)
{
	int ret;
	hidpp_dev->hid_dev = hid_dev;

	INIT_WORK(&hidpp_dev->work, delayed_work_cb);
	mutex_init(&hidpp_dev->send_mutex);
	init_waitqueue_head(&hidpp_dev->wait);

	spin_lock_init(&hidpp_dev->delayed_work_lock);
	ret = kfifo_alloc(&hidpp_dev->delayed_work_fifo,
			4 * sizeof(struct hidpp_report),
			GFP_KERNEL);
	if (ret) {
		dev_err(&hidpp_dev->hid_dev->dev,
			"%s:failed allocating delayed_work_fifo\n", __func__);
		mutex_destroy(&hidpp_dev->send_mutex);
		return ret;
	}

	return 0;
}

static struct hidpp_device *hidpp_allocate(struct hid_device *hdev)
{
	struct hidpp_device *hidpp_dev;
	int ret;

	hidpp_dev = kzalloc(sizeof(struct hidpp_device), GFP_KERNEL);
	if (!hidpp_dev)
		return NULL;

	ret = hidpp_init(hidpp_dev, hdev);
	if (ret)
		goto out_fail;

	return hidpp_dev;

out_fail:
	kfree(hidpp_dev);
	return NULL;
}

static void hidpp_remove(struct hidpp_device *hidpp_dev)
{
	cancel_work_sync(&hidpp_dev->work);
	mutex_destroy(&hidpp_dev->send_mutex);
	kfifo_free(&hidpp_dev->delayed_work_fifo);
	hidpp_dev->hid_dev = NULL;
}

struct hidpp_devres {
	struct hidpp_device *hidpp_dev;
};

static void devm_hidpp_device_release(struct device *dev, void *res)
{
	struct hidpp_devres *devres = res;

	hidpp_remove(devres->hidpp_dev);
	kfree(devres->hidpp_dev);
}

struct hidpp_device *devm_hidpp_allocate(struct hid_device *hdev)
{
	struct hidpp_device *hidpp_dev;

	struct hidpp_devres *devres;

	devres = devres_alloc(devm_hidpp_device_release,
			      sizeof(struct hidpp_devres), GFP_KERNEL);
	if (!devres)
		return NULL;

	hidpp_dev = hidpp_allocate(hdev);
	if (!hidpp_dev) {
		devres_free(devres);
		return NULL;
	}

	devres->hidpp_dev = hidpp_dev;
	devres_add(&hdev->dev, devres);

	return hidpp_dev;
}
EXPORT_SYMBOL_GPL(devm_hidpp_allocate);

static inline bool hidpp_match_answer(struct hidpp_report *question,
		struct hidpp_report *answer)
{
	return (answer->fap.feature_index == question->fap.feature_index) &&
	   (answer->fap.funcindex_clientid == question->fap.funcindex_clientid);
}

static inline bool hidpp_match_error(struct hidpp_report *question,
		struct hidpp_report *answer)
{
	return (answer->fap.feature_index == HIDPP_ERROR) &&
	    (answer->fap.funcindex_clientid == question->fap.feature_index) &&
	    (answer->fap.params[0] == question->fap.funcindex_clientid);
}

static inline bool hidpp_report_is_connect_event(struct hidpp_report *report)
{
	return (report->report_id == REPORT_ID_HIDPP_SHORT) &&
		(report->rap.sub_id == 0x41);
}

static int hidpp_raw_hidpp_event(struct hidpp_device *hidpp_dev, u8 *data,
		int size)
{
	struct hidpp_report *question = hidpp_dev->send_receive_buf;
	struct hidpp_report *answer = hidpp_dev->send_receive_buf;
	struct hidpp_report *report = (struct hidpp_report *)data;

	/*
	 * If the mutex is locked then we have a pending answer from a
	 * previoulsly sent command
	 */
	if (unlikely(mutex_is_locked(&hidpp_dev->send_mutex))) {
		/*
		 * Check for a correct hidpp20 answer or the corresponding
		 * error
		 */
		if (hidpp_match_answer(question, report) ||
				hidpp_match_error(question, report)) {
			*answer = *report;
			hidpp_dev->answer_available = true;
			wake_up(&hidpp_dev->wait);
			/*
			 * This was an answer to a command that this driver sent
			 * We return 1 to hid-core to avoid forwarding the
			 * command upstream as it has been treated by the driver
			 */

			return 1;
		}
	}

	if (hidpp_dev->device_connect && hidpp_report_is_connect_event(report))
		schedule_delayed_hidpp_connect(hidpp_dev,
			!(report->rap.params[0] & (1 << 6)));

	return 0;
}

int hidpp_raw_event(struct hidpp_device *hidpp_dev, u8 *data, int size)
{
	struct hid_device *hdev = hidpp_dev->hid_dev;

	switch (data[0]) {
	case REPORT_ID_HIDPP_LONG:
		if (size != HIDPP_REPORT_LONG_LENGTH) {
			hid_err(hdev, "received hid++ report of bad size (%d)",
				size);
			return 1;
		}
		return hidpp_raw_hidpp_event(hidpp_dev, data, size);
	case REPORT_ID_HIDPP_SHORT:
		if (size != HIDPP_REPORT_SHORT_LENGTH) {
			hid_err(hdev, "received hid++ report of bad size (%d)",
				size);
			return 1;
		}
		return hidpp_raw_hidpp_event(hidpp_dev, data, size);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(hidpp_raw_event);

/* -------------------------------------------------------------------------- */
/* HIDP++ 1.0 commands                                                        */
/* -------------------------------------------------------------------------- */

#define HIDPP_SET_REGISTER				0x80
#define HIDPP_GET_REGISTER				0x81
#define HIDPP_SET_LONG_REGISTER				0x82
#define HIDPP_GET_LONG_REGISTER				0x83

#define HIDPP_REG_ENABLE_HIDPP_NOTIFICATIONS		0x00
#define ENABLE_HIDPP_WIRELESS_BIT			0
#define ENABLE_HIDPP_SOFTWARE_BIT			3

int hidpp_enable_notifications(struct hidpp_device *hidpp_dev,
	bool wireless_notifs, bool software_present)
{
	struct hidpp_report response;
	u8 params[3] = { 0x00,
			 ((!!wireless_notifs) << ENABLE_HIDPP_WIRELESS_BIT)
			 | ((!!software_present) << ENABLE_HIDPP_SOFTWARE_BIT),
			 0x00 };

	return hidpp_send_rap_command_sync(hidpp_dev, REPORT_ID_HIDPP_SHORT,
					HIDPP_SET_REGISTER,
					HIDPP_REG_ENABLE_HIDPP_NOTIFICATIONS,
					params, 3, &response);
}
EXPORT_SYMBOL_GPL(hidpp_enable_notifications);

#define HIDPP_REG_PAIRING_INFORMATION			0xB5
#define DEVICE_NAME					0x40

char *hidpp_get_unifying_name(struct hidpp_device *hidpp_dev, int device_index)
{
	struct hidpp_report response;
	int ret;
	u8 params[1] = { DEVICE_NAME | (device_index - 1) };
	char *name;
	int len;

	ret = hidpp_send_rap_command_sync(hidpp_dev,
					REPORT_ID_HIDPP_SHORT,
					HIDPP_GET_LONG_REGISTER,
					HIDPP_REG_PAIRING_INFORMATION,
					params, 1, &response);
	if (ret)
		return NULL;

	len = response.rap.params[1];

	name = kzalloc(len + 1, GFP_KERNEL);
	if (!name)
		return NULL;

	memcpy(name, &response.rap.params[2], len);
	return name;
}
EXPORT_SYMBOL_GPL(hidpp_get_unifying_name);

/* -------------------------------------------------------------------------- */
/* 0x0000: Root                                                               */
/* -------------------------------------------------------------------------- */

#define CMD_ROOT_GET_FEATURE				0x01
#define CMD_ROOT_GET_PROTOCOL_VERSION			0x11

int hidpp_root_get_feature(struct hidpp_device *hidpp_dev, u16 feature,
	u8 *feature_index, u8 *feature_type)
{
	struct hidpp_report response;
	int ret;
	u8 params[2] = { feature >> 8, feature & 0x00FF };

	ret = hidpp_send_fap_command_sync(hidpp_dev,
			HIDPP_PAGE_ROOT_IDX,
			CMD_ROOT_GET_FEATURE,
			params, 2, &response);
	if (ret)
		return ret;

	*feature_index = response.fap.params[0];
	*feature_type = response.fap.params[1];

	return ret;
}
EXPORT_SYMBOL_GPL(hidpp_root_get_feature);

int hidpp_root_get_protocol_version(struct hidpp_device *hidpp_dev,
	u8 *protocol_major, u8 *protocol_minor)
{
	struct hidpp_report response;
	int ret;

	ret = hidpp_send_fap_command_sync(hidpp_dev,
			HIDPP_PAGE_ROOT_IDX,
			CMD_ROOT_GET_PROTOCOL_VERSION,
			NULL, 0, &response);

	if (ret == 1) {
		*protocol_major = 1;
		*protocol_minor = 0;
		return 0;
	}

	if (ret)
		return -ret;

	*protocol_major = response.fap.params[0];
	*protocol_minor = response.fap.params[1];

	return ret;
}
EXPORT_SYMBOL_GPL(hidpp_root_get_protocol_version);

/* -------------------------------------------------------------------------- */
/* 0x0005: GetDeviceNameType                                                  */
/* -------------------------------------------------------------------------- */

#define CMD_GET_DEVICE_NAME_TYPE_GET_COUNT		0x01
#define CMD_GET_DEVICE_NAME_TYPE_GET_DEVICE_NAME	0x11
#define CMD_GET_DEVICE_NAME_TYPE_GET_TYPE		0x21

static int hidpp_devicenametype_get_count(struct hidpp_device *hidpp_dev,
	u8 feature_index, u8 *nameLength)
{
	struct hidpp_report response;
	int ret;
	ret = hidpp_send_fap_command_sync(hidpp_dev, feature_index,
		CMD_GET_DEVICE_NAME_TYPE_GET_COUNT, NULL, 0, &response);

	if (ret)
		return -ret;

	*nameLength = response.fap.params[0];

	return ret;
}

static int hidpp_devicenametype_get_device_name(struct hidpp_device *hidpp_dev,
	u8 feature_index, u8 char_index, char *device_name, int len_buf)
{
	struct hidpp_report response;
	int ret, i;
	int count;
	ret = hidpp_send_fap_command_sync(hidpp_dev, feature_index,
		CMD_GET_DEVICE_NAME_TYPE_GET_DEVICE_NAME, &char_index, 1,
		&response);

	if (ret)
		return -ret;

	if (response.report_id == REPORT_ID_HIDPP_LONG)
		count = HIDPP_REPORT_LONG_LENGTH - 4;
	else
		count = HIDPP_REPORT_SHORT_LENGTH - 4;

	if (len_buf < count)
		count = len_buf;

	for (i = 0; i < count; i++)
		device_name[i] = response.fap.params[i];

	return count;
}

char *hidpp_get_device_name(struct hidpp_device *hidpp_dev, u8 *name_length)
{
	u8 feature_type;
	u8 feature_index;
	u8 __name_length;
	char *name;
	unsigned index = 0;
	int ret;

	ret = hidpp_root_get_feature(hidpp_dev, HIDPP_PAGE_GET_DEVICE_NAME_TYPE,
		&feature_index, &feature_type);
	if (ret)
		goto out_err;

	ret = hidpp_devicenametype_get_count(hidpp_dev, feature_index,
		&__name_length);
	if (ret)
		goto out_err;

	name = kzalloc(__name_length + 1, GFP_KERNEL);
	if (!name)
		goto out_err;

	*name_length = __name_length + 1;
	while (index < __name_length)
		index += hidpp_devicenametype_get_device_name(hidpp_dev,
			feature_index, index, name + index,
			__name_length - index);

	return name;

out_err:
	*name_length = 0;
	return NULL;
}
EXPORT_SYMBOL_GPL(hidpp_get_device_name);

/* -------------------------------------------------------------------------- */
/* 0x6100: TouchPadRawXY                                                      */
/* -------------------------------------------------------------------------- */

#define CMD_TOUCHPAD_GET_RAW_INFO			0x01
#define CMD_TOUCHPAD_GET_RAW_REPORT_STATE		0x11
#define CMD_TOUCHPAD_SET_RAW_REPORT_STATE		0x21

int hidpp_touchpad_get_raw_info(struct hidpp_device *hidpp_dev,
	u8 feature_index, struct hidpp_touchpad_raw_info *raw_info)
{
	struct hidpp_report response;
	int ret;
	u8 *params = (u8 *)response.fap.params;

	ret = hidpp_send_fap_command_sync(hidpp_dev, feature_index,
		CMD_TOUCHPAD_GET_RAW_INFO, NULL, 0, &response);

	if (ret)
		return -ret;

	raw_info->x_size = (params[0] << 8) | params[1];
	raw_info->y_size = (params[2] << 8) | params[3];
	raw_info->z_range = params[4];
	raw_info->area_range = params[5];
	raw_info->maxcontacts = params[7];
	raw_info->origin = params[8];
	raw_info->res = (params[13] << 8) | params[14];

	return ret;
}
EXPORT_SYMBOL_GPL(hidpp_touchpad_get_raw_info);

int hidpp_touchpad_set_raw_report_state(struct hidpp_device *hidpp_dev,
		u8 feature_index, bool send_raw_reports,
		bool sensor_enhanced_settings)
{
	struct hidpp_report response;
	int ret;

	/*
	 * Params:
	 *   bit 0 - enable raw
	 *   bit 1 - 16bit Z, no area
	 *   bit 2 - enhanced sensitivity
	 *   bit 3 - width, height (4 bits each) instead of area
	 *   bit 4 - send raw + gestures (degrades smoothness)
	 *   remaining bits - reserved
	 */
	u8 params = send_raw_reports | (sensor_enhanced_settings << 2);

	ret = hidpp_send_fap_command_sync(hidpp_dev, feature_index,
		CMD_TOUCHPAD_SET_RAW_REPORT_STATE, &params, 1, &response);

	if (ret)
		return -ret;

	return ret;
}
EXPORT_SYMBOL_GPL(hidpp_touchpad_set_raw_report_state);

static void hidpp_touchpad_touch_event(u8 *data,
	struct hidpp_touchpad_raw_xy_finger *finger)
{
	u8 x_m = data[0] << 2;
	u8 y_m = data[2] << 2;

	finger->x = x_m << 6 | data[1];
	finger->y = y_m << 6 | data[3];

	finger->contact_type = data[0] >> 6;
	finger->contact_status = data[2] >> 6;

	finger->z = data[4];
	finger->area = data[5];
	finger->finger_id = data[6] >> 4;
}

void hidpp_touchpad_raw_xy_event(struct hidpp_device *hidpp_dev,
		u8 *data, struct hidpp_touchpad_raw_xy *raw_xy)
{
	raw_xy->end_of_frame = data[8] & 0x01;
	raw_xy->spurious_flag = (data[8] >> 1) & 0x01;
	raw_xy->finger_count = data[15] & 0x0f;
	raw_xy->button = (data[8] >> 2) & 0x01;

	if (raw_xy->finger_count) {
		hidpp_touchpad_touch_event(&data[2], &raw_xy->fingers[0]);
		hidpp_touchpad_touch_event(&data[9], &raw_xy->fingers[1]);
	}
}
EXPORT_SYMBOL_GPL(hidpp_touchpad_raw_xy_event);
