#pragma once

#include <cstdint>
#include <cstring>

#include <moq/cmaf.h>
#include <moq/codec_signaling.h>

struct TrackCodec {
	moq_codec_source_format_t source_format;
	moq_codec_config_format_t config_format;
	moq_cmaf_codec_kind_t cmaf_kind;
	const char *sample_entry;
	bool has_object_type_indication;
	uint8_t object_type_indication;
};

inline constexpr TrackCodec kCodecH264{
	MOQ_CODEC_SOURCE_AVC_ANNEXB, MOQ_CODEC_CONFIG_AVCC, MOQ_CMAF_CODEC_AVC, "avc1", false, 0x00};
inline constexpr TrackCodec kCodecH265{
	MOQ_CODEC_SOURCE_HEVC_ANNEXB, MOQ_CODEC_CONFIG_HVCC, MOQ_CMAF_CODEC_HEVC, "hvc1", false, 0x00};
inline constexpr TrackCodec kCodecAv1{
	MOQ_CODEC_SOURCE_AV1_OBU, MOQ_CODEC_CONFIG_AV1C, MOQ_CMAF_CODEC_AV1, "av01", false, 0x00};
inline constexpr TrackCodec kCodecAac{
	MOQ_CODEC_SOURCE_AAC_ASC, MOQ_CODEC_CONFIG_AAC_ASC, MOQ_CMAF_CODEC_AAC, "mp4a", true, 0x40};
inline constexpr TrackCodec kCodecOpus{
	MOQ_CODEC_SOURCE_OPUS_HEAD, MOQ_CODEC_CONFIG_OPUS, MOQ_CMAF_CODEC_OPUS, "opus", false, 0x00};

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
