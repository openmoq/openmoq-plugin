#include "moq-output.h"
#include "codec.h"
#include "codec-signaling.h"
#include "moq-service.h"
#include "moq-cmaf.h"
#include "extra-canvas.h"

#include <util/platform.h>
#include <obs.hpp>
#include <obs-avc.h>
#include <obs-hevc.h>

#include <algorithm>

#define VIDEO_TIMESCALE 1000000u
#define MOQ_HANDSHAKE_TIMEOUT_US 5000000ull

#define CMAF_VIDEO_TIMESCALE 90000u

#define AUDIO_GROUP_FRAMES_MAX 1024u
#define AUDIO_GROUP_DEFAULT_SEC 1u

#define AUDIO_PUBLISHER_PRIORITY 64u
#define VIDEO_PUBLISHER_PRIORITY 128u

static constexpr size_t kMainEncoderIdx = 0;
static constexpr size_t kFirstExtraEncoderIdx = 1;

MOQOutput::MOQOutput(obs_data_t *settings, obs_output_t *output) : output(output)
{
	blog(LOG_INFO, "[obs-moq] output created");
}

MOQOutput::~MOQOutput()
{
	blog(LOG_INFO, "[obs-moq] output destroying");
	Stop(false);

	std::lock_guard<std::mutex> lock(start_stop_mutex);
	if (start_stop_thread.joinable()) {
		start_stop_thread.join();
	}
}

static constexpr size_t kMaxNamespaceFields = 32;

static bool IsNamespaceLiteral(int c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static int NamespaceHexDigit(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

static bool DecodeNamespaceField(char *field, size_t &len, std::string &error)
{
	char msg[64];
	size_t in = 0;
	size_t out = 0;

	while (in < len) {
		int c = (unsigned char)field[in];

		if (IsNamespaceLiteral(c)) {
			field[out++] = field[in++];
			continue;
		}
		if (c != '.') {
			if (c > ' ' && c < 0x7f)
				snprintf(msg, sizeof(msg), "write '%c' as .%02x", c, c);
			else
				snprintf(msg, sizeof(msg), "write byte 0x%02x as .%02x", c, c);
			error = msg;
			return false;
		}

		int hi = in + 1 < len ? NamespaceHexDigit((unsigned char)field[in + 1]) : -1;
		int lo = hi < 0 || in + 2 >= len ? -1 : NamespaceHexDigit((unsigned char)field[in + 2]);
		if (lo < 0) {
			error = "'.' must be followed by two lowercase hex digits";
			return false;
		}

		c = hi << 4 | lo;
		if (IsNamespaceLiteral(c)) {
			snprintf(msg, sizeof(msg), "write .%c%c as '%c'", field[in + 1], field[in + 2], c);
			error = msg;
			return false;
		}
		field[out++] = (char)c;
		in += 3;
	}
	len = out;
	return true;
}

bool MOQOutput::SplitNamespace(std::string &error)
{
	ns_bytes.clear();
	char *base = stream_key.data();
	size_t start = 0;
	for (size_t i = 0; i <= stream_key.size(); ++i) {
		bool sep = (i == stream_key.size()) || stream_key[i] == '-';
		if (sep) {
			if (i > start) {
				if (ns_bytes.size() == kMaxNamespaceFields) {
					error = "more than 32 fields";
					return false;
				}
				size_t len = i - start;
				if (!DecodeNamespaceField(base + start, len, error))
					return false;
				ns_bytes.push_back({(const uint8_t *)(base + start), len});
			}
			start = i + 1;
		}
	}
	if (ns_bytes.empty()) {
		error = "no fields";
		return false;
	}
	namespace_val.parts = ns_bytes.data();
	namespace_val.count = ns_bytes.size();
	return true;
}

bool MOQOutput::LoadVideoEncoderSettings(MOQVideoTrack &vt)
{
	obs_encoder_t *venc = obs_output_get_video_encoder2(output, vt.encoder_idx);
	if (!venc) {
		blog(LOG_WARNING, "[obs-moq] no video encoder assigned at index %zu", vt.encoder_idx);
		obs_output_set_last_error(output, obs_module_text("Error.NoVideoEncoder"));
		return false;
	}

	OBSDataAutoRelease settings = obs_encoder_get_settings(venc);
	vt.conf.bitrate = (uint64_t)obs_data_get_int(settings, "bitrate") * 1000;
	vt.conf.keyint_sec = (uint32_t)obs_data_get_int(settings, "keyint_sec");

	const char *codec = obs_encoder_get_codec(venc);
	vt.conf.video_width = obs_encoder_get_width(venc);
	vt.conf.video_height = obs_encoder_get_height(venc);

	const struct video_output_info *voi = video_output_get_info(obs_encoder_video(venc));
	if (!voi) {
		blog(LOG_WARNING, "[obs-moq] video encoder at index %zu has no video", vt.encoder_idx);
		obs_output_set_last_error(output, obs_module_text("Error.NoVideoEncoder"));
		return false;
	}
	vt.conf.fps_num = voi->fps_num;
	vt.conf.fps_den = voi->fps_den;

	uint8_t *extra = nullptr;
	size_t extra_size = 0;
	obs_encoder_get_extra_data(venc, &extra, &extra_size);
	vt.init_data = BuildInitData(codec, extra, extra_size);
	vt.codec = BuildCodecString(codec, vt.init_data);
	vt.track_codec = ResolveTrackCodec(codec);
	return true;
}

bool MOQOutput::LoadAudioEncoderSettings()
{
	obs_encoder_t *aenc = obs_output_get_audio_encoder(output, 0);
	if (!aenc) {
		blog(LOG_WARNING, "[obs-moq] no audio encoder assigned");
		obs_output_set_last_error(output, obs_module_text("Error.NoAudioEncoder"));
		return false;
	}

	OBSDataAutoRelease settings = obs_encoder_get_settings(aenc);
	audio_conf.bitrate = (uint64_t)obs_data_get_int(settings, "bitrate") * 1000;
	audio_t *audio = obs_encoder_audio(aenc);
	audio_conf.samplerate = obs_encoder_get_sample_rate(aenc);
	audio_conf.channel_count = (uint32_t)audio_output_get_channels(audio);
	audio_conf.channels = std::to_string(audio_conf.channel_count);

	const char *codec = obs_encoder_get_codec(aenc);

	uint8_t *extra = nullptr;
	size_t extra_size = 0;
	obs_encoder_get_extra_data(aenc, &extra, &extra_size);
	audio_init_data = BuildInitData(codec, extra, extra_size);
	audio_codec = BuildCodecString(codec, audio_init_data);
	audio_track_codec = ResolveTrackCodec(codec);

	ResolveAudioGroupFrames(aenc);

	return true;
}

void MOQOutput::ResolveAudioGroupFrames(obs_encoder_t *aenc)
{
	const size_t frame_size = obs_encoder_get_frame_size(aenc);
	audio_conf.frame_size = (uint32_t)frame_size;
	const uint32_t keyint_sec = video_tracks.empty() ? 0 : video_tracks.front().conf.keyint_sec;
	const uint64_t group_sec = keyint_sec ? keyint_sec : AUDIO_GROUP_DEFAULT_SEC;
	audio_group_frames = 1;
	if (frame_size > 0 && audio_conf.samplerate > 0)
		audio_group_frames = (uint32_t)std::clamp<uint64_t>(
			(group_sec * audio_conf.samplerate + frame_size / 2) / frame_size, 1, AUDIO_GROUP_FRAMES_MAX);
}

bool MOQOutput::LoadEndpointSettings(obs_service_t *service)
{
	OBSDataAutoRelease resolved = obs_service_defaults(obs_service_get_type(service));
	if (!resolved) {
		obs_output_signal_stop(output, OBS_OUTPUT_ERROR);
		return false;
	}

	OBSDataAutoRelease settings = obs_service_get_settings(service);
	obs_data_apply(resolved, settings);

	endpoint_conf.skip_tls_verify = obs_data_get_bool(resolved, kSettingSkipTlsVerify);
	endpoint_conf.draft_version = (moq_version_t)obs_data_get_int(resolved, kSettingDraftVersion);

	return true;
}

void MOQOutput::LoadContainerSettings(obs_service_t *service)
{
	OBSDataAutoRelease settings = obs_service_get_settings(service);
	cmaf_enabled = strcmp(obs_data_get_string(settings, kSettingContainer), kContainerCMAF) == 0;
}

bool MOQOutput::InitCMAFVideoPackager(MOQVideoTrack &vt)
{
	if (!vt.track_codec) {
		blog(LOG_WARNING, "[obs-moq] CMAF: unsupported video codec");
		obs_output_set_last_error(output, obs_module_text("Error.NoVideoEncoder"));
		return false;
	}

	moq_cmaf_packager_cfg_t cfg;
	moq_cmaf_packager_cfg_init(&cfg);
	cfg.sample_entry = vt.track_codec->cmaf_entry;
	cfg.sample_format = vt.track_codec->cmaf_format;
	cfg.codec_config = {vt.init_data.data(), vt.init_data.size()};
	cfg.timescale = CMAF_VIDEO_TIMESCALE;
	cfg.width = vt.conf.video_width;
	cfg.height = vt.conf.video_height;
	// OBS advances pts/dts by fps_den per frame, so they are already in 1/fps_num units.
	cfg.source_timebase_num = 1;
	cfg.source_timebase_den = vt.conf.fps_num;
	cfg.rebase_timestamps = true;

	vt.sample_duration =
		vt.conf.fps_num ? (uint32_t)((uint64_t)CMAF_VIDEO_TIMESCALE * vt.conf.fps_den / vt.conf.fps_num) : 0;
	if (vt.sample_duration == 0)
		vt.sample_duration = 1;

	vt.packager = create_packager(&cfg, vt.track_codec->sample_entry);
	if (!vt.packager) {
		obs_output_set_last_error(output, obs_module_text("Error.NoVideoEncoder"));
		return false;
	}
	return true;
}

bool MOQOutput::InitCMAFAudioPackager()
{
	if (!audio_track_codec) {
		blog(LOG_WARNING, "[obs-moq] CMAF: unsupported audio codec");
		obs_output_set_last_error(output, obs_module_text("Error.NoAudioEncoder"));
		return false;
	}

	moq_cmaf_packager_cfg_t cfg;
	moq_cmaf_packager_cfg_init(&cfg);
	if (audio_conf.frame_size == 0) {
		blog(LOG_WARNING, "[obs-moq] CMAF: the audio encoder does not report a frame size");
		obs_output_set_last_error(output, obs_module_text("Error.NoAudioEncoder"));
		return false;
	}

	cfg.sample_entry = audio_track_codec->cmaf_entry;
	cfg.sample_format = audio_track_codec->cmaf_format;
	cfg.codec_config = {audio_init_data.data(), audio_init_data.size()};
	cfg.timescale = 0;
	cfg.samplerate = audio_conf.samplerate;
	cfg.channel_count = audio_conf.channel_count;
	cfg.avg_bitrate = static_cast<uint32_t>(audio_conf.bitrate);
	cfg.rebase_timestamps = true;

	audio_packager = create_packager(&cfg, audio_track_codec->sample_entry);
	if (!audio_packager) {
		obs_output_set_last_error(output, obs_module_text("Error.NoAudioEncoder"));
		return false;
	}
	return true;
}

static std::vector<std::string> read_extra_canvas_uuids(obs_service_t *service)
{
	std::vector<std::string> uuids;
	if (!service)
		return uuids;

	OBSDataAutoRelease settings = obs_service_get_settings(service);
	if (!settings)
		return uuids;

	for (const extra_canvas_entry &entry : extra_canvas_list()) {
		if (obs_data_get_bool(settings, ExtraCanvasSettingKey(entry.uuid).c_str()))
			uuids.push_back(entry.uuid);
	}

	const std::vector<std::string> unavailable = UnavailableExtraCanvases(settings);
	uuids.insert(uuids.end(), unavailable.begin(), unavailable.end());
	return uuids;
}

bool MOQOutput::SetupExtraVideo(const std::vector<std::string> &canvas_uuids)
{
	const size_t max_extra = MAX_OUTPUT_VIDEO_ENCODERS - kFirstExtraEncoderIdx;
	if (canvas_uuids.size() > max_extra) {
		blog(LOG_WARNING, "[obs-moq] %zu extra canvases selected, an output fits at most %zu",
		     canvas_uuids.size(), max_extra);
		obs_output_set_last_error(output, obs_module_text("Error.TooManyExtraCanvases"));
		return false;
	}

	std::vector<extra_canvas_info> canvases;
	for (const std::string &uuid : canvas_uuids) {
		extra_canvas_info canvas;
		if (!extra_canvas_resolve(uuid.c_str(), &canvas)) {
			blog(LOG_WARNING, "[obs-moq] the configured extra canvas %s could not be resolved",
			     uuid.c_str());
			obs_output_set_last_error(output, obs_module_text("Error.NoExtraCanvas"));
			return false;
		}
		canvases.push_back(canvas);
	}

	obs_encoder_t *main_encoder = obs_output_get_video_encoder(output);
	if (!main_encoder) {
		blog(LOG_WARNING, "[obs-moq] no video encoder assigned");
		obs_output_set_last_error(output, obs_module_text("Error.NoVideoEncoder"));
		return false;
	}

	const char *enc_id = obs_encoder_get_id(main_encoder);
	OBSDataAutoRelease settings = obs_encoder_get_settings(main_encoder);

	for (const extra_canvas_info &canvas : canvases) {
		const size_t encoder_idx = kFirstExtraEncoderIdx + extra_encoders.size();
		const std::string name = "moq_extra_video_" + std::to_string(encoder_idx);
		obs_encoder_t *venc = obs_video_encoder_create(enc_id, name.c_str(), settings, nullptr);
		if (!venc) {
			blog(LOG_WARNING, "[obs-moq] failed to create the extra video encoder '%s'", enc_id);
			obs_output_set_last_error(output, obs_module_text("Error.NoVideoEncoder"));
			return false;
		}

		obs_encoder_set_video(venc, canvas.video);
		extra_encoders.push_back(venc);
		extra_canvas_names.push_back(canvas.name);
	}

	for (size_t i = 0; i < extra_encoders.size(); i++) {
		const size_t encoder_idx = kFirstExtraEncoderIdx + i;
		obs_output_set_video_encoder2(output, extra_encoders[i], encoder_idx);

		blog(LOG_INFO, "[obs-moq] extra canvas %ux%u @ %.3f fps attached to encoder slot %zu (cloned '%s')",
		     canvases[i].width, canvases[i].height, (double)canvases[i].fps_num / (double)canvases[i].fps_den,
		     encoder_idx, enc_id);
	}
	return true;
}

void MOQOutput::ReleaseExtraVideo()
{
	if (!obs_output_active(output)) {
		for (size_t idx = kFirstExtraEncoderIdx; idx < MAX_OUTPUT_VIDEO_ENCODERS; idx++)
			obs_output_set_video_encoder2(output, nullptr, idx);
	}

	for (obs_encoder_t *venc : extra_encoders)
		obs_encoder_release(venc);
	extra_encoders.clear();
	extra_canvas_names.clear();
}

obs_encoder_group_t *MOQOutput::CreateEncoderGroup()
{
	if (extra_encoders.empty())
		return nullptr;

	// Attempt to coordinate the encoders startup
	obs_encoder_group_t *group = obs_encoder_group_create();
	if (!group)
		return nullptr;

	if (!obs_encoder_set_group(obs_output_get_video_encoder(output), group))
		blog(LOG_WARNING, "[obs-moq] main encoder could not join the group; "
				  "canvas start times will not be aligned");
	for (obs_encoder_t *venc : extra_encoders)
		obs_encoder_set_group(venc, group);
	return group;
}

static std::string video_track_name(size_t encoder_idx, const video_config &conf)
{
	const std::string role = encoder_idx == kMainEncoderIdx ? "video_main_"
								: "video_extra" + std::to_string(encoder_idx) + "_";
	return role + std::to_string(conf.video_width) + "x" + std::to_string(conf.video_height);
}

static void log_video_track(const MOQVideoTrack &vt)
{
	char alt_group_str[16] = "none";
	if (vt.has_alt_group)
		snprintf(alt_group_str, sizeof(alt_group_str), "%d", vt.alt_group);

	blog(LOG_INFO, "[obs-moq] video track '%s' from encoder slot %zu (%ux%u, altGroup %s, label '%s')",
	     vt.name.c_str(), vt.encoder_idx, vt.conf.video_width, vt.conf.video_height, alt_group_str,
	     vt.label.c_str());
}

bool MOQOutput::AddVideoTrack(size_t encoder_idx, bool multitrack, const std::string &label)
{
	MOQVideoTrack vt;
	vt.encoder_idx = encoder_idx;

	if (!LoadVideoEncoderSettings(vt))
		return false;

	if (multitrack) {
		vt.name = video_track_name(encoder_idx, vt.conf);
		vt.has_alt_group = true;
		vt.alt_group = (int)encoder_idx + 1;
		vt.label = label;
	} else {
		vt.name = "video";
	}

	log_video_track(vt);
	video_tracks.push_back(std::move(vt));
	return true;
}

bool MOQOutput::LoadVideoTracks()
{
	video_tracks.clear();

	const bool multitrack = !extra_encoders.empty();

	OBSCanvasAutoRelease main_canvas = obs_get_main_canvas();
	const char *main_canvas_name = main_canvas ? obs_canvas_get_name(main_canvas) : nullptr;

	if (!AddVideoTrack(kMainEncoderIdx, multitrack, main_canvas_name ? main_canvas_name : ""))
		return false;
	for (size_t i = 0; i < extra_encoders.size(); i++) {
		if (!AddVideoTrack(kFirstExtraEncoderIdx + i, multitrack, extra_canvas_names[i]))
			return false;
	}

	return !video_tracks.empty();
}

bool MOQOutput::ResolveServiceConfig()
{
	url.clear();
	stream_key.clear();

	obs_service_t *service = obs_output_get_service(output);
	if (!service) {
		obs_output_signal_stop(output, OBS_OUTPUT_ERROR);
		return false;
	}

	if (!LoadEndpointSettings(service))
		return false;

	LoadContainerSettings(service);

	const char *server = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
	if (server && *server) {
		url = server;
		blog(LOG_INFO, "[obs-moq] using URL from service: %s", url.c_str());
	} else {
		blog(LOG_WARNING, "[obs-moq] no relay URL configured in the service");
		obs_output_set_last_error(output, obs_module_text("Error.NoURL"));
		obs_output_signal_stop(output, OBS_OUTPUT_BAD_PATH);
		return false;
	}

	const char *key = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_STREAM_KEY);
	if (key && *key) {
		stream_key = key;
	} else {
		blog(LOG_WARNING, "[obs-moq] no stream key configured in the service");
		obs_output_set_last_error(output, obs_module_text("Error.NoKey"));
		obs_output_signal_stop(output, OBS_OUTPUT_BAD_PATH);
		return false;
	}

	std::string ns_error;
	if (!SplitNamespace(ns_error)) {
		blog(LOG_WARNING, "[obs-moq] invalid namespace '%s': %s", key, ns_error.c_str());
		obs_output_signal_stop(output, OBS_OUTPUT_BAD_PATH);
		return false;
	}

	return true;
}

moq_media_track_t *MOQOutput::CreateVideoTrack(MOQVideoTrack &vt, moq_media_sender_t *new_sender)
{
	moq_media_track_cfg_t tcfg;
	moq_media_track_cfg_init(&tcfg);
	tcfg.name = {(const uint8_t *)vt.name.c_str(), vt.name.size()};
	tcfg.media_type = MOQ_MEDIA_TYPE_VIDEO;
	tcfg.packaging = cmaf_enabled ? MOQ_MEDIA_PACKAGING_CMAF : MOQ_MEDIA_PACKAGING_RAW;
	tcfg.codec = {(const uint8_t *)vt.codec.c_str(), vt.codec.size()};

	if (cmaf_enabled) {
		if (!InitCMAFVideoPackager(vt))
			return nullptr;

		tcfg.init_data = moq_cmaf_packager_init_segment(vt.packager.get());
		tcfg.timescale = moq_cmaf_packager_timescale(vt.packager.get());
	} else {
		tcfg.init_data = {vt.init_data.data(), vt.init_data.size()};
		tcfg.timescale = VIDEO_TIMESCALE;
	}

	tcfg.is_live = true;
	tcfg.width = vt.conf.video_width;
	tcfg.height = vt.conf.video_height;
	tcfg.framerate_millis = vt.conf.fps_num * 1000 / vt.conf.fps_den;
	tcfg.bitrate = vt.conf.bitrate;
	tcfg.has_alt_group = vt.has_alt_group;
	tcfg.alt_group = vt.alt_group;
	if (!vt.label.empty()) {
		tcfg.has_label = true;
		tcfg.label = {(const uint8_t *)vt.label.c_str(), vt.label.size()};
	}
	tcfg.has_publisher_priority = true;
	tcfg.publisher_priority = VIDEO_PUBLISHER_PRIORITY;

	moq_media_track_t *new_track = nullptr;
	moq_result_t result = moq_media_sender_add_track(new_sender, &tcfg, &new_track);
	if (result != MOQ_OK) {
		return nullptr;
	}

	return new_track;
}

moq_media_track_t *MOQOutput::CreateVideoTrackFromPacket(MOQVideoTrack &vt, moq_media_sender_t *cur_sender,
							 struct encoder_packet *packet)
{
	obs_encoder_t *venc = obs_output_get_video_encoder2(output, vt.encoder_idx);
	const char *codec = venc ? obs_encoder_get_codec(venc) : nullptr;

	std::vector<uint8_t> init = BuildInitData(codec, packet->data, packet->size);
	if (init.empty()) {
		return nullptr;
	}

	vt.init_data = std::move(init);
	vt.codec = BuildCodecString(codec, vt.init_data);
	vt.track_codec = ResolveTrackCodec(codec);

	moq_media_track_t *new_track = CreateVideoTrack(vt, cur_sender);
	if (!new_track) {
		blog(LOG_WARNING, "[obs-moq] failed to create track '%s' from first frame", vt.name.c_str());
		vt.packager.reset();
		return nullptr;
	}

	blog(LOG_INFO, "[obs-moq] track '%s' created from first frame (codec %s, init_data %zu bytes)", vt.name.c_str(),
	     vt.codec.c_str(), vt.init_data.size());
	return new_track;
}

moq_media_track_t *MOQOutput::CreateAudioTrack(moq_media_sender_t *new_sender)
{
	moq_media_track_cfg_t tcfg;
	moq_media_track_cfg_init(&tcfg);
	tcfg.name = {(const uint8_t *)"audio", 5};
	tcfg.media_type = MOQ_MEDIA_TYPE_AUDIO;
	tcfg.packaging = cmaf_enabled ? MOQ_MEDIA_PACKAGING_CMAF : MOQ_MEDIA_PACKAGING_RAW;
	tcfg.codec = {(const uint8_t *)audio_codec.c_str(), audio_codec.size()};
	tcfg.samplerate = audio_conf.samplerate;
	tcfg.channel_config = {(const uint8_t *)audio_conf.channels.c_str(), audio_conf.channels.size()};
	tcfg.bitrate = audio_conf.bitrate;
	tcfg.has_publisher_priority = true;
	tcfg.publisher_priority = AUDIO_PUBLISHER_PRIORITY;

	if (cmaf_enabled) {
		if (!InitCMAFAudioPackager())
			return nullptr;

		tcfg.init_data = moq_cmaf_packager_init_segment(audio_packager.get());
		tcfg.timescale = moq_cmaf_packager_timescale(audio_packager.get());
	}

	moq_media_track_t *new_track = nullptr;
	moq_result_t result = moq_media_sender_add_track(new_sender, &tcfg, &new_track);
	if (result != MOQ_OK) {
		return nullptr;
	}
	return new_track;
}

void MOQOutput::OnReady(void *ctx, moq_media_sender_t *sender)
{
	MOQOutput *self = static_cast<MOQOutput *>(ctx);

	if (self->stopping.load()) {
		blog(LOG_INFO, "[obs-moq] session ready ignored: stop in progress");
		return;
	}

	self->connect_time_ms.store((int)((os_gettime_ns() - self->start_time_ns) / 1000000.0));
	obs_encoder_group_t *group = self->CreateEncoderGroup();
	if (!obs_output_begin_data_capture(self->output, 0)) {
		obs_encoder_group_destroy(group);
		blog(LOG_WARNING, "[obs-moq] obs_output_begin_data_capture failed");
		return;
	}

	obs_encoder_group_destroy(group);
	self->running.store(true);
	self->got_ready.store(true);
}

void MOQOutput::OnClosed(void *ctx, moq_media_sender_t *sender, bool is_fatal, uint64_t fatal_code)
{
	MOQOutput *self = static_cast<MOQOutput *>(ctx);
	if (self->running.exchange(false)) {
		obs_output_signal_stop(self->output, OBS_OUTPUT_DISCONNECTED);
	} else if (!self->got_ready.load() && !self->stopping.load()) {
		obs_output_set_last_error(self->output, obs_module_text("Error.Connect"));
		obs_output_signal_stop(self->output, OBS_OUTPUT_CONNECT_FAILED);
	}
}

void MOQOutput::OnTrackClosed(void *ctx, moq_media_sender_t *sender, moq_media_track_t *track)
{
	MOQOutput *self = static_cast<MOQOutput *>(ctx);
	if (self->running.exchange(false)) {
		obs_output_signal_stop(self->output, OBS_OUTPUT_DISCONNECTED);
	}
}

bool MOQOutput::Connect()
{
	moq_endpoint_cfg_t ecfg;
	moq_endpoint_cfg_init_sized(&ecfg, sizeof(ecfg));
	ecfg.url.data = (const uint8_t *)url.c_str();
	ecfg.url.len = url.size();
	ecfg.insecure_skip_verify = endpoint_conf.skip_tls_verify;
	ecfg.handshake_timeout_us = MOQ_HANDSHAKE_TIMEOUT_US;

	ecfg.versions.struct_size = sizeof(ecfg.versions);
	if (endpoint_conf.draft_version) {
		ecfg.versions.policy = MOQ_VERSION_POLICY_EXACT;
		ecfg.versions.versions = &endpoint_conf.draft_version;
		ecfg.versions.version_count = 1;
	} else {
		ecfg.versions.policy = MOQ_VERSION_POLICY_AUTO;
	}

	moq_media_sender_cfg_t scfg;
	moq_media_sender_cfg_init_live_sized(&scfg, sizeof(scfg));
	scfg.endpoint = &ecfg;
	scfg.namespace_ = namespace_val;
	scfg.publish_tracks = true;
	scfg.drop_without_demand = true;

	moq_media_sender_callbacks_init_sized(&scfg.callbacks, sizeof(scfg.callbacks));
	scfg.callbacks.ctx = this;
	scfg.callbacks.on_ready = &MOQOutput::OnReady;
	scfg.callbacks.on_closed = &MOQOutput::OnClosed;
	scfg.callbacks.on_track_closed = &MOQOutput::OnTrackClosed;

	moq_media_sender_t *media_sender = nullptr;
	moq_result_t create_result = moq_media_sender_create(&scfg, &media_sender);
	if (create_result != MOQ_OK) {
		blog(LOG_WARNING, "[obs-moq] moq_media_sender_create failed: %d", (int)create_result);
		const char *error = (create_result == MOQ_ERR_UNSUPPORTED && endpoint_conf.draft_version)
					    ? "Error.UnsupportedVersion"
					    : "Error.Connect";
		obs_output_set_last_error(output, obs_module_text(error));
		obs_output_signal_stop(output, OBS_OUTPUT_CONNECT_FAILED);
		return false;
	}

	for (MOQVideoTrack &vt : video_tracks) {
		if (vt.init_data.empty())
			continue; // created from the first keyframe instead

		vt.track = CreateVideoTrack(vt, media_sender);
		if (!vt.track) {
			blog(LOG_WARNING, "[obs-moq] failed to create video track '%s'", vt.name.c_str());
			moq_media_sender_destroy(media_sender);
			obs_output_signal_stop(output, OBS_OUTPUT_ERROR);
			return false;
		}
	}

	moq_media_track_t *new_audio_track = CreateAudioTrack(media_sender);
	if (!new_audio_track) {
		blog(LOG_WARNING, "[obs-moq] failed to create audio track");
		moq_media_sender_destroy(media_sender);
		obs_output_signal_stop(output, OBS_OUTPUT_ERROR);
		return false;
	}

	{
		std::lock_guard<std::mutex> lock(sender_mutex);
		sender = media_sender;
		audio_track = new_audio_track;
	}

	return true;
}

bool MOQOutput::Start()
{
	std::lock_guard<std::mutex> lock(start_stop_mutex);

	blog(LOG_INFO, "[obs-moq] Start() requested");

	ReleaseExtraVideo();

	const std::vector<std::string> extra_canvas_uuids = read_extra_canvas_uuids(obs_output_get_service(output));
	if (!extra_canvas_uuids.empty() && !SetupExtraVideo(extra_canvas_uuids)) {
		blog(LOG_ERROR, "[obs-moq] cannot start requested extra video");
		ReleaseExtraVideo();
		return false;
	}

	if (!obs_output_can_begin_data_capture(output, 0)) {
		blog(LOG_ERROR, "[obs-moq] cannot begin data capture");
		ReleaseExtraVideo();
		return false;
	}
	if (!obs_output_initialize_encoders(output, 0)) {
		blog(LOG_ERROR, "[obs-moq] failed to initialize encoders");
		ReleaseExtraVideo();
		return false;
	}

	running.store(false);
	got_ready.store(false);
	stopping.store(false);
	total_bytes_sent.store(0);
	connect_time_ms.store(0);
	start_time_ns = os_gettime_ns();
	audio_group_sent = 0;

	const int64_t wall_us = std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::system_clock::now().time_since_epoch())
					.count();
	epoch_offset_us = wall_us - (int64_t)(start_time_ns / 1000);

	if (start_stop_thread.joinable())
		start_stop_thread.join();

	start_stop_thread = std::thread(&MOQOutput::StartThread, this);

	return true;
}

void MOQOutput::Stop(bool signal)
{
	stopping.store(true);

	std::lock_guard<std::mutex> lock(start_stop_mutex);
	if (start_stop_thread.joinable())
		start_stop_thread.join();

	bool was_running = running.exchange(false);

	if (was_running) {
		obs_output_end_data_capture(output);
	}

	moq_media_sender_t *doomed = nullptr;
	{
		std::lock_guard<std::mutex> slock(sender_mutex);
		doomed = sender;
		sender = nullptr;
		audio_track = nullptr;
		video_tracks.clear();
	}
	if (doomed) {
		moq_media_sender_destroy(doomed);
	}

	audio_packager.reset();

	ReleaseExtraVideo();

	if (signal) {
		obs_output_signal_stop(output, OBS_OUTPUT_SUCCESS);
	}

	total_bytes_sent.store(0);
	connect_time_ms.store(0);
	start_time_ns = os_gettime_ns();
}

static bool ReframeAnnexB(const TrackCodec *video_codec, struct encoder_packet *packet, struct encoder_packet *out)
{
	if (packet->type != OBS_ENCODER_VIDEO)
		return false;

	if (video_codec == &kCodecH264)
		obs_parse_avc_packet(out, packet);
	else if (video_codec == &kCodecH265)
		obs_parse_hevc_packet(out, packet);
	else
		return false;

	if (out->size == 0 && packet->size > 0) {
		obs_encoder_packet_release(out);
		return false;
	}

	return true;
}

