#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <moq/codec_signaling.h>

#include "codec.h"

inline std::vector<uint8_t> BuildInitData(const char *codec, const uint8_t *src, size_t len)
{
	const TrackCodec *tc = ResolveTrackCodec(codec);
	if (!tc || !src || len == 0)
		return {};

	moq_codec_init_data_cfg_t cfg;
	moq_codec_init_data_cfg_init(&cfg);
	cfg.source_format = tc->source_format;
	cfg.source = {src, len};

	size_t need = 0;
	moq_result_t r = moq_codec_init_data_build(&cfg, nullptr, 0, &need);
	if ((r != MOQ_ERR_BUFFER && r != MOQ_OK) || need == 0)
		return {};

	std::vector<uint8_t> init_data(need);
	if (moq_codec_init_data_build(&cfg, init_data.data(), init_data.size(), &need) != MOQ_OK)
		return {};

	init_data.resize(need);
	return init_data;
}

inline std::string BuildCodecString(const char *codec, const std::vector<uint8_t> &init_data)
{
	const TrackCodec *tc = ResolveTrackCodec(codec);
	if (!tc)
		return {};

	moq_codec_string_cfg_t cfg;
	moq_codec_string_cfg_init(&cfg);
	cfg.config_format = tc->config_format;
	cfg.sample_entry = moq_bytes_cstr(tc->sample_entry);
	cfg.has_mp4_object_type_indication = tc->has_object_type_indication;
	cfg.mp4_object_type_indication = tc->object_type_indication;
	cfg.decoder_config = {init_data.data(), init_data.size()};

	uint8_t buf[64];
	size_t need = 0;
	if (moq_codec_string_format(&cfg, buf, sizeof(buf), &need) == MOQ_OK)
		return std::string((const char *)buf, need);

	return {};
}
