#pragma once
#include <obs-module.h>

#include "extra-canvas.h"

#include <chrono>
#include <mutex>
#include <thread>
#include <atomic>
#include <string>
#include <vector>

#include <moq/url.h>
#include <moq/wire.h>
#include <moq/rcbuf.h>
#include <moq/media_object.h>
#include <moq/media_sender.h>

struct TrackCodec;

struct video_config {
	uint32_t video_width;
	uint32_t video_height;
	uint32_t fps_num;
	uint32_t fps_den;
	uint64_t bitrate;
};

struct audio_config {
	uint32_t samplerate;
	std::string channels;
	uint64_t bitrate;
};

struct endpoint_config {
	bool skip_tls_verify = false;
	moq_version_t draft_version = (moq_version_t)0; // 0 : negotiate automatically
};

struct MOQVideoTrack {
	size_t encoder_idx = 0;
	std::string name;

	// CMSF altGroup: tracks sharing a value are quality alternatives of one
	// another. Should not be mixed between different canvas
	bool has_alt_group = false;
	int alt_group = 0;

	video_config conf = {};
	std::vector<uint8_t> init_data;
	std::string codec;
	const TrackCodec *track_codec = nullptr;

	moq_media_track_t *track = nullptr;
};

class MOQOutput {
public:
	MOQOutput(obs_data_t *settings, obs_output_t *output);
	~MOQOutput();

	bool Start();
	void Stop(bool signal = true);
	void Data(struct encoder_packet *packet);

	inline size_t GetTotalBytes() { return total_bytes_sent; }

	inline int GetConnectTime() { return connect_time_ms; }

private:
	void StartThread();
	void SplitNamespace();
	bool LoadVideoTracks();
	bool LoadVideoEncoderSettings(MOQVideoTrack &vt);
	bool LoadAudioEncoderSettings();
	moq_media_track_t *CreateVideoTrack(MOQVideoTrack &vt, moq_media_sender_t *new_sender);
	moq_media_track_t *CreateVideoTrackFromPacket(MOQVideoTrack &vt, moq_media_sender_t *cur_sender,
						      struct encoder_packet *packet);
	moq_media_track_t *CreateAudioTrack(moq_media_sender_t *new_sender);
	void WriteMediaObject(moq_media_track_t *track, struct encoder_packet *packet, const uint8_t *data,
			      size_t size, bool is_sync, bool starts_group, bool ends_group);
	void SendVideoPacket(MOQVideoTrack &vt, struct encoder_packet *packet);
	void SendAudioPacket(struct encoder_packet *packet);
	bool ResolveServiceConfig();
	bool SetupExtraVideo(const char *canvas_uuid);
	void ReleaseExtraVideo();
	bool LoadEndpointSettings(obs_service_t *service);
	bool Connect();

	static void OnReady(void *ctx, moq_media_sender_t *sender);
	static void OnClosed(void *ctx, moq_media_sender_t *sender, bool is_fatal, uint64_t fatal_code);
	static void OnTrackClosed(void *ctx, moq_media_sender_t *sender, moq_media_track_t *track);

	obs_output_t *output;

	std::mutex start_stop_mutex;
	std::mutex sender_mutex;
	std::thread start_stop_thread;

	std::atomic<size_t> total_bytes_sent;
	std::atomic<int> connect_time_ms;
	int64_t start_time_ns = 0;

	int64_t epoch_offset_us = 0;

	std::atomic<bool> stopping;
	std::atomic<bool> running;
	std::atomic<bool> got_ready;

	obs_encoder_t *extra_encoder = nullptr;
	extra_canvas_info extra_conf;

	obs_encoder_group_t *encoder_group = nullptr;

	audio_config audio_conf;
	endpoint_config endpoint_conf;

	std::vector<uint8_t> audio_init_data;
	std::string audio_codec;

	std::string url;
	moq_namespace_t namespace_val;

	std::string stream_key;
	std::vector<moq_bytes_t> ns_bytes;

	moq_media_sender_t *sender = nullptr;
	std::vector<MOQVideoTrack> video_tracks;
	moq_media_track_t *audio_track = nullptr;
};

void register_moq_output();