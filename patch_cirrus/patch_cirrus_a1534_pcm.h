/* ---- CS4208 live headphone<->speaker auto-switch state ----
 * The captured macOS sequences (play_a1534 / headphones_a1534) only get run at
 * PCM-open time. To switch a *running* stream when the jack is plugged/unplugged
 * we cache the live stream tag+format here and re-point the DMA stream onto the
 * correct converter from the unsolicited jack handler below. */
static unsigned int cs4208_cur_stream_tag;	/* running PCM stream tag (0 = none) */
static unsigned int cs4208_cur_format;		/* running PCM HDA format word */
static int cs4208_cur_hp = -1;			/* last HP jack state (-1 = unknown) */
/* cs_4208_hp_jack_callback is defined further down, after get_hda_cvt_setup_4208,
 * which it needs to invalidate the converter format cache. */
/* headset detection (A1534 combo jack), defined further down as well */
static void cs4208_hs_jack_update(struct hda_codec *codec, bool plugged,
				  bool replay);

static int cs_4208_playback_pcm_prepare(struct hda_pcm_stream *hinfo,
					struct hda_codec *codec,
					unsigned int stream_tag,
					unsigned int format,
					struct snd_pcm_substream *substream)
{
	struct cs_spec *spec = codec->spec;
	codec_dbg(codec, "cs_4208_playback_pcm_prepare start");
	cs4208_cur_stream_tag = stream_tag;	/* cache for live jack re-pointing */
	cs4208_cur_format = format;
	snd_hda_codec_setup_stream(codec, hinfo->nid, stream_tag, 0, format);

	if (spec->gen.pcm_playback_hook)
		spec->gen.pcm_playback_hook(hinfo, codec, substream, HDA_GEN_PCM_ACT_PREPARE);

	codec_dbg(codec, "cs_4208_playback_pcm_prepare end");

	return 0;
}

static int cs_4208_playback_pcm_cleanup(struct hda_pcm_stream *hinfo,
					struct hda_codec *codec,
					struct snd_pcm_substream *substream)
{
	//struct cs_spec *spec = codec->spec;
	codec_dbg(codec, "cs_4208_playback_pcm_cleanup start");
	cs4208_cur_stream_tag = 0;	/* no running stream to re-point on jack change */
	snd_hda_codec_cleanup_stream(codec, hinfo->nid);
	codec_dbg(codec, "cs_4208_playback_pcm_cleanup end");

	return 0;
}

// this is very hacky but until get more understanding of what we can do with the 4208 setup
// re-define these from hda_codec.c here
// NOTA BENE - need to check this is consistent with any hda_codec.c updates!!

/*
 * audio-converter setup caches
 */
struct hda_cvt_setup {
	hda_nid_t nid;
	u8 stream_tag;
	u8 channel_id;
	u16 format_id;
	unsigned char active;   /* cvt is currently used */
	unsigned char dirty;    /* setups should be cleared */
};
/* get or create a cache entry for the given audio converter NID */
static struct hda_cvt_setup *
get_hda_cvt_setup_4208(struct hda_codec *codec, hda_nid_t nid)
{
	struct hda_cvt_setup *p;
	int i;

	for (i = 0; i < codec->cvt_setups.used; i++) {
		p = snd_array_elem(&codec->cvt_setups, i);
		if (p->nid == nid)
			return p;
	}
	p = snd_array_new(&codec->cvt_setups);
	if (p)
		p->nid = nid;
	return p;
}

static int cs_4208_playback_pcm_open(struct hda_pcm_stream *hinfo,
				     struct hda_codec *codec,
				     struct snd_pcm_substream *substream)
{
	int err;
	int hp_pin_sense;
	unsigned int hp_pin;
	codec_dbg(codec, "cs_4208_playback_pcm_open start");
	codec_dbg(codec, "playback_pcm_open nid 0x%02x rates 0x%08x formats 0x%016llx\n",hinfo->nid,hinfo->rates,hinfo->formats);
		hp_pin = 0x10; // HP pin is Node 0x10
		hp_pin_sense = snd_hda_jack_detect(codec, hp_pin);
		cs4208_cur_hp = hp_pin_sense; // keep jack-callback in sync with open-path routing
		codec_dbg(codec, "cs_4208_playback_pcm_open hp_pin_sense: %d", hp_pin_sense);
		if (hp_pin_sense) { // output to headphones
			codec_dbg(codec, "cs_4208_playback_pcm_open playing to headphones");
			hinfo->nid = 0x02; // headphone converter (DAC) -> HP pin 0x10
			err = headphones_a1534(codec);
			cs4208_hs_jack_update(codec, true, true);
		} else { // output to speakers
			codec_dbg(codec, "cs_4208_playback_pcm_open playing to speakers");
			hinfo->nid = 0x04; // speaker converter (DAC) -> speaker pin 0x1d
			err = play_a1534(codec);
		}
	
	//call_pcm_playback_hook(hinfo, codec, substream, HDA_GEN_PCM_ACT_OPEN);

	//mutex_lock(&gen_spec->pcm_mutex);
	//err = snd_hda_multi_out_analog_open(codec,
						//&spec->multiout, substream,
						//hinfo);
	//if (!err) {
		//spec->active_streams |= 1 << STREAM_MULTI_OUT;
		//call_pcm_playback_hook(hinfo, codec, substream,
					//HDA_GEN_PCM_ACT_OPEN);
	//}
	//mutex_unlock(&gen_spec->pcm_mutex);
	codec_dbg(codec, "cs_4208_playback_pcm_open end");
	return 0;
}

static const struct hda_pcm_stream cs4208_pcm_analog_playback = {
	.substreams = 1,
	.channels_min = 2,
	.channels_max = 4,
	.rates = SNDRV_PCM_RATE_44100,
	.formats = SNDRV_PCM_FMTBIT_S16_LE, //|SNDRV_PCM_FMTBIT_S24_LE
	.maxbps = 16, // 32 before
	.ops = {
 		.open = cs_4208_playback_pcm_open,
		.prepare = cs_4208_playback_pcm_prepare,
		.cleanup = cs_4208_playback_pcm_cleanup,
	},
};

static void cs_4208_fill_pcm_stream_name(char *str, size_t len, const char *sfx,
					 const char *chip_name)
{
	char *p;

	if (*str)
		return;
	strscpy(str, chip_name, len);

	/* drop non-alnum chars after a space */
	for (p = strchr(str, ' '); p; p = strchr(p + 1, ' ')) {
		if (!isalnum(p[1])) {
			*p = 0;
			break;
		}
	}
	strscpy(str, sfx, len);
}

void cs_4208_playback_pcm_hook(struct hda_pcm_stream *hinfo, struct hda_codec *codec, struct snd_pcm_substream *substream, int action)
{

	struct cs_spec *spec = codec->spec;

	// so finally getting a handle on ordering here
	// we need to do the OSX setup in the OPEN section
	// as the generic hda format and stream setup is done BEFORE the PREPARE hook
	// (theres a good chance we only need to do this once at least as long as machine doesnt sleep)
	// (or we could just override the prepare function completely)
	// I now think the noise was caused by mis-match between the stream format and the nid setup format
	// (because the generic setup was done before the OSX setup and the actual streamed format is slightly different)
	// (the hda documentation says these really need to match)
	// It appears the 4208 setup can handle at least some differences in the stream format
	// certainly seems to handle S24_LE or S32_LE differences

	
	if (action == HDA_GEN_PCM_ACT_OPEN) {
		struct hda_cvt_setup *p = NULL;
        	int hp_pin_sense;
        	unsigned int hp_pin;
        	int err;
		codec_dbg(codec, "cs_4208_playback_pcm_hook HDA_GEN_PCM_ACT_OPEN start");

		hp_pin = 0x10; // HP pin is Node 0x10
		hp_pin_sense = snd_hda_jack_detect(codec, hp_pin);
		codec_dbg(codec, "cs_4208_playback_pcm_hook hp_pin_sense: %d", hp_pin_sense);
		//hp_pin_sense = 0;
		//if (hp_pin_sense == 1) { // output to headphones
		//	codec_dbg(codec, "cs_4208_playback_pcm_hook playing to headphones");
		//}
		if (hp_pin_sense == 0) { // output to speakers
			//codec_dbg(codec, "cs_4208_playback_pcm_hook playing to speakers");
        		err = play_a1534(codec);
		}

		//if (!spec->play_init) {
		if (1) {
			//int power_chk = 0;
			struct timespec64 curtim;
			ktime_get_real_ts64(&curtim);
			spec->first_play_time.tv_sec = curtim.tv_sec;
			//cs_4208_play_setup(codec);
			codec_dbg(codec, "cs_4208_playback_pcm_hook BAD setup play called");
			spec->play_init = 1;
			spec->playing = 0;
		}

		// we need to force the stream to be re-set here
		// problem is it appears hda_codec caches the stream format and id and only updates if changed
		// and there doesnt seem to be a good way to force an update
		// problem - the get_hda_cvt_setup function is local to hda_codec

		// this routine doesnt seem to be nid specific - so explicitly fix the known nids here

		p = get_hda_cvt_setup_4208(codec, 0x02);
		codec_dbg(codec, "cs_4208_playback_pcm_hook BAD cvt pointer 0x02 %p", p);

		p->stream_tag = 0;
		p->channel_id = 0;
		p->format_id = 0;

		p = get_hda_cvt_setup_4208(codec, 0x02);

		codec_dbg(codec, "cs_4208_playback_pcm_hook BAD cvt pointer 0x03 %p", p);

		p->stream_tag = 0;
		p->channel_id = 0;
		p->format_id = 0;

		codec_dbg(codec, "cs_4208_playback_pcm_hook HDA_GEN_PCM_ACT_OPEN end");

	} else if (action == HDA_GEN_PCM_ACT_PREPARE) {
		struct timespec64 curtim;
		ktime_get_real_ts64(&curtim);
		codec_dbg(codec, "cs_4208_playback_pcm_hook HDA_GEN_PCM_ACT_PREPARE start");
		codec_dbg(codec, "cs_4208_playback_pcm_hook BAD HOOK PREPARE init %d last %lld cur %lld", spec->play_init,spec->last_play_time.tv_sec,curtim.tv_sec);
		//if (spec->play_init && curtim.tv_sec > (spec->first_play_time.tv_sec + 0))
		//if (spec->play_init) {
		if (1) {
			int power_chk = 0;
        		power_chk = snd_hda_codec_read(codec, codec->core.afg, 0, AC_VERB_GET_POWER_STATE, 0);
			codec_dbg(codec, "cs_4208_playback_pcm_hook BAD power check 0x01 2 %d", power_chk); 
			spec->last_play_time.tv_sec = curtim.tv_sec;
			spec->playing = 1;
		}

		codec_dbg(codec, "cs_4208_playback_pcm_hook HDA_GEN_PCM_ACT_PREPARE end");
	} else if (action == HDA_GEN_PCM_ACT_CLEANUP) {
		int power_chk = 0;
		codec_dbg(codec, "cs_4208_playback_pcm_hook HDA_GEN_PCM_ACT_CLEANUP start");
		power_chk = snd_hda_codec_read(codec, codec->core.afg, 0, AC_VERB_GET_POWER_STATE, 0);
		codec_dbg(codec, "cs_4208_playback_pcm_hook BAD power check 0x01 3 %d", power_chk);
		//if (spec->playing) {
			//cs_4208_play_cleanup(codec);
			codec_dbg(codec, "cs_4208_playback_pcm_hook BAD done play down");
			spec->playing = 0;
		//}
		//cs_4208_play_cleanup(codec);
		power_chk = snd_hda_codec_read(codec, codec->core.afg, 0, AC_VERB_GET_POWER_STATE, 0);
		codec_dbg(codec, "cs_4208_playback_pcm_hook BAD power check 0x01 4 %d", power_chk);
		codec_dbg(codec, "cs_4208_playback_pcm_hook HDA_GEN_PCM_ACT_CLEANUP end");
	}
	//} else if (action == HDA_GEN_PCM_ACT_CLOSE) {
		codec_dbg(codec, "cs_4208_playback_pcm_hook HDA_GEN_PCM_ACT_CLOSE start");
		codec_dbg(codec, "cs_4208_playback_pcm_hook HDA_GEN_PCM_ACT_CLOSE end");
	//}
	codec_dbg(codec, "cs_4208_playback_pcm_hook end");

}

static int cs_4208_build_controls_explicit(struct hda_codec *codec)
{
	int retval;
	codec_dbg(codec, "cs_4208_build_controls_explicit start");

	// Without this, no capture / mic mixer controls get created, ALSA never
	// selects an ADC for input, so the capture stream stays silent. We let
	// the generic builder create the standard set; speaker output still goes
	// through our custom playback PCM regardless.
	retval = snd_hda_gen_build_controls(codec);
	if (retval < 0)
		return retval;

	codec_dbg(codec, "cs_4208_build_controls_explicit end");
	return 0;
}

int cs_4208_build_pcms_explicit(struct hda_codec *codec)
{
	int retval;

	struct cs_spec *spec = codec->spec;
	struct hda_gen_spec *gen_spec = &(spec->gen);
	struct hda_pcm *info;

	codec_dbg(codec, "cs_4208_build_pcms_explicit start");

	// Let the generic builder set up everything (including the capture stream
	// for the internal/external mic). Without this we get no capture device
	// at all — speakers may work but mic is dead. We then override only the
	// analog playback stream below with the leifliddy custom config.
	retval = snd_hda_gen_build_pcms(codec);
	if (retval < 0)
		return retval;

	cs_4208_fill_pcm_stream_name(gen_spec->stream_name_analog,
					sizeof(gen_spec->stream_name_analog),
					" Analog", codec->core.chip_name);
	info = gen_spec->pcm_rec[0];
	if (!info)
		return -ENOMEM;

	info->stream[SNDRV_PCM_STREAM_PLAYBACK] = cs4208_pcm_analog_playback;
	info->stream[SNDRV_PCM_STREAM_PLAYBACK].nid = 0x04; // 0x04 is for speakers 0x02 is for headphones
	info->stream[SNDRV_PCM_STREAM_PLAYBACK].channels_max = 4;

	info->pcm_type = HDA_PCM_TYPE_AUDIO;

	codec_dbg(codec, "cs_4208_build_pcms_explicit end");
	return retval;
}

