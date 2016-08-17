/*
 *   Lowlevel hardware access for the
 *   Razer Naga mouse
 *
 *   Important notice:
 *   This hardware driver is based on reverse engineering, only.
 *
 *   Copyright (C) 2007-2010 Michael Buesch <m@bues.ch>
 *   Copyright (C) 2010 Bernd Michael Helm <naga@rw23.de>
 *
 *   Naga-2012 fixes by Tibor Peluch <messani@gmail.com>
 *
 *   This program is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU General Public License
 *   as published by the Free Software Foundation; either version 2
 *   of the License, or (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 */

#include "hw_naga.h"
#include "razer_private.h"

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>


enum {
	NAGA_LED_SCROLL = 0,
	NAGA_LED_LOGO,
	NAGA_LED_THUMB_GRID,
	NAGA_NR_LEDS,
};

enum { /* Misc constants */
	/* Naga Classic/Epic/2012/Hex: from 100 to 5600 DPI. */
	NAGA_5600_NR_DPIMAPPINGS	= 56,
	/* Naga 2014: from 100 to 8200 DPI. */
	NAGA_8200_NR_DPIMAPPINGS	= 82,
	NAGA_NR_DPIMAPPINGS		= NAGA_8200_NR_DPIMAPPINGS,
	NAGA_NR_AXES			= 3,
};

struct naga_command {
	uint8_t status;
	uint8_t padding0[3];
	be16_t command;
	be16_t request;
	uint8_t values[5];
	uint8_t padding1[75];
	uint8_t checksum;
	uint8_t padding2;
} _packed;

struct naga_private {
	struct razer_mouse *m;

	/* Firmware version number. */
	uint16_t fw_version;
	/* The currently set LED states.
	 * Note: unsupported LEDs for a particular Naga model
	 * will be set to RAZER_LED_UNKNOWN */
	enum razer_led_state led_states[NAGA_NR_LEDS];
	/* The currently set frequency. */
	enum razer_mouse_freq frequency;
	/* The currently set resolution. */
	struct razer_mouse_dpimapping *cur_dpimapping_X;
	struct razer_mouse_dpimapping *cur_dpimapping_Y;

	struct razer_mouse_profile profile;
	struct razer_mouse_dpimapping dpimapping[NAGA_NR_DPIMAPPINGS];
	/* Number of mappings actually supported by this Naga model. */
	int nb_dpimappings;
	/* Model dependent method to initialize a resolution command. */
	void (*command_init_resolution)(struct naga_command *, struct naga_private *);
	struct razer_axis axes[NAGA_NR_AXES];

	bool commit_pending;
	struct razer_event_spacing packet_spacing;
};

#define NAGA_FW_MAJOR(ver)		(((ver) >> 8) & 0xFF)
#define NAGA_FW_MINOR(ver)		((ver) & 0xFF)
#define NAGA_FW(major, minor)		(((major) << 8) | (minor))

static const struct
{
	/* LED name. */
	const char *name;
	/* LED id when sending config command request. */
	uint8_t values[2];

} naga_leds[NAGA_NR_LEDS] = {
	{ "Scrollwheel", {0x01, 0x01} },
	{ "GlowingLogo", {0x01, 0x04} },
	{ "ThumbGrid",   {0x01, 0x05} },
};

static void naga_command_init(struct naga_command *cmd)
{
	memset(cmd, 0, sizeof(*cmd));
}

static void naga_command_init_resolution_5600(struct naga_command *cmd,
					      struct naga_private *priv)
{
	unsigned int xres, yres;

	naga_command_init(cmd);
	cmd->command = cpu_to_be16(0x0003);
	cmd->request = cpu_to_be16(0x0401);
	xres = (((unsigned int)priv->cur_dpimapping_X->res[RAZER_DIM_0] / 100) - 1) * 4;
	yres = (((unsigned int)priv->cur_dpimapping_Y->res[RAZER_DIM_0] / 100) - 1) * 4;
	cmd->values[0] = xres;
	cmd->values[1] = yres;
}

static void naga_command_init_resolution_8200(struct naga_command *cmd,
					      struct naga_private *priv)
{
	be16_t xres, yres;