void MOQOutput::WriteMediaObject(moq_media_track_t *track, struct encoder_packet *packet, const uint8_t *data,
				 size_t size, bool is_sync, bool starts_group, bool ends_group)
{
	moq_rcbuf_t *payload = nullptr;
	// moq_rcbuf_create will copy the data into a new rcbuf, and increment the refcount. We will need to decref it after sending, or if we don't send it.
	if (moq_rcbuf_create(moq_alloc_default(), data, size, &payload) != MOQ_OK) {
		blog(LOG_WARNING, "[obs-moq] rcbuf alloc failed");
		return;
	}

	// OBS advances pts/dts by timebase_num per frame, so they are already in 1/timebase_den units.
	const uint64_t pts_usec = util_mul_div64((uint64_t)packet->pts, 1000000ull, (uint64_t)packet->timebase_den);
	const uint64_t dts_usec = util_mul_div64((uint64_t)packet->dts, 1000000ull, (uint64_t)packet->timebase_den);

	moq_media_send_object_t obj = {};
	obj.struct_size = sizeof(obj);
	obj.payload = payload;
	obj.properties = nullptr;
	obj.is_sync = is_sync;
	obj.starts_group = starts_group;
	obj.ends_group = ends_group;
	obj.presentation_time_us = pts_usec;
	obj.decode_time_us = dts_usec;

	const int64_t capture_us = (int64_t)packet->sys_dts_usec + epoch_offset_us;
	if (epoch_offset_us > 0 && capture_us > 0 && (uint64_t)capture_us <= MOQ_QUIC_VARINT_MAX) {
		obj.has_capture_time = true;
		obj.capture_time_us = (uint64_t)capture_us;
	}

	if (cmaf_enabled && starts_group) {
		obj.has_sap_type = true;
		obj.sap_type = MOQ_SAP_TYPE_1;
	}

	if (moq_media_sender_write(sender, track, &obj) != MOQ_OK) {
		moq_rcbuf_decref(payload);
		return;
	}

	total_bytes_sent.fetch_add(size);
}

