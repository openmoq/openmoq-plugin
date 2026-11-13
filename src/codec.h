#pragma once

#include <cstdint>
#include <cstring>

#include <moq/cmaf_packager.h>
#include <moq/codec_signaling.h>

struct TrackCodec {
	moq_codec_source_format_t source_format;
	moq_codec_config_format_t config_format;
	moq_cmaf_sample_entry_t cmaf_entry;
	moq_cmaf_sample_format_t cmaf_format;
	const char *sample_entry;
	bool has_object_type_indication;
	uint8_t object_type_indication;
};

inline constexpr TrackCodec kCodecH264{
	MOQ_CODEC_SOURCE_AVC_ANNEXB,
	MOQ_CODEC_CONFIG_AVCC,
	MOQ_CMAF_ENTRY_AVC1,
	MOQ_CMAF_SAMPLE_NAL_ANNEXB,
	"avc1",
	false,
	0x00,
};
inline constexpr TrackCodec kCodecH265{
	MOQ_CODEC_SOURCE_HEVC_ANNEXB,
	MOQ_CODEC_CONFIG_HVCC,
	MOQ_CMAF_ENTRY_HVC1,
	MOQ_CMAF_SAMPLE_NAL_ANNEXB,
	"hvc1",
	false,
	0x00,
};
inline constexpr TrackCodec kCodecAv1{
	MOQ_CODEC_SOURCE_AV1_OBU,
	MOQ_CODEC_CONFIG_AV1C,
	MOQ_CMAF_ENTRY_AV01,
	MOQ_CMAF_SAMPLE_AV1_OBU,
	"av01",
	false,
	0x00,
};
inline constexpr TrackCodec kCodecAac{
	MOQ_CODEC_SOURCE_AAC_ASC,
	MOQ_CODEC_CONFIG_AAC_ASC,
	MOQ_CMAF_ENTRY_MP4A,
	MOQ_CMAF_SAMPLE_AAC_RAW,
	"mp4a",
	true,
	0x40,
};
inline constexpr TrackCodec kCodecOpus{
	MOQ_CODEC_SOURCE_OPUS_HEAD,
	MOQ_CODEC_CONFIG_OPUS,
	MOQ_CMAF_ENTRY_OPUS,
	MOQ_CMAF_SAMPLE_OPUS_PACKET,
	"opus",
	false,
	0x00,
};

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