	naga_command_init(cmd);
	cmd->command = cpu_to_be16(0x0007);
	cmd->request = cpu_to_be16(0x0405);
	xres = cpu_to_be16(priv->cur_dpimapping_X->res[RAZER_DIM_0]);
	yres = cpu_to_be16(priv->cur_dpimapping_Y->res[RAZER_DIM_0]);
	memcpy(cmd->values + 1, &xres, 2);
	memcpy(cmd->values + 3, &yres, 2);
}

static int naga_usb_write(struct naga_private *priv,
			  int request, int command,
			  void *buf, size_t size)
{
	int err;

	razer_event_spacing_enter(&priv->packet_spacing);
	err = libusb_control_transfer(
		priv->m->usb_ctx->h,
		LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_CLASS |
		LIBUSB_RECIPIENT_INTERFACE,
		request, command, 0,
		(unsigned char *)buf, size,
		RAZER_USB_TIMEOUT);
	razer_event_spacing_leave(&priv->packet_spacing);
	if (err < 0 || (size_t)err != size) {
		razer_error("razer-naga: "
			"USB write 0x%02X 0x%02X failed: %d\n",
			request, command, err);
		return err;
	}

	return 0;
}

static int naga_usb_read(struct naga_private *priv,
			 int request, int command,
			 void *buf, size_t size)
{
	int err, try;

	for (try = 0; try < 3; try++) {
		razer_event_spacing_enter(&priv->packet_spacing);
		err = libusb_control_transfer(
			priv->m->usb_ctx->h,
			LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_CLASS |
			LIBUSB_RECIPIENT_INTERFACE,
			request, command, 0,
			buf, size,
			RAZER_USB_TIMEOUT);
		razer_event_spacing_leave(&priv->packet_spacing);
		if (err >= 0 && (size_t)err == size)
			break;
	}
	if (err < 0 || (size_t)err != size) {
		razer_error("razer-naga: "
			"USB read 0x%02X 0x%02X failed: %d\n",
			request, command, err);
		return err;
	}

	return 0;
}

static int naga_send_command(struct naga_private *priv,
			     struct naga_command *cmd)
{
	int err;

	cmd->checksum = razer_xor8_checksum((uint8_t *)cmd + 2, sizeof(*cmd) - 4);
	err = naga_usb_write(priv, LIBUSB_REQUEST_SET_CONFIGURATION, 0x300,
			     cmd, sizeof(*cmd));
	if (err)
		return err;
	err = naga_usb_read(priv, LIBUSB_REQUEST_CLEAR_FEATURE, 0x300,
			    cmd, sizeof(*cmd));
	if (err)
		return err;
	if (cmd->status != 2 &&
	    cmd->status != 1 &&
	    cmd->status != 0) {
		razer_error("razer-naga: Command %04X/%04X failed with %02X\n",
			    be16_to_cpu(cmd->command),
			    be16_to_cpu(cmd->request),
			    cmd->status);
	}

	return 0;
}

static int naga_read_fw_ver(struct naga_private *priv)
{
	struct naga_command cmd;
	be16_t be16;
	uint16_t ver;
	int err;
	unsigned int i;

	/* Poke the device several times until it responds with a
	 * valid version number */
	for (i = 0; i < 5; i++) {
		naga_command_init(&cmd);
		cmd.command = cpu_to_be16(0x0002);
		cmd.request = cpu_to_be16(0x0081);
		err = naga_send_command(priv, &cmd);
		memcpy(&be16, &cmd.values, 2);
		ver = be16_to_cpu(be16);
		if (!err && (ver & 0xFF00) != 0)
			return ver;
		razer_msleep(250);
	}
	razer_error("razer-naga: Failed to read firmware version\n");

	return -ENODEV;
}

