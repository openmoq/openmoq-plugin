#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <moq/codec_signaling.h>

struct TrackCodec {
	moq_codec_source_format_t source_format;
	moq_codec_config_format_t config_format;
	const char *sample_entry;
	bool has_object_type_indication;
	uint8_t object_type_indication;
};

inline constexpr TrackCodec kCodecH264{MOQ_CODEC_SOURCE_AVC_ANNEXB, MOQ_CODEC_CONFIG_AVCC, "avc1", false, 0x00};
inline constexpr TrackCodec kCodecH265{MOQ_CODEC_SOURCE_HEVC_ANNEXB, MOQ_CODEC_CONFIG_HVCC, "hvc1", false, 0x00};
inline constexpr TrackCodec kCodecAv1{MOQ_CODEC_SOURCE_AV1_OBU, MOQ_CODEC_CONFIG_AV1C, "av01", false, 0x00};
inline constexpr TrackCodec kCodecAac{MOQ_CODEC_SOURCE_AAC_ASC, MOQ_CODEC_CONFIG_AAC_ASC, "mp4a", true, 0x40};
inline constexpr TrackCodec kCodecOpus{MOQ_CODEC_SOURCE_OPUS_HEAD, MOQ_CODEC_CONFIG_OPUS, "opus", false, 0x00};

inline const TrackCodec *ResolveTrackCodec(const char *codec)
{
	if (!codec)
		return nullptr;
	if (strcmp(codec, "h264") == 0)
		return &kCodecH264;
	if (strcmp(codec, "hevc") == 0)
		return &kCodecH265;
	if (strcmp(codec, "av1") == 0)
		return &kCodecAv1;
	if (strcmp(codec, "aac") == 0)
		return &kCodecAac;
	if (strcmp(codec, "opus") == 0)
		return &kCodecOpus;
	return nullptr;
}

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