void MOQOutput::SendVideoPacket(MOQVideoTrack &vt, struct encoder_packet *packet)
{
	if (!sender)
		return;

	if (!vt.track) {
		if (!packet->keyframe)
			return;
		// Expects annex-b, thus we don't use the reframed packet
		vt.track = CreateVideoTrackFromPacket(vt, sender, packet);
		if (!vt.track)
			return;
	}

	moq_bytes_t fragment = {packet->data, packet->size};
	struct encoder_packet reframed;
	bool did_reframe = false;
	if (cmaf_enabled) {
		if (!package_packet(vt.packager.get(), vt.track_codec, packet, vt.sample_duration, cmaf_scratch,
				    &fragment)) {
			blog(LOG_DEBUG, "[obs-moq] dropping a packet that could not be packaged");
			return;
		}
	} else if (ReframeAnnexB(vt.track_codec, packet, &reframed)) {
		did_reframe = true;
		fragment = {reframed.data, reframed.size};
	}

	WriteMediaObject(vt.track, packet, fragment.data, fragment.len, packet->keyframe, packet->keyframe, false);

	if (did_reframe)
		obs_encoder_packet_release(&reframed);
}

void MOQOutput::SendAudioPacket(struct encoder_packet *packet, bool starts_group, bool ends_group)
{
	if (!sender || !audio_track)
		return;

	moq_bytes_t fragment = {packet->data, packet->size};
	if (cmaf_enabled && !package_packet(audio_packager.get(), audio_track_codec, packet, audio_conf.frame_size,
					    cmaf_scratch, &fragment)) {
		blog(LOG_DEBUG, "[obs-moq] dropping a packet that could not be packaged");
		return;
	}

	WriteMediaObject(audio_track, packet, fragment.data, fragment.len, true, starts_group, ends_group);
}