static int naga_do_commit(struct naga_private *priv)
{
	struct naga_command cmd;
	uint8_t freq;
	int led_id;
	int err;

	/* Set the resolution. */
	priv->command_init_resolution(&cmd, priv);
	err = naga_send_command(priv, &cmd);
	if (err)
		return err;

	/* Set the LEDs. */
	for (led_id = 0; led_id < NAGA_NR_LEDS; ++led_id) {
		if (RAZER_LED_UNKNOWN == priv->led_states[led_id]) {
			/* Not a supported LED on this model. */
			continue;
		}

		naga_command_init(&cmd);
		cmd.command = cpu_to_be16(0x0003);
		cmd.request = cpu_to_be16(0x0300);
		memcpy(cmd.values, naga_leds[led_id].values, 2);
		if (priv->led_states[led_id])
			cmd.values[2] = 1;
		err = naga_send_command(priv, &cmd);
		if (err)
			return err;
	}

	/* Set scan frequency. */
	switch (priv->frequency) {
	case RAZER_MOUSE_FREQ_125HZ:
		freq = 8;
		break;
	case RAZER_MOUSE_FREQ_500HZ:
		freq = 2;
		break;
	case RAZER_MOUSE_FREQ_1000HZ:
	case RAZER_MOUSE_FREQ_UNKNOWN:
		freq = 1;
		break;
	default:
		return -EINVAL;
	}
	naga_command_init(&cmd);
	cmd.command = cpu_to_be16(0x0001);
	cmd.request = cpu_to_be16(0x0005);
	cmd.values[0] = freq;
	err = naga_send_command(priv, &cmd);
	if (err)
		return err;

	return 0;
}

static int naga_get_fw_version(struct razer_mouse *m)
{
	struct naga_private *priv = m->drv_data;

	return priv->fw_version;
}

static int naga_commit(struct razer_mouse *m, int force)
{
	struct naga_private *priv = m->drv_data;
	int err = 0;

	if (!m->claim_count)
		return -EBUSY;
	if (priv->commit_pending || force) {
		err = naga_do_commit(priv);
		if (!err)
			priv->commit_pending = 0;
	}

	return err;
}

static int naga_led_toggle(struct razer_led *led,
			   enum razer_led_state new_state)
{
	struct razer_mouse *m = led->u.mouse;
	struct naga_private *priv = m->drv_data;

	if (led->id >= NAGA_NR_LEDS)
		return -EINVAL;
	if ((new_state != RAZER_LED_OFF) &&
	    (new_state != RAZER_LED_ON))
		return -EINVAL;
	if (priv->led_states[led->id] == RAZER_LED_UNKNOWN) {
		/* Not a supported LED on this model. */
		return -EINVAL;
	}

	if (!priv->m->claim_count)
		return -EBUSY;

	priv->led_states[led->id] = new_state;
	priv->commit_pending = 1;

	return 0;
}

static int naga_get_leds(struct razer_mouse *m,
			 struct razer_led **leds_list)
{
	struct naga_private *priv = m->drv_data;
	struct razer_led *led;
	int nb_leds;
	int led_id;

	nb_leds = 0;
	*leds_list = NULL;

	for (led_id = 0; led_id < NAGA_NR_LEDS; ++led_id) {
		if (RAZER_LED_UNKNOWN == priv->led_states[led_id]) {
			/* Not a supported LED on this model. */
			continue;
		}

		led = zalloc(sizeof(struct razer_led));
		if (!led)
			return -ENOMEM;

		led->name = naga_leds[led_id].name;
		led->id = led_id;
		led->state = priv->led_states[led_id];
		led->toggle_state = naga_led_toggle;
		led->u.mouse = m;

		led->next = *leds_list;
		*leds_list = led;
		++nb_leds;
	}

	return nb_leds;
}

static int naga_supported_freqs(struct razer_mouse *m,
				      enum razer_mouse_freq **freq_list)
{
	enum razer_mouse_freq *list;
	const int count = 3;

	list = zalloc(sizeof(*list) * count);
	if (!list)
		return -ENOMEM;

	list[0] = RAZER_MOUSE_FREQ_125HZ;
	list[1] = RAZER_MOUSE_FREQ_500HZ;
	list[2] = RAZER_MOUSE_FREQ_1000HZ;

	*freq_list = list;

	return count;
}

static enum razer_mouse_freq naga_get_freq(struct razer_mouse_profile *p)
{
	struct naga_private *priv = p->mouse->drv_data;

	return priv->frequency;
}

