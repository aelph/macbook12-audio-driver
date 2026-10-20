/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * CS4208 headset block state for the 12-inch MacBook (A1534, MacBook9,1 / MacBook10,1).
 *
 * The combo jack's microphone contact reaches pin 0x18, but the pin's own
 * presence detection never fires and the contact is grounded by default, so a
 * headset microphone is neither seen nor heard.  The codec has a headset block
 * behind the vendor widget 0x24 that handles this; how it is driven was worked
 * out for the MacBookPro11,x (same codec) by Igor Chicherin in
 * https://github.com/IgorChicherin/mbp11-3-sound (AppleHDA class
 * AppleHDAMikeyInternalCS4208) and verified on the A1534 with hda-verb.
 *
 * Included before struct cs_spec; the functions live in patch_cirrus_a1534_pcm.h.
 */

#ifndef PATCH_CIRRUS_A1534_HEADSET_H
#define PATCH_CIRRUS_A1534_HEADSET_H

#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/input.h>

static bool headset_mic = true;
module_param(headset_mic, bool, 0444);
MODULE_PARM_DESC(headset_mic,
		 "CS4208 A1534: headset microphone in the combo jack (default=1)");

static bool headset_buttons = true;
module_param(headset_buttons, bool, 0444);
MODULE_PARM_DESC(headset_buttons,
		 "CS4208 A1534: headset remote buttons as media keys (default=1)");

static bool headset_redetect = true;
module_param(headset_redetect, bool, 0644);
MODULE_PARM_DESC(headset_redetect,
		 "CS4208 A1534: detect the headset again after every headphone trace replay instead of restoring its state (default=1)");

struct cs4208_hs {
	struct hda_codec *codec;
	struct mutex lock;		/* protects the state below */
	struct delayed_work work;	/* delayed detection start / timeout */
	int (*exec_verb)(struct hdac_device *dev, unsigned int cmd,
			 unsigned int flags, unsigned int *res);
	unsigned int gpio_fets;		/* FET control lines driven high */
	int btn_state;			/* SND_JACK_BTN_* currently pressed */
	int type;			/* detector's verdict, -1 = unknown */
	int retries;
	unsigned int enabled:1;
	unsigned int plugged:1;		/* something is in the combo jack */
	unsigned int detecting:1;	/* type detection is running */
	unsigned int mic:1;		/* reported as presence of pin 0x18 */
	unsigned int powered:1;		/* codec kept powered for the headset */
	unsigned int buttons:1;		/* button reception is set up */
};

#endif /* PATCH_CIRRUS_A1534_HEADSET_H */