/*
void cs_4208_jack_unsol_event(struct hda_codec *codec, unsigned int res)
{
	struct hda_jack_tbl *event;
	int tag = (res >> AC_UNSOL_RES_TAG_SHIFT) & 0x7f;

	dev_info(hda_codec_dev(codec), "cs_4208_jack_unsol_event 0x%08x tag 0x%02x\n",res,tag);

	event = snd_hda_jack_tbl_get_from_tag(codec, tag, 0);
        if (!event)
		return;
	event->jack_dirty = 1;

	//call_jack_callback(codec, event);
	snd_hda_jack_report_sync(codec);
}
*/

#define cs_4208_free            snd_hda_gen_free

/* headphones_a1534()/play_a1534() cycle the AFG through D3, which clears the
 * stream id of the capture converters while the core's hda_cvt_setup cache
 * still believes the tag is programmed. Re-arm the cache and, for a live
 * capture stream, reprogram the converter so the mic keeps recording. */
static void cs_4208_rearm_capture_cvt(struct hda_codec *codec, hda_nid_t nid)
{
	struct hda_cvt_setup *p = get_hda_cvt_setup_4208(codec, nid);
	unsigned int tag, fmt;

	if (!p)
		return;
	tag = p->stream_tag;
	fmt = p->format_id;
	p->stream_tag = 0; p->channel_id = 0; p->format_id = 0;
	if (p->active && tag) {
		codec_dbg(codec, "cs_4208_rearm_capture_cvt nid 0x%02x tag=%u fmt=0x%x", nid, tag, fmt);
		snd_hda_codec_setup_stream(codec, nid, tag, 0, fmt);
	}
}

/* ---- A1534 headset microphone (combo jack) ----
 *
 * The CS4208 has a headset block behind the vendor widget 0x24.  Two depletion
 * FETs, controlled by GPIO5 ("US") and GPIO4 ("CH"), ground the two possible
 * microphone contacts of the combo jack while their GPIO is low; the stock
 * state leaves both low, so a headset microphone is always shorted.  With both
 * GPIOs high, the pin's bias on and coefficient 0x19 = 0xc383 the block runs a
 * type detection and answers with an unsolicited response carrying the fixed
 * tag 0x2e: bit 2 = valid, bits 1:0 = 0 or 1 for the two headset pinouts, 2 or
 * 3 for plain headphones (3 = undecided, retry).  For a headset the FET of its
 * ground contact is closed again and the microphone is on pin 0x18.
 *
 * The sequence is the one AppleHDA (class AppleHDAMikeyInternalCS4208) uses and
 * was worked out for the MacBookPro11,x by Igor Chicherin
 * (https://github.com/IgorChicherin/mbp11-3-sound).  One difference on the
 * A1534: pin 0x18 must carry VREF80 while the detector runs, otherwise it
 * answers "headphones" for a headset.  The captured macOS trace replayed by
 * headphones_a1534() runs the same detection but, recorded with plain
 * headphones, ends with both FETs low, so the detection is run again after
 * every replay.
 *
 * The verdict is reported as the presence of pin 0x18 (its own sense never
 * fires) by answering GET_PIN_SENSE from cs4208_hs_exec_verb(), so the generic
 * "Mic Jack" control and auto-mic switching work as for any other jack.  The
 * headset block loses its state when the codec powers down, so the codec is
 * kept powered while a headset with a microphone is plugged in.
 */
#define CS4208_HP_PIN			0x10
#define CS4208_HS_MIC_PIN		0x18
#define CS4208_HS_UNSOL_TAG		0x2e	/* headset detector */
#define CS4208_HS_BTN_UNSOL_TAG		0x2f	/* headset buttons */
#define CS4208_HS_GPIO_US		BIT(5)
#define CS4208_HS_GPIO_CH		BIT(4)
#define CS4208_HS_GPIO_MASK		(CS4208_HS_GPIO_US | CS4208_HS_GPIO_CH)
#define CS4208_VERB_HS_IRQ_MASK		0x7f0
#define CS4208_VERB_HS_BTN_IRQ_MASK	0x7f1
#define CS4208_IDX_HP_CLAMP		0x0000	/* bit 13: headphone clamps off */
#define CS4208_IDX_HS_DETECT		0x0019
#define CS4208_IDX_HS_BIAS		0x001c
#define CS4208_HS_DETECT_DELAY		250	/* ms, "HeadsetRetryTimeout" */
#define CS4208_HS_DETECT_TIMEOUT	1000	/* ms */
#define CS4208_HS_MAX_RETRIES		4

/* the jack table is stale during a resume, so ask the codec itself */
static bool cs4208_hp_plugged(struct hda_codec *codec)
{
	return snd_hda_codec_read(codec, CS4208_HP_PIN, 0,
				  AC_VERB_GET_PIN_SENSE, 0) & AC_PINSENSE_PRESENCE;
}

static void cs4208_hs_coef_update(struct hda_codec *codec, unsigned int idx,
				  unsigned int mask, unsigned int val)
{
	unsigned int coef = cs_vendor_coef_get(codec, idx);

	cs_vendor_coef_set(codec, idx, (coef & ~mask) | val);
}

/* @open: the FET lines to drive high, i.e. the contacts to leave ungrounded.
 * The replayed traces write GPIO data behind spec->gpio_data's back, so start
 * from what the codec holds. */
static void cs4208_hs_set_fets(struct hda_codec *codec, unsigned int open)
{
	struct cs_spec *spec = codec->spec;
	unsigned int data;

	data = snd_hda_codec_read(codec, 0x01, 0, AC_VERB_GET_GPIO_DATA, 0);
	data = (data & ~CS4208_HS_GPIO_MASK) | open;
	spec->hs.gpio_fets = open;
	spec->gpio_data = (spec->gpio_data & ~CS4208_HS_GPIO_MASK) | open;
	snd_hda_codec_write(codec, 0x01, 0, AC_VERB_SET_GPIO_DATA, data);
}

/* tell the generic code whether pin 0x18 has a microphone now */
static void cs4208_hs_report(struct hda_codec *codec, bool mic)
{
	struct cs_spec *spec = codec->spec;
	struct hda_jack_tbl *jack;

	if (spec->hs.mic == mic)
		return;
	spec->hs.mic = mic;
	jack = snd_hda_jack_tbl_get(codec, CS4208_HS_MIC_PIN);
	if (jack)
		snd_hda_jack_unsol_event(codec,
					 jack->tag << AC_UNSOL_RES_TAG_SHIFT);
}

/* keep the codec powered while a headset microphone is in use */
static void cs4208_hs_keep_power(struct hda_codec *codec, bool on)
{
	struct cs_spec *spec = codec->spec;

	if (spec->hs.powered == on)
		return;
	spec->hs.powered = on;
	if (on)
		snd_hda_power_up_pm(codec);
	else
		snd_hda_power_down_pm(codec);
}

/* Apply the detector's verdict: 0 and 1 are the two headset pinouts, 2 and 3
 * mean no microphone.  Type 0 (CTIA, Apple EarPods) is verified on a
 * MacBook10,1; the line for type 1 follows AppleHDA. */
static void cs4208_hs_set_type(struct hda_codec *codec, int type)
{
	static const unsigned int open_fets[4] = {
		CS4208_HS_GPIO_US, CS4208_HS_GPIO_CH, 0, 0
	};
	struct cs_spec *spec = codec->spec;

	spec->hs.type = type;
	cs4208_hs_set_fets(codec, open_fets[type & 3]);
	/* headphone clamps on again */
	cs4208_hs_coef_update(codec, CS4208_IDX_HP_CLAMP, 0x2000, 0);
	if (type < 2)	/* bias from the pin's VREF, auto-clamp on */
		cs4208_hs_coef_update(codec, CS4208_IDX_HS_BIAS,
				      0x0172, 0x0142);
	else		/* bias off */
		cs4208_hs_coef_update(codec, CS4208_IDX_HS_BIAS,
				      0x0172, 0x0112);
	cs4208_hs_keep_power(codec, type < 2);
}

static void cs4208_hs_stop_detect(struct hda_codec *codec)
{
	struct cs_spec *spec = codec->spec;

	if (!spec->hs.detecting)
		return;
	spec->hs.detecting = 0;
	cs_vendor_coef_set(codec, CS4208_IDX_HS_DETECT, 0x8383);
	snd_hda_power_down_pm(codec);
}

static void cs4208_hs_start_detect(struct hda_codec *codec)
{
	struct cs_spec *spec = codec->spec;

	/* stay powered until the detector has answered */
	snd_hda_power_up_pm(codec);
	spec->hs.detecting = 1;
	cs4208_hs_set_fets(codec, CS4208_HS_GPIO_MASK);
	snd_hda_codec_write(codec, CS4208_HS_MIC_PIN, 0,
			    AC_VERB_SET_POWER_STATE, AC_PWRST_D0);
	/* the A1534 detector needs the pin's VREF80 to see a microphone */
	snd_hda_codec_write(codec, CS4208_HS_MIC_PIN, 0,
			    AC_VERB_SET_PIN_WIDGET_CONTROL,
			    PIN_IN | AC_PINCTL_VREF_80);
	cs4208_hs_coef_update(codec, CS4208_IDX_HS_BIAS, 0x0030, 0);
	cs_vendor_coef_set(codec, CS4208_IDX_HS_DETECT, 0xc383);
	schedule_delayed_work(&spec->hs.work,
			      msecs_to_jiffies(CS4208_HS_DETECT_TIMEOUT));
}

/* delayed start of the detection after a plug, and its timeout */
static void cs4208_hs_work(struct work_struct *work)
{
	struct cs4208_hs *hs = container_of(work, struct cs4208_hs, work.work);
	struct hda_codec *codec = hs->codec;
	struct cs_spec *spec = codec->spec;

	mutex_lock(&spec->hs.lock);
	if (spec->hs.detecting) {
		/* no answer: handle it as headphones */
		codec_dbg(codec, "headset detector: no answer\n");
		cs4208_hs_stop_detect(codec);
		cs4208_hs_set_type(codec, 2);
		cs4208_hs_report(codec, false);
	} else if (spec->hs.plugged && spec->hs.type < 0) {
		cs4208_hs_start_detect(codec);
	}
	mutex_unlock(&spec->hs.lock);
}

/* unsolicited response of the headset detector */
static void cs4208_hs_unsol_event(struct hda_codec *codec, unsigned int res)
{
	struct cs_spec *spec = codec->spec;
	int type = res & 3;

	mutex_lock(&spec->hs.lock);
	if (!spec->hs.detecting || !(res & 4))
		goto out;
	cancel_delayed_work(&spec->hs.work);
	cs4208_hs_stop_detect(codec);
	if (type == 3 && spec->hs.retries++ < CS4208_HS_MAX_RETRIES) {
		/* undecided: as AppleHDA, run the detection again */
		schedule_delayed_work(&spec->hs.work,
				      msecs_to_jiffies(CS4208_HS_DETECT_DELAY));
		goto out;
	}
	codec_dbg(codec, "headset detector: type %d (res 0x%08x)\n", type, res);
	cs4208_hs_set_type(codec, type);
	cs4208_hs_report(codec, type < 2);
out:
	mutex_unlock(&spec->hs.lock);
}

/* start over for a new plug state; called with hs.lock held */
static void cs4208_hs_set_plugged(struct hda_codec *codec, bool plugged)
{
	struct cs_spec *spec = codec->spec;

	spec->hs.plugged = plugged;
	spec->hs.type = -1;
	cancel_delayed_work(&spec->hs.work);
	cs4208_hs_stop_detect(codec);
	if (plugged) {
		snd_hda_codec_write(codec, CS4208_VENDOR_NID, 0,
				    CS4208_VERB_HS_BTN_IRQ_MASK, 0);
		snd_hda_codec_write(codec, CS4208_VENDOR_NID, 0,
				    CS4208_VERB_HS_IRQ_MASK, 0xae);
		/* headphone clamps off while the detector measures */
		cs4208_hs_coef_update(codec, CS4208_IDX_HP_CLAMP,
				      0x2000, 0x2000);
		spec->hs.retries = 0;
		schedule_delayed_work(&spec->hs.work,
				      msecs_to_jiffies(CS4208_HS_DETECT_DELAY));
	} else {
		cs4208_hs_set_fets(codec, 0);
		cs4208_hs_coef_update(codec, CS4208_IDX_HP_CLAMP, 0x2000, 0);
		cs_vendor_coef_set(codec, CS4208_IDX_HS_DETECT, 0);
		cs_vendor_coef_set(codec, CS4208_IDX_HS_BIAS, 0);
		cs4208_hs_report(codec, false);
		cs4208_hs_keep_power(codec, false);
	}
}

/* The combo jack was plugged or unplugged, or a trace replay reset the headset
 * block (@replay): a plugged jack is detected again either way. */
static void cs4208_hs_jack_update(struct hda_codec *codec, bool plugged,
				  bool replay)
{
	struct cs_spec *spec = codec->spec;

	if (!spec->hs.enabled)
		return;
	mutex_lock(&spec->hs.lock);
	if (plugged != spec->hs.plugged || (plugged && replay))
		cs4208_hs_set_plugged(codec, plugged);
	mutex_unlock(&spec->hs.lock);
}

/* codec init and resume */
static void cs4208_hs_init(struct hda_codec *codec)
{
	struct cs_spec *spec = codec->spec;
	bool plugged;

	if (!spec->hs.enabled)
		return;
	plugged = cs4208_hp_plugged(codec);
	mutex_lock(&spec->hs.lock);
	if (plugged && spec->hs.plugged && spec->hs.type >= 2) {
		/* The same headphones as before the power-down: nothing to
		 * detect, and a detection would lift their ground. */
		cs4208_hs_set_type(codec, spec->hs.type);
		goto out;
	}
	/* A headset keeps the codec powered, so this is the first init or a
	 * system resume: the headset block has to detect again. */
	if (!plugged)
		cs4208_hs_set_fets(codec, 0);
	cs4208_hs_set_plugged(codec, plugged);
out:
	mutex_unlock(&spec->hs.lock);
}

/* stop a running detection; it starts again from cs4208_hs_init() */
static void cs4208_hs_stop(struct hda_codec *codec, bool release)
{
	struct cs_spec *spec = codec->spec;

	if (!spec->hs.enabled)
		return;
	cancel_delayed_work_sync(&spec->hs.work);
	mutex_lock(&spec->hs.lock);
	if (spec->hs.detecting) {
		spec->hs.detecting = 0;
		snd_hda_power_down_pm(codec);
	}
	if (release)
		cs4208_hs_keep_power(codec, false);
	mutex_unlock(&spec->hs.lock);
}

/* pin 0x18 cannot sense its plug: answer with the detector's verdict */
static int cs4208_hs_exec_verb(struct hdac_device *dev, unsigned int cmd,
			       unsigned int flags, unsigned int *res)
{
	struct hda_codec *codec = container_of(dev, struct hda_codec, core);
	struct cs_spec *spec = codec->spec;
	unsigned int nid = (cmd >> 20) & 0x7f;
	unsigned int verb = (cmd >> 8) & 0xfff;

	if (spec->hs.enabled && nid == CS4208_HS_MIC_PIN &&
	    verb == AC_VERB_GET_PIN_SENSE) {
		if (res)
			*res = spec->hs.mic ? AC_PINSENSE_PRESENCE : 0;
		return 0;
	}
	return spec->hs.exec_verb(dev, cmd, flags, res);
}

/* The digital microphone (pin 0x19) feeds both ADCs regardless of the
 * selected input: with it enabled, a capture from the headset microphone on
 * ADC 0x07 carries the internal microphone instead.  Keep the pin off while
 * the headset microphone is the selected input. */
#define CS4208_DMIC_PIN		0x19

static void cs4208_hs_mic_autoswitch(struct hda_codec *codec,
				     struct hda_jack_callback *jack)
{
	struct cs_spec *spec = codec->spec;

	snd_hda_gen_mic_autoswitch(codec, jack);
	snd_hda_set_pin_ctl_cache(codec, CS4208_DMIC_PIN,
				  spec->hs.mic ? 0 : PIN_IN);
}

/* before the pin parser runs: hook the pin sense of 0x18 */
static void cs4208_hs_pre_probe(struct hda_codec *codec)
{
	struct cs_spec *spec = codec->spec;

	if (!headset_mic)
		return;
	spec->hs.codec = codec;
	spec->hs.type = -1;
	mutex_init(&spec->hs.lock);
	INIT_DELAYED_WORK(&spec->hs.work, cs4208_hs_work);
	spec->hs.exec_verb = codec->core.exec_verb;
	codec->core.exec_verb = cs4208_hs_exec_verb;
}

/* after the pin parser: enable if it created a jack for pin 0x18 */
static void cs4208_hs_probe(struct hda_codec *codec)
{
	struct cs_spec *spec = codec->spec;

	if (!headset_mic || !spec->hs.codec)
		return;
	if (!snd_hda_jack_tbl_get(codec, CS4208_HS_MIC_PIN)) {
		codec_info(codec, "headset mic: no jack for pin 0x18, disabled\n");
		return;
	}
	spec->gpio_mask |= CS4208_HS_GPIO_MASK;
	spec->gpio_dir |= CS4208_HS_GPIO_MASK;
	/* the bias must not drop between two recordings */
	snd_hda_gen_fix_pin_power(codec, CS4208_HS_MIC_PIN);
	spec->gen.mic_autoswitch_hook = cs4208_hs_mic_autoswitch;
	spec->hs.enabled = 1;
}

static void cs_4208_unsol_event(struct hda_codec *codec, unsigned int res)
{
	struct cs_spec *spec = codec->spec;
	unsigned int tag = (res & AC_UNSOL_RES_TAG) >> AC_UNSOL_RES_TAG_SHIFT;

	if (spec->hs.enabled && tag == CS4208_HS_UNSOL_TAG) {
		cs4208_hs_unsol_event(codec, res);
		return;
	}
	if (spec->hs.enabled && tag == CS4208_HS_BTN_UNSOL_TAG) {
		/* headset buttons: not decoded yet, keep the raw response visible */
		codec_info(codec, "headset button response 0x%08x\n", res);
		return;
	}
	snd_hda_jack_unsol_event(codec, res);
}

static int cs_4208_suspend(struct hda_codec *codec)
{
	cs4208_hs_stop(codec, false);
	return 0;
}

/* Unsolicited HP-jack handler: re-route speaker<->headphone for a *running*
 * stream. Defined here (not at the top) because it needs get_hda_cvt_setup_4208
 * to invalidate the converter format cache before re-pointing the stream. */
static void cs_4208_hp_jack_callback(struct hda_codec *codec,
				     struct hda_jack_callback *cb)
{
	struct hda_cvt_setup *p;
	int hp = snd_hda_jack_detect(codec, 0x10);

	codec_dbg(codec, "cs_4208_hp_jack_callback hp=%d (prev=%d) tag=%u",
		  hp, cs4208_cur_hp, cs4208_cur_stream_tag);
	if (hp == cs4208_cur_hp)
		return;
	cs4208_cur_hp = hp;

	if (hp) {				/* headphones plugged */
		headphones_a1534(codec);
		cs_4208_rearm_capture_cvt(codec, 0x06);	/* internal mic ADC */
		cs_4208_rearm_capture_cvt(codec, 0x07);	/* external mic ADC */
		if (cs4208_cur_stream_tag) {
			/* Invalidate the cached converter setup so setup_stream
			 * actually reprograms the live format - otherwise 0x02
			 * keeps headphones_a1534()'s hardcoded format and plays
			 * loud garbage until PipeWire's own reopen fixes it. */
			p = get_hda_cvt_setup_4208(codec, 0x02);
			p->stream_tag = 0; p->channel_id = 0; p->format_id = 0;
			snd_hda_codec_setup_stream(codec, 0x02,
						   cs4208_cur_stream_tag, 0,
						   cs4208_cur_format);
		}
		/* Silence the speaker by disabling its output pin (0x1d) without
		 * touching the converters/stream the headphones now use.
		 * play_a1534() re-enables 0x1d on the way back to speakers. */
		snd_hda_codec_write(codec, 0x1d, 0,
				    AC_VERB_SET_PIN_WIDGET_CONTROL, 0x00);
		cs4208_hs_jack_update(codec, true, true);
	} else {				/* back to speakers */
		play_a1534(codec);
		cs_4208_rearm_capture_cvt(codec, 0x06);
		cs_4208_rearm_capture_cvt(codec, 0x07);
		if (cs4208_cur_stream_tag) {
			p = get_hda_cvt_setup_4208(codec, 0x04);
			p->stream_tag = 0; p->channel_id = 0; p->format_id = 0;
			snd_hda_codec_setup_stream(codec, 0x04,
						   cs4208_cur_stream_tag, 0,
						   cs4208_cur_format);
		}
		cs4208_hs_jack_update(codec, false, false);
	}
}

static int cs_4208_init_explicit(struct hda_codec *codec)
{
	int retval;
	codec_dbg(codec, "cs_4208_init_explicit start");

	/* Keep the standard Cirrus initialization in the custom path.  Besides
	 * configuring the generic input/output routes, cs_init() restores the
	 * CS4208 vendor coefficients and GPIO state expected after boot/resume. */
	retval = cs_init(codec);
	if (retval < 0)
		return retval;

	/* Enable live speaker<->headphone switching: fire cs_4208_hp_jack_callback
	 * on every plug/unplug interrupt from the HP pin (0x10). */
	cs4208_cur_hp = -1;
	snd_hda_jack_detect_enable_callback(codec, 0x10, cs_4208_hp_jack_callback);

	cs4208_hs_init(codec);

	codec_dbg(codec, "cs_4208_init_explicit end");
	return 0;
}

static void cs_4208_free_explicit(struct hda_codec *codec)
{
	cs4208_hs_stop(codec, true);
	kfree(codec->spec);
}

static int cs_4208_resume(struct hda_codec *codec)
{
	int err;

	/* The A1534 speaker amplifiers behind vendor widget 0x24 lose their
	 * configuration across a system sleep.  With no .resume callback the
	 * core only re-runs init + regmap sync, which restores the codec but
	 * not the amps, leaving the speakers silent until the next module
	 * probe (i.e. reboot).  Replay the probe-time amp bring-up first,
	 * mirroring the boot order of cs4208_probe().
	 *
	 * Runtime resume from power_save idle does not drop amp power, so
	 * skip the ~1000-verb replay there: power_state is PMSG_ON during a
	 * plain runtime resume and PMSG_RESUME/RESTORE after S3/S4 (also for
	 * a deferred first wake after sleep, via hda_codec_pm_complete()). */
	if (codec->core.dev.power.power_state.event != PM_EVENT_ON) {
		setup_a1534(codec);
		play_a1534(codec);
		/* No jack event follows a system resume, so the output would
		 * stay on the speakers until the next PCM open.  Route plugged
		 * headphones now; the headset detector also only answers
		 * "headset" with the headphone path set up. */
		snd_hda_jack_set_dirty_all(codec);
		if (cs4208_hp_plugged(codec)) {
			cs4208_cur_hp = -1;
			cs_4208_hp_jack_callback(codec, NULL);
		}
	}

	err = cs_4208_init_explicit(codec);
	if (err < 0)
		return err;

	snd_hda_regmap_sync(codec);
	return 0;
}

// real def is CONFIG_PM
static const struct hda_codec_ops cs_4208_patch_ops_explicit = {
	.build_controls = cs_4208_build_controls_explicit,
	.build_pcms = cs_4208_build_pcms_explicit,
	.init = cs_4208_init_explicit,
	.free = cs_4208_free_explicit,
	.unsol_event = cs_4208_unsol_event,
	.suspend = cs_4208_suspend,
	/* cs_alloc_spec() enables per-widget power saving.  Preserve the generic
	 * stream callback so capture wakes ADC 0x06 before programming its stream
	 * tag and format; without it the ADC remains in D3 and DMA never advances. */
	.stream_pm = snd_hda_gen_stream_pm,
	.resume = cs_4208_resume,
};