static int naga_set_freq(struct razer_mouse_profile *p,
			       enum razer_mouse_freq freq)
{
	struct naga_private *priv = p->mouse->drv_data;

	if (!priv->m->claim_count)
		return -EBUSY;

	priv->frequency = freq;
	priv->commit_pending = 1;

	return 0;
}

static int naga_supported_axes(struct razer_mouse *m,
			       struct razer_axis **axes_list)
{
	struct naga_private *priv = m->drv_data;

	*axes_list = priv->axes;

	return ARRAY_SIZE(priv->axes);
}

static int naga_supported_resolutions(struct razer_mouse *m,
					    enum razer_mouse_res **res_list)
{
	struct naga_private *priv = m->drv_data;
	enum razer_mouse_res *list;
	int i;

	list = zalloc(sizeof(*list) * priv->nb_dpimappings);
	if (!list)
		return -ENOMEM;
	for (i = 0; i < priv->nb_dpimappings; i++)
		list[i] = (enum razer_mouse_res)((i + 1) * 100);
	*res_list = list;

	return priv->nb_dpimappings;
}

static struct razer_mouse_profile * naga_get_profiles(struct razer_mouse *m)
{
	struct naga_private *priv = m->drv_data;

	return &priv->profile;
}

static int naga_supported_dpimappings(struct razer_mouse *m,
				      struct razer_mouse_dpimapping **res_ptr)
{
	struct naga_private *priv = m->drv_data;

	*res_ptr = &priv->dpimapping[0];

	return priv->nb_dpimappings;
}

static struct razer_mouse_dpimapping * naga_get_dpimapping(struct razer_mouse_profile *p,
							   struct razer_axis *axis)
{
	struct naga_private *priv = p->mouse->drv_data;

	if (!axis)
		axis = &priv->axes[0];
	if (axis->id == 0)
		return priv->cur_dpimapping_X;
	if (axis->id == 1)
		return priv->cur_dpimapping_Y;

	return NULL;
}

static int naga_set_dpimapping(struct razer_mouse_profile *p,
			       struct razer_axis *axis,
			       struct razer_mouse_dpimapping *d)
{
	struct naga_private *priv = p->mouse->drv_data;

	if (!priv->m->claim_count)
		return -EBUSY;
	if (axis && axis->id >= ARRAY_SIZE(priv->axes))
		return -EINVAL;

	if (axis) {
		if (axis->id == 0)
			priv->cur_dpimapping_X = d;
		else if (axis->id == 1)
			priv->cur_dpimapping_Y = d;
		else
			return -EINVAL;
	} else {
		priv->cur_dpimapping_X = d;
		priv->cur_dpimapping_Y = d;
	}
	priv->commit_pending = 1;

	return 0;
}

int razer_naga_init(struct razer_mouse *m,
		    struct libusb_device *usbdev)
{
	struct naga_private *priv;
	struct libusb_device_descriptor desc;
	int i, fwver, err;
	const char *model;

	BUILD_BUG_ON(sizeof(struct naga_command) != 90);

	err = libusb_get_device_descriptor(usbdev, &desc);
	if (err) {
		razer_error("hw_naga: Failed to get device descriptor\n");
		return -EIO;
	}

	priv = zalloc(sizeof(struct naga_private));
	if (!priv)
		return -ENOMEM;
	priv->m = m;
	m->drv_data = priv;

	/* Need to wait some time between USB packets to
	 * not confuse the firmware of some devices. */
	razer_event_spacing_init(&priv->packet_spacing, 25);

	err = razer_usb_add_used_interface(m->usb_ctx, 0, 0);
	if (err)
		goto err_free;

	err = m->claim(m);
	if (err) {
		razer_error("hw_naga: Failed to claim device\n");
		goto err_free;
	}

	/* Fetch firmware version */
	fwver = naga_read_fw_ver(priv);
	if (fwver < 0) {
		err = fwver;
		goto err_release;
	}
	priv->fw_version = fwver;
	if (desc.idProduct == RAZER_NAGA_PID_EPIC) {
		if (priv->fw_version < NAGA_FW(0x01, 0x04)) {
			razer_error("hw_naga: The firmware version %d.%d of this Naga "
				"has known bugs. Please upgrade to version 1.04 or later.",
				NAGA_FW_MAJOR(priv->fw_version),
				NAGA_FW_MINOR(priv->fw_version));
			m->flags |= RAZER_MOUSEFLG_SUGGESTFWUP;
		}
	}

	priv->frequency = RAZER_MOUSE_FREQ_1000HZ;
	priv->led_states[NAGA_LED_SCROLL] = RAZER_LED_ON;
	/* FIXME: not supported for Epic? */
	priv->led_states[NAGA_LED_LOGO] = RAZER_LED_ON;
	if (desc.idProduct == RAZER_NAGA_PID_2014)
		priv->led_states[NAGA_LED_THUMB_GRID] = RAZER_LED_ON;
	else
		priv->led_states[NAGA_LED_THUMB_GRID] = RAZER_LED_UNKNOWN;

	priv->profile.nr = 0;
	priv->profile.get_freq = naga_get_freq;
	priv->profile.set_freq = naga_set_freq;
	priv->profile.get_dpimapping = naga_get_dpimapping;
	priv->profile.set_dpimapping = naga_set_dpimapping;
	priv->profile.mouse = m;

	if (desc.idProduct == RAZER_NAGA_PID_2014) {
		priv->nb_dpimappings = NAGA_8200_NR_DPIMAPPINGS;
		priv->command_init_resolution = naga_command_init_resolution_8200;
	} else {
		priv->nb_dpimappings = NAGA_5600_NR_DPIMAPPINGS;
		priv->command_init_resolution = naga_command_init_resolution_5600;
	}

	for (i = 0; i < priv->nb_dpimappings; i++) {
		priv->dpimapping[i].nr = (unsigned int)i;
		priv->dpimapping[i].res[RAZER_DIM_0] = (enum razer_mouse_res)((i + 1) * 100);
		if (priv->dpimapping[i].res[RAZER_DIM_0] == 1000) {
			priv->cur_dpimapping_X = &priv->dpimapping[i];
			priv->cur_dpimapping_Y = &priv->dpimapping[i];
		}
		priv->dpimapping[i].dimension_mask = (1 << RAZER_DIM_0);
		priv->dpimapping[i].change = NULL;
		priv->dpimapping[i].mouse = m;
	}
	razer_init_axes(&priv->axes[0],
			"X", RAZER_AXIS_INDEPENDENT_DPIMAPPING,
			"Y", RAZER_AXIS_INDEPENDENT_DPIMAPPING,
			"Scroll", 0);

	m->type = RAZER_MOUSETYPE_NAGA;
	switch (desc.idProduct) {
	default:
	case RAZER_NAGA_PID_CLASSIC:
	    model = "Naga";
	    break;
	case RAZER_NAGA_PID_EPIC:
	    model = "Naga Epic";
	    break;
	case RAZER_NAGA_PID_2012:
	    model = "Naga 2012";
	    break;
	case RAZER_NAGA_PID_HEX:
	    model = "Naga Hex";
	    break;
	case RAZER_NAGA_PID_HEX_V2:
	    model = "Naga Hex v2";
	    break;
	case RAZER_NAGA_PID_2014:
	    model = "Naga 2014";
	    break;
	}
	razer_generic_usb_gen_idstr(usbdev, m->usb_ctx->h, model, 1,
				    NULL, m->idstr);

	m->get_fw_version = naga_get_fw_version;
	m->commit = naga_commit;
	m->global_get_leds = naga_get_leds;
	m->get_profiles = naga_get_profiles;
	m->supported_axes = naga_supported_axes;
	m->supported_resolutions = naga_supported_resolutions;
	m->supported_freqs = naga_supported_freqs;
	m->supported_dpimappings = naga_supported_dpimappings;

	err = naga_do_commit(priv);
	if (err) {
		razer_error("hw_naga: Failed to commit initial settings\n");
		goto err_release;
	}

	m->release(m);

	return 0;

err_release:
	m->release(m);
err_free:
	free(priv);
	return err;
}

void razer_naga_release(struct razer_mouse *m)
{
	struct naga_private *priv = m->drv_data;

	free(priv);
}