void MOQOutput::Data(struct encoder_packet *packet)
{
	if (!packet) {
		Stop(false);
		obs_output_signal_stop(output, OBS_OUTPUT_ENCODE_ERROR);
		return;
	}

	if (!running.load()) {
		return;
	}

	std::lock_guard<std::mutex> lock(sender_mutex);

	if (packet->type == OBS_ENCODER_VIDEO) {
		for (MOQVideoTrack &vt : video_tracks) {
			if (vt.encoder_idx == packet->track_idx) {
				SendVideoPacket(vt, packet);
				break;
			}
		}
	} else if (packet->type == OBS_ENCODER_AUDIO) {
		const bool starts_group = audio_group_sent == 0;
		const bool ends_group = ++audio_group_sent == audio_group_frames;
		if (ends_group)
			audio_group_sent = 0;
		SendAudioPacket(packet, starts_group, ends_group);
	}
}

void MOQOutput::StartThread()
{
	blog(LOG_INFO, "[obs-moq] Starting");

	if (!ResolveServiceConfig()) {
		blog(LOG_WARNING, "[obs-moq] failed to resolve service config");
		return;
	}

	if (!LoadVideoTracks()) {
		blog(LOG_WARNING, "[obs-moq] failed to configure video tracks");
		return;
	}

	if (!LoadAudioEncoderSettings()) {
		blog(LOG_WARNING, "[obs-moq] failed to configure audio track");
		return;
	}

	if (!Connect()) {
		blog(LOG_WARNING, "[obs-moq] failed to connect");
		return;
	}
}

