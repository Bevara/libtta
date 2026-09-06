/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / True Audio (TTA) decoder filter, based on
 *  libtta.
 *
 *  TTA is a lossless codec; a .tta file carries its own header (rate, channel
 *  count, bit depth, total samples) so the whole file is taken in one go and
 *  decoded into raw PCM.
 *
 *  One caveat worth stating plainly: libtta's C API is a **singleton**. Every
 *  entry point - tta_decoder_new, tta_decoder_process_stream,
 *  tta_decoder_done - works on one hidden global decoder rather than on a
 *  handle, so two TTA streams cannot be decoded at the same time in the same
 *  module. A second concurrent instance is refused here rather than allowed to
 *  corrupt the first one's state; that is a real limitation of the library, not
 *  a shortcut taken by this filter.
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <string.h>
#include <stdlib.h>

#include <libtta.h>

/* Tracks the singleton: set while an instance holds the global decoder. */
static Bool tta_decoder_busy = GF_FALSE;

typedef struct
{
	/* must stay first: the callbacks receive this pointer and cast it back */
	TTA_io_callback iocb;
	const u8 *data;
	u32 size;
	u32 pos;
} GF_TTAReader;

typedef struct
{
	GF_FilterPid *ipid, *opid;
	GF_TTAReader reader;
	Bool owns_decoder;
	u32 sample_rate, nb_chan, bps;
} GF_TTADecCtx;

static TTAint32 CALLBACK ttadec_read(TTA_io_callback *iocb, TTAuint8 *buffer, TTAuint32 size)
{
	GF_TTAReader *r = (GF_TTAReader *)iocb;
	u32 avail = (r->pos < r->size) ? (r->size - r->pos) : 0;
	if (size > avail)
		size = avail;
	if (size)
	{
		memcpy(buffer, r->data + r->pos, size);
		r->pos += size;
	}
	return (TTAint32)size;
}

static TTAint32 CALLBACK ttadec_write(TTA_io_callback *iocb, TTAuint8 *buffer, TTAuint32 size)
{
	/* decode only; libtta still wants the slot filled */
	(void)iocb;
	(void)buffer;
	(void)size;
	return 0;
}

static TTAint64 CALLBACK ttadec_seek(TTA_io_callback *iocb, TTAint64 offset)
{
	GF_TTAReader *r = (GF_TTAReader *)iocb;
	if ((offset < 0) || ((TTAuint64)offset > r->size))
		return -1;
	r->pos = (u32)offset;
	return offset;
}

static GF_Err ttadec_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	GF_TTADecCtx *ctx = (GF_TTADecCtx *)gf_filter_get_udta(filter);

	if (is_remove)
	{
		if (ctx->opid)
		{
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	if (tta_decoder_busy && !ctx->owns_decoder)
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[TTADec] libtta's decoder is a singleton and is already in use; a second TTA stream cannot be decoded at the same time\n"));
		return GF_NOT_SUPPORTED;
	}

	ctx->ipid = pid;
	gf_filter_pid_set_framing_mode(pid, GF_TRUE);

	if (!ctx->opid)
		ctx->opid = gf_filter_pid_new(filter);

	/* Corrected in process() once the file header has been read; GPAC resolves
	 * the graph from these before any data flows. */
	ctx->sample_rate = 44100;
	ctx->nb_chan = 2;
	ctx->bps = 16;

	gf_filter_pid_copy_properties(ctx->opid, pid);
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STREAM_TYPE, &PROP_UINT(GF_STREAM_AUDIO));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_RAW));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_AUDIO_FORMAT, &PROP_UINT(GF_AUDIO_FMT_S16));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_SAMPLE_RATE, &PROP_UINT(ctx->sample_rate));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_TIMESCALE, &PROP_UINT(ctx->sample_rate));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_NUM_CHANNELS, &PROP_UINT(ctx->nb_chan));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CHANNEL_LAYOUT,
	                           &PROP_LONGUINT(GF_AUDIO_CH_FRONT_LEFT | GF_AUDIO_CH_FRONT_RIGHT));

	return GF_OK;
}

static GF_Err ttadec_process(GF_Filter *filter)
{
	GF_FilterPacket *pck, *dst_pck;
	u8 *data, *output;
	u32 size;
	TTA_info info;
	GF_TTADecCtx *ctx = (GF_TTADecCtx *)gf_filter_get_udta(filter);

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			gf_filter_pid_set_eos(ctx->opid);
			return GF_EOS;
		}
		return GF_OK;
	}
	data = (u8 *)gf_filter_pck_get_data(pck, &size);
	if (!data || (size < 22))
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[TTADec] File too short to hold a TTA header\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}
	if (memcmp(data, "TTA1", 4))
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[TTADec] Not a TTA1 file\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	ctx->reader.iocb.read = ttadec_read;
	ctx->reader.iocb.write = ttadec_write;
	ctx->reader.iocb.seek = ttadec_seek;
	ctx->reader.data = data;
	ctx->reader.size = size;
	ctx->reader.pos = 0;

	tta_decoder_new(&ctx->reader.iocb);
	tta_decoder_busy = GF_TRUE;
	ctx->owns_decoder = GF_TRUE;

	memset(&info, 0, sizeof(info));
	if (tta_decoder_init_get_info(&info) != 0)
	{
		tta_decoder_done();
		tta_decoder_busy = GF_FALSE;
		ctx->owns_decoder = GF_FALSE;
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[TTADec] Could not read the TTA header\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	if ((info.bps != 16) || !info.nch || !info.sps || !info.samples)
	{
		/* 8- and 24-bit TTA exist; converting them to the S16 the pid carries
		 * would be a lossy step, so they are refused rather than silently
		 * degraded. */
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[TTADec] Unsupported TTA stream: %u channels, %u bits, %u Hz\n",
		                                    info.nch, info.bps, info.sps));
		tta_decoder_done();
		tta_decoder_busy = GF_FALSE;
		ctx->owns_decoder = GF_FALSE;
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_NOT_SUPPORTED;
	}

	if ((info.sps != ctx->sample_rate) || (info.nch != ctx->nb_chan))
	{
		ctx->sample_rate = info.sps;
		ctx->nb_chan = info.nch;
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_SAMPLE_RATE, &PROP_UINT(info.sps));
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_TIMESCALE, &PROP_UINT(info.sps));
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_NUM_CHANNELS, &PROP_UINT(info.nch));
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CHANNEL_LAYOUT,
		                           &PROP_LONGUINT((info.nch == 2)
		                                              ? (GF_AUDIO_CH_FRONT_LEFT | GF_AUDIO_CH_FRONT_RIGHT)
		                                              : GF_AUDIO_CH_FRONT_CENTER));
	}

	{
		u32 out_size = info.samples * info.nch * 2;
		int written;

		dst_pck = gf_filter_pck_new_alloc(ctx->opid, out_size, &output);
		if (!dst_pck)
		{
			tta_decoder_done();
			tta_decoder_busy = GF_FALSE;
			ctx->owns_decoder = GF_FALSE;
			gf_filter_pid_drop_packet(ctx->ipid);
			return GF_OUT_OF_MEM;
		}

		written = tta_decoder_process_stream(output, out_size, NULL);
		tta_decoder_done();
		tta_decoder_busy = GF_FALSE;
		ctx->owns_decoder = GF_FALSE;

		if (written <= 0)
		{
			gf_filter_pck_discard(dst_pck);
			gf_filter_pid_drop_packet(ctx->ipid);
			GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[TTADec] Failed to decode the stream\n"));
			return GF_NON_COMPLIANT_BITSTREAM;
		}

		gf_filter_pck_set_cts(dst_pck, 0);
		gf_filter_pck_set_duration(dst_pck, info.samples);
		gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
		gf_filter_pck_send(dst_pck);
	}

	gf_filter_pid_drop_packet(ctx->ipid);
	gf_filter_pid_set_eos(ctx->opid);
	return GF_EOS;
}

static void ttadec_finalize(GF_Filter *filter)
{
	GF_TTADecCtx *ctx = (GF_TTADecCtx *)gf_filter_get_udta(filter);
	if (ctx->owns_decoder)
	{
		tta_decoder_done();
		tta_decoder_busy = GF_FALSE;
		ctx->owns_decoder = GF_FALSE;
	}
}

static const GF_FilterCapability TTADecCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_FILE_EXT, "tta"),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_MIME, "audio/x-tta|audio/tta"),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_AUDIO),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
};

GF_FilterRegister TTADecoderRegister = {
	.name = "ttadec",
	GF_FS_SET_DESCRIPTION("True Audio (TTA) decoder")
		GF_FS_SET_HELP("This filter decodes True Audio (TTA) lossless files using libtta. Note that libtta's decoder is a singleton, so only one TTA stream can be decoded at a time.")
			.private_size = sizeof(GF_TTADecCtx),
	SETCAPS(TTADecCaps),
	.configure_pid = ttadec_configure_pid,
	.process = ttadec_process,
	.finalize = ttadec_finalize,
};

const GF_FilterRegister *EMSCRIPTEN_KEEPALIVE ttadec_register(GF_FilterSession *session)
{
	return &TTADecoderRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_ttadec(void) {
    gf_filter_auto_register("ttadec", ttadec_register);
}
