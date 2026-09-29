#pragma once

#include <cstdint>
#include <cstring>

#include <obs-module.h>

#include "moq-output.h"

static moq_cmaf_codec_kind_t codec_kind_from_name(const char *codec)
{
	if (!codec)
		return MOQ_CMAF_CODEC_UNKNOWN;
	if (strcmp(codec, "h264") == 0)
		return MOQ_CMAF_CODEC_AVC;
	if (strcmp(codec, "hevc") == 0)
		return MOQ_CMAF_CODEC_HEVC;
	if (strcmp(codec, "av1") == 0)
		return MOQ_CMAF_CODEC_AV1;
	if (strcmp(codec, "aac") == 0)
		return MOQ_CMAF_CODEC_AAC;
	if (strcmp(codec, "opus") == 0)
		return MOQ_CMAF_CODEC_OPUS;
	return MOQ_CMAF_CODEC_UNKNOWN;
}

static CMAFPackagerPtr create_packager(const moq_cmaf_packager_cfg_t *cfg, const char *codec)
{
	moq_cmaf_packager_t *packager = nullptr;
	const moq_result_t rc = moq_cmaf_packager_create(nullptr, cfg, &packager);
	if (rc != MOQ_OK) {
		blog(LOG_WARNING, "[obs-moq] CMAF: packager init failed for '%s': %s", codec, moq_strerror(rc));
		return nullptr;
	}

	return CMAFPackagerPtr(packager);
}

static int64_t rescale_timestamp(int64_t v, int64_t num, int64_t den)
{
	if (den <= 0 || num <= 0 || num == den)
		return v;
	return (v / den) * num + ((v % den) * num) / den;
}

static bool package_packet(moq_cmaf_packager_t *packager, const struct encoder_packet *packet, moq_bytes_t *out)
{
	if (!packager || !packet || !packet->data || packet->size == 0)
		return false;

	const int64_t num = packet->timebase_num ? packet->timebase_num : 1;
	const int64_t den = packet->timebase_den ? packet->timebase_den : 1;
	const int64_t scale = static_cast<int64_t>(moq_cmaf_packager_timescale(packager)) * num;

	moq_cmaf_packager_sample_t sample = {};
	sample.data = {packet->data, packet->size};
	sample.pts = rescale_timestamp(packet->pts, scale, den);
	sample.dts = rescale_timestamp(packet->dts, scale, den);
	sample.keyframe = packet->keyframe;

	const moq_result_t rc = moq_cmaf_packager_write(packager, &sample, 1, out);
	if (rc != MOQ_OK) {
		return false;
	}

	return true;
}
