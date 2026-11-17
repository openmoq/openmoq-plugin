#pragma once
#include <obs-module.h>

#include "extra-canvas.h"

#include <chrono>
#include <memory>
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
#include <moq/cmaf_packager.h>

struct CMAFPackagerDeleter {
	void operator()(moq_cmaf_packager_t *p) const { moq_cmaf_packager_destroy(p); }
};

using CMAFPackagerPtr = std::unique_ptr<moq_cmaf_packager_t, CMAFPackagerDeleter>;

struct TrackCodec;

struct video_config {
	uint32_t video_width;
	uint32_t video_height;
	uint32_t fps_num;
	uint32_t fps_den;
	uint32_t keyint_sec;
	uint64_t bitrate;
};

struct audio_config {
	uint32_t samplerate;
	uint32_t channel_count;
	uint32_t frame_size;
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

	bool has_alt_group = false;
	int alt_group = 0;

	video_config conf = {};
	std::vector<uint8_t> init_data;
	std::string codec;
	const TrackCodec *track_codec = nullptr;

	CMAFPackagerPtr packager;
	uint32_t sample_duration = 0;

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
	bool SplitNamespace(std::string &error);
	bool LoadVideoTracks();
	bool AddVideoTrack(size_t encoder_idx, bool multitrack);
	bool LoadVideoEncoderSettings(MOQVideoTrack &vt);
	bool LoadAudioEncoderSettings();
	bool InitCMAFVideoPackager(MOQVideoTrack &vt);
	bool InitCMAFAudioPackager();
	moq_media_track_t *CreateVideoTrack(MOQVideoTrack &vt, moq_media_sender_t *new_sender);
	moq_media_track_t *CreateVideoTrackFromPacket(MOQVideoTrack &vt, moq_media_sender_t *cur_sender,
						      struct encoder_packet *packet);
	moq_media_track_t *CreateAudioTrack(moq_media_sender_t *new_sender);
	void WriteMediaObject(moq_media_track_t *track, struct encoder_packet *packet, const uint8_t *data, size_t size,
			      bool is_sync, bool starts_group, bool ends_group);
	void SendVideoPacket(MOQVideoTrack &vt, struct encoder_packet *packet);
	void SendAudioPacket(struct encoder_packet *packet, bool starts_group, bool ends_group);
	bool ResolveServiceConfig();
	bool SetupExtraVideo(const char *canvas_uuid);
	void ReleaseExtraVideo();
	obs_encoder_group_t *CreateEncoderGroup();
	bool LoadEndpointSettings(obs_service_t *service);
	void LoadContainerSettings(obs_service_t *service);
	void ResolveAudioGroupFrames(obs_encoder_t *aenc);
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

	audio_config audio_conf;
	endpoint_config endpoint_conf;

	std::vector<uint8_t> audio_init_data;
	std::string audio_codec;
	const TrackCodec *audio_track_codec = nullptr;

	std::string url;
	moq_namespace_t namespace_val;

	std::string stream_key;
	std::vector<moq_bytes_t> ns_bytes;

	moq_media_sender_t *sender = nullptr;
	std::vector<MOQVideoTrack> video_tracks;
	moq_media_track_t *audio_track = nullptr;

	bool cmaf_enabled = false;

	uint32_t audio_group_frames = 0;
	uint32_t audio_group_sent = 0;

	CMAFPackagerPtr audio_packager;
	std::vector<uint8_t> cmaf_scratch;
};

void register_moq_output();