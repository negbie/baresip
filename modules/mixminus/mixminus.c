/**
 * @file mixminus.c  Mixes N-1 audio streams for conferencing
 *
 * Copyright (C) 2021 Sebastian Reimers
 * Copyright (C) 2021 AGFEO GmbH & Co. KG
 */

#include <string.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>

enum {
	MAX_SRATE       = 48000,  /* Maximum sample rate in [Hz] */
	MAX_CHANNELS    =     2,  /* Maximum number of channels  */
	MAX_PTIME       =    60,  /* Maximum packet time in [ms] */

	AUDIO_SAMPSZ    = MAX_SRATE * MAX_CHANNELS * MAX_PTIME / 1000
};

struct mix {
	struct aubuf *ab;
	const struct audio *au;
	uint32_t srate;
	uint8_t ch;
	bool ready;
	struct le le_priv;
};

struct mixminus_enc {
	struct aufilt_enc_st af;  /* inheritance */

	const struct audio *au;
	struct list mixers;
	int16_t *sampv;
	int16_t *rsampv;
	struct auresamp resamp;
	uint32_t srate;
	uint8_t ch;
	struct le le_priv;
};

struct mixminus_dec {
	struct aufilt_dec_st af;  /* inheritance */

	const struct audio *au;
};

static struct list encs;


static void enc_destructor(void *arg)
{
	struct mixminus_enc *st = arg;
	struct mixminus_enc *enc;
	struct le *le, *lem;
	struct mix *mix;

	list_flush(&st->mixers);
	mem_deref(st->sampv);
	mem_deref(st->rsampv);
	list_unlink(&st->le_priv);

	for (le = list_head(&encs); le; le = le->next) {
		enc = le->data;
		if (!enc)
			continue;

		lem = list_head(&enc->mixers);
		while (lem) {
			mix = lem->data;
			lem = lem->next;

			if (st->au != mix->au)
				continue;

			mix->ready = false;
			list_unlink(&mix->le_priv);
			sys_msleep(25);
			mem_deref(mix);
		}
	}
}


static void mix_destructor(void *arg)
{
	struct mix *mix = arg;
	mem_deref(mix->ab);
}


static int encode_update(struct aufilt_enc_st **stp, void **ctx,
			 const struct aufilt *af, struct aufilt_prm *prm,
			 const struct audio *au)
{
	struct mixminus_enc *st, *enc;
	struct mix *mix;
	size_t psize;
	struct le *le;
	int err;
	(void)af;

	if (!stp || !ctx || !prm)
		return EINVAL;

	if (prm->fmt != AUFMT_S16LE) {
		warning("mixminus: Only S16LE is supported.\n");
		return EINVAL;
	}

	if (*stp)
		return 0;

	st = mem_zalloc(sizeof(*st), enc_destructor);
	if (!st)
		return ENOMEM;

	psize = AUDIO_SAMPSZ * sizeof(int16_t);

	st->sampv = mem_zalloc(psize, NULL);
	if (!st->sampv)
		return ENOMEM;

	st->rsampv = mem_zalloc(psize, NULL);
	if (!st->rsampv)
		return ENOMEM;

	st->au = au;
	st->ch = prm->ch;
	st->srate = prm->srate;
	auresamp_init(&st->resamp);

	list_append(&encs, &st->le_priv, st);

	/* add mix to other encs */
	for (le = list_head(&encs); le; le = le->next) {
		enc = le->data;
		if (!enc)
			continue;
		if (enc->au == au)
			continue;

		mix = mem_zalloc(sizeof(*mix), mix_destructor);
		if (!mix)
			return ENOMEM;
		psize = st->srate * st->ch * 20 / 1000;
		err = aubuf_alloc(&mix->ab, psize, 5 * psize);
		if (err)
			return err;

		mix->au = st->au; /* using audio object as id */
		mix->srate = st->srate;
		mix->ch = st->ch;
		list_append(&enc->mixers, &mix->le_priv, mix);
	}

	/* add other mixes to new enc */
	for (le = list_head(&encs); le; le = le->next) {
		enc = le->data;
		if (!enc)
			continue;
		if (enc->au == au)
			continue;

		mix = mem_zalloc(sizeof(*mix), mix_destructor);
		if (!mix)
			return ENOMEM;

		psize = enc->srate * enc->ch * 20 / 1000;
		err = aubuf_alloc(&mix->ab, psize, 5 * psize);
		if (err)
			return err;

		mix->au = enc->au; /* using audio object as id */
		mix->srate = enc->srate;
		mix->ch = enc->ch;
		list_append(&st->mixers, &mix->le_priv, mix);
	}

	*stp = (struct aufilt_enc_st *) st;

	return 0;
}


static int decode_update(struct aufilt_dec_st **stp, void **ctx,
			 const struct aufilt *af, struct aufilt_prm *prm,
			 const struct audio *au)
{
	struct mixminus_dec *st;
	(void)af;
	(void)prm;

	if (!stp || !ctx)
		return EINVAL;

	if (prm->fmt != AUFMT_S16LE) {
		warning("mixminus: Only S16LE is supported.\n");
		return EINVAL;
	}

	if (*stp)
		return 0;

	st = mem_zalloc(sizeof(*st), NULL);
	if (!st)
		return ENOMEM;

	st->au = au;

	*stp = (struct aufilt_dec_st *)st;

	return 0;
}


static void read_samp(struct aubuf *ab, int16_t *sampv, size_t sampc,
		      size_t stime)
{
	size_t i;

	for (i = 0; i < stime - 1; i++) {
		if (aubuf_cur_size(ab) < sampc) {
			sys_msleep(1);
		}
		else {
			break;
		}
	}

	aubuf_read_samp(ab, sampv, sampc);
}


static int encode(struct aufilt_enc_st *aufilt_enc_st, struct auframe *af)
{
	struct mixminus_enc *enc = (struct mixminus_enc *)aufilt_enc_st;
	size_t i, inc, outc, stime;
	struct le *lem;
	struct mix *mix;
	int16_t *sampv = af->sampv;
	int16_t *sampv_mix = enc->sampv;
	int32_t sample;
	int err = 0;

	stime = 1000 * af->sampc / (enc->srate * enc->ch);

	for (lem = list_head(&enc->mixers); lem; lem = lem->next) {
		mix = lem->data;
		if (!mix)
			continue;

		if (!audio_is_conference(mix->au))
			continue;

		mix->ready = true;

		err = auresamp_setup(&enc->resamp, mix->srate, mix->ch,
				     enc->srate, enc->ch);
		if (err) {
			warning("mixminus/auresamp_setup error (%m)\n", err);
			return err;
		}

		if (enc->resamp.resample) {
			outc = AUDIO_SAMPSZ;
			sampv_mix = enc->rsampv;

			if (enc->srate > mix->srate) {
				inc = af->sampc / enc->resamp.ratio;
			}
			else {
				inc = af->sampc * enc->resamp.ratio;
			}

			if (enc->ch == 2 && mix->ch == 1) {
				inc = inc / 2;
			}

			if (enc->ch == 1 && mix->ch == 2) {
				inc = inc * 2;
			}

			read_samp(mix->ab, enc->sampv, inc, stime);

			err = auresamp(&enc->resamp, sampv_mix, &outc,
				       enc->sampv, inc);
			if (err) {
				warning("mixminus/auresamp error (%m)\n", err);
				return err;
			}
			if (outc != af->sampc) {
				warning("mixminus/auresamp sample count "
					"error\n");
				return EINVAL;
			}
		}
		else {
			read_samp(mix->ab, sampv_mix, af->sampc, stime);
		}

		for (i = 0; i < af->sampc; i++) {
			sample = sampv[i] + sampv_mix[i];

			/* soft clipping */
			if (sample >= 32767)
				sample = 32767;
			if (sample <= -32767)
				sample = -32767;
			sampv[i] = sample;
		}
	}

	return err;
}


static int decode(struct aufilt_dec_st *aufilt_dec_st, struct auframe *af)
{
	struct mixminus_dec *dec = (struct mixminus_dec *)aufilt_dec_st;
	struct mixminus_enc *enc;
	struct le *le;
	struct le *lem;
	struct mix *mix;
	size_t num_bytes = auframe_size(af);

	for (le = list_head(&encs); le; le = le->next) {
		enc = le->data;
		if (!enc)
			continue;

		for (lem = list_head(&enc->mixers); lem; lem = lem->next) {
			mix = lem->data;

			if (!mix || dec->au != mix->au)
				continue;

			if (!mix->ready)
				continue;

			aubuf_write(mix->ab, af->sampv, num_bytes);
		}
	}

	return 0;
}


static int enable_conference(struct re_printf *pf, void *arg)
{
	struct le *le, *lec;
	struct call *call;
	struct ua *ua;
	struct audio *au;
	(void)pf;
	(void)arg;

	for (le = list_head(uag_list()); le; le = le->next) {
		ua = le->data;
		lec = NULL;

		for (lec = list_head(ua_calls(ua)); lec; lec = lec->next) {
			call = lec->data;
			info("conference with %s\n", call_peeruri(call));
			call_hold(call, false);
			au = call_audio(call);
			audio_set_conference(au, true);
		}
	}

	return 0;
}


static int debug_conference(struct re_printf *pf, void *arg)
{
	struct mixminus_enc *enc;
	struct mix *mix;
	struct le *le, *lem;
	(void)pf;
	(void)arg;

	for (le = list_head(&encs); le; le = le->next) {
		enc = le->data;
		if (!enc)
			continue;

		info("mixminus/enc au %x:"
		     "ch %d srate %d, is_conference (%s)\n",
		     enc->au, enc->ch, enc->srate,
		     audio_is_conference(enc->au) ? "true" : "false");

		for (lem = list_head(&enc->mixers); lem; lem = lem->next) {
			mix = lem->data;

			info("\tmix au %x: ch %d srate %d %H\n", mix->au, mix->ch,
			     mix->srate, aubuf_debug, mix->ab);
		}
	}

	return 0;
}


static struct aufilt mixminus = {
	LE_INIT, "mixminus", encode_update, encode, decode_update, decode
};


static const struct cmd cmdv[] = {
	{"conference", 'z', 0, "Start conference", enable_conference},
	{"conference_debug", 'Z', 0, "Debug conference", debug_conference}
};


static int module_init(void)
{
	int err;

	aufilt_register(baresip_au_pre_filtl(), &mixminus);
	err  = cmd_register(baresip_commands(), cmdv, ARRAY_SIZE(cmdv));

	return err;
}


static int module_close(void)
{
	cmd_unregister(baresip_commands(), cmdv);
	aufilt_unregister(&mixminus);
	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(mixminus) = {
	"mixminus",
	"filter",
	module_init,
	module_close
};