void register_moq_output()
{
	struct obs_output_info info = {};
	info.id = "moq_output";
	uint32_t flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED | OBS_OUTPUT_SERVICE | OBS_OUTPUT_MULTI_TRACK_VIDEO;
#ifdef OBS_OUTPUT_NO_INTERLEAVE
	flags |= OBS_OUTPUT_NO_INTERLEAVE;
#else
	blog(LOG_INFO, "[obs-moq] libobs does not have OBS_OUTPUT_NO_INTERLEAVE; the interleaver will remain active");
#endif
	info.flags = flags;
	info.protocols = "MOQ";
	info.encoded_video_codecs = "h264;hevc;av1";
	info.encoded_audio_codecs = "aac;opus";

	info.get_name = [](void *) -> const char * {
		return obs_module_text("Output.Name");
	};
	info.create = [](obs_data_t *settings, obs_output_t *output) -> void * {
		return new MOQOutput(settings, output);
	};
	info.destroy = [](void *priv_data) {
		delete static_cast<MOQOutput *>(priv_data);
	};
	info.start = [](void *priv_data) -> bool {
		return static_cast<MOQOutput *>(priv_data)->Start();
	};
	info.stop = [](void *priv_data, uint64_t) {
		static_cast<MOQOutput *>(priv_data)->Stop(true);
	};
	info.encoded_packet = [](void *priv_data, struct encoder_packet *packet) {
		static_cast<MOQOutput *>(priv_data)->Data(packet);
	};

	obs_register_output(&info);
	blog(LOG_INFO, "[obs-moq] registered output '%s' (protocol MOQ)", info.id);
}