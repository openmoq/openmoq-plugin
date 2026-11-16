#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <obs-module.h>

#include "codec.h"
#include "moq-output.h"

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

static bool is_parameter_set(const TrackCodec *codec, uint8_t nal_header)
{
	if (codec == &kCodecH264) {
		const uint8_t type = nal_header & 0x1f;
		return type == 7 || type == 8 || type == 13;
	}
	if (codec == &kCodecH265) {
		const uint8_t type = (nal_header >> 1) & 0x3f;
		return type >= 32 && type <= 34;
	}
	return false;
}

static size_t find_start_code(const uint8_t *data, size_t size, size_t from)
{
	for (size_t i = from; i + 3 <= size; i++) {
		if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
			return i;
	}
	return size;
}

static bool strip_parameter_sets(const TrackCodec *codec, const uint8_t *data, size_t size, std::vector<uint8_t> &out)
{
	static const uint8_t start_code[] = {0, 0, 0, 1};
	bool found = false;

	for (int pass = 0; pass < 2; pass++) {
		size_t pos = find_start_code(data, size, 0);
		while (pos < size) {
			const size_t nal = pos + 3;
			const size_t next = find_start_code(data, size, nal);
			size_t end = next;
			while (end > nal && data[end - 1] == 0)
				end--;

			const bool is_ps = end > nal && is_parameter_set(codec, data[nal]);
			if (pass == 0) {
				found = found || is_ps;
			} else if (!is_ps && end > nal) {
				out.insert(out.end(), start_code, start_code + sizeof(start_code));
				out.insert(out.end(), data + nal, data + end);
			}
			pos = next;
		}

		if (!found)
			return false;
		if (pass == 0) {
			out.clear();
			out.reserve(size);
		}
	}
	return true;
}

static bool package_packet(moq_cmaf_packager_t *packager, const TrackCodec *codec, const struct encoder_packet *packet,
			   uint32_t duration, std::vector<uint8_t> &scratch, moq_bytes_t *out)
{
	if (!packager || !codec || !packet || !packet->data || packet->size == 0)
		return false;

	moq_cmaf_packager_sample_t sample = {};
	sample.data = {packet->data, packet->size};
	if (codec->cmaf_format == MOQ_CMAF_SAMPLE_NAL_ANNEXB &&
	    strip_parameter_sets(codec, packet->data, packet->size, scratch)) {
		if (scratch.empty())
			return false;
		sample.data = {scratch.data(), scratch.size()};
	}

	sample.pts = packet->pts;
	sample.dts = packet->dts;
	sample.duration = duration;
	sample.keyframe = packet->keyframe;

	const moq_result_t rc = moq_cmaf_packager_write(packager, &sample, 1, out);
	if (rc != MOQ_OK) {
		blog(LOG_DEBUG, "[obs-moq] CMAF: packager write failed: %s", moq_strerror(rc));
		return false;
	}

	return true;
}
