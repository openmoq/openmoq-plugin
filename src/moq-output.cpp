#include "moq-output.h"
#include "codec-signaling.h"
#include "moq-service.h"

#include <util/platform.h>
#include <obs.hpp>
#include <obs-avc.h>
#include <obs-hevc.h>

#define VIDEO_TIMESCALE 1000000u
#define MOQ_HANDSHAKE_TIMEOUT_US 5000000ull


#ifndef OBS_OUTPUT_MULTI_TRACK_VIDEO
#define OBS_OUTPUT_MULTI_TRACK_VIDEO (1 << 6)
#endif

static constexpr size_t kMainEncoderIdx = 0;
static constexpr size_t kExtraEncoderIdx = 1;

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

void MOQOutput::SplitNamespace()
{
	ns_bytes.clear();
	const char *base = stream_key.data();
	size_t start = 0;
	for (size_t i = 0; i <= stream_key.size(); ++i) {
		bool sep = (i == stream_key.size()) || stream_key[i] == '-';
		if (sep) {
			if (i > start)
				ns_bytes.push_back({(const uint8_t *)(base + start), i - start});
			start = i + 1;
		}
	}
	namespace_val.parts = ns_bytes.data();
	namespace_val.count = ns_bytes.size();
}

bool MOQOutput::LoadVideoEncoderSettings(MOQVideoTrack &vt)
{
	obs_encoder_t *venc = obs_output_get_video_encoder2(output, vt.encoder_idx);
	if (!venc) {
		blog(LOG_WARNING, "[obs-moq] no video encoder assigned at index %zu", vt.encoder_idx);
		obs_output_set_last_error(output, obs_module_text("Error.NoEncoder"));
		return false;
	}

	OBSDataAutoRelease settings = obs_encoder_get_settings(venc);
	vt.conf.bitrate = (uint64_t)obs_data_get_int(settings, "bitrate") * 1000;

	const char *codec = obs_encoder_get_codec(venc);
	vt.conf.video_width = obs_encoder_get_width(venc);
	vt.conf.video_height = obs_encoder_get_height(venc);

	struct obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	vt.conf.fps_num = ovi.fps_num;
	vt.conf.fps_den = ovi.fps_den;

	//initialize init_data
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
	audio_conf.samplerate = audio_output_get_sample_rate(audio);
	audio_conf.channels = std::to_string(audio_output_get_channels(audio));

	const char *codec = obs_encoder_get_codec(aenc);
	//todo: add codec validation here
	uint8_t *extra = nullptr;
	size_t extra_size = 0;
	obs_encoder_get_extra_data(aenc, &extra, &extra_size);
	audio_init_data = BuildInitData(codec, extra, extra_size);
	audio_codec = BuildCodecString(codec, audio_init_data);

	return true;
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

static std::string read_extra_canvas_uuid(obs_service_t *service)
{
	if (!service)
		return {};

	OBSDataAutoRelease settings = obs_service_get_settings(service);
	if (!settings)
		return {};

	const char *uuid = obs_data_get_string(settings, kSettingExtraCanvas);
	return uuid ? uuid : "";
}

bool MOQOutput::SetupExtraVideo(const char *canvas_uuid)
{
	if (!extra_canvas_resolve(canvas_uuid, &extra_conf)) {
		blog(LOG_WARNING, "[obs-moq] the configured extra canvas could not be resolved");
		obs_output_set_last_error(output, obs_module_text("Error.NoExtraCanvas"));
		extra_conf = {};
		return false;
	}

	obs_encoder_t *main_encoder = obs_output_get_video_encoder(output);
	if (!main_encoder) {
		blog(LOG_WARNING, "[obs-moq] no video encoder assigned");
		obs_output_set_last_error(output, obs_module_text("Error.NoEncoder"));
		extra_conf = {};
		return false;
	}

	const char *enc_id = obs_encoder_get_id(main_encoder);
	OBSDataAutoRelease settings = obs_encoder_get_settings(main_encoder);

	obs_encoder_t *venc = obs_video_encoder_create(enc_id, "moq_extra_video", settings, nullptr);
	if (!venc) {
		blog(LOG_WARNING, "[obs-moq] failed to create the extra video encoder '%s'", enc_id);
		obs_output_set_last_error(output, obs_module_text("Error.NoEncoder"));
		extra_conf = {};
		return false;
	}

	obs_encoder_set_video(venc, extra_conf.video);
	// Attempt to coordinate the two encoders startup
	encoder_group = obs_encoder_group_create();
	if (encoder_group) {
		if (!obs_encoder_set_group(main_encoder, encoder_group))
			blog(LOG_WARNING, "[obs-moq] main encoder could not join the group; "
					  "canvas start times will not be aligned");
		obs_encoder_set_group(venc, encoder_group);
	}

	extra_encoder = venc;
	obs_output_set_video_encoder2(output, venc, kExtraEncoderIdx);

	blog(LOG_INFO, "[obs-moq] extra canvas %ux%u @ %.3f fps attached to encoder slot %zu (cloned '%s')",
	     extra_conf.width, extra_conf.height, (double)extra_conf.fps_num / (double)extra_conf.fps_den,
	     kExtraEncoderIdx, enc_id);
	return true;
}

void MOQOutput::ReleaseExtraVideo()
{
	extra_conf = {};

	if (!obs_output_active(output))
		obs_output_set_video_encoder2(output, nullptr, kExtraEncoderIdx);

	if (extra_encoder) {
		obs_encoder_release(extra_encoder);
		extra_encoder = nullptr;
	}

	if (encoder_group) {
		obs_encoder_group_destroy(encoder_group);
		encoder_group = nullptr;
	}
}

bool MOQOutput::LoadVideoTracks()
{
	video_tracks.clear();

	const bool multitrack = extra_conf.video != nullptr;

	auto add_track = [&](size_t encoder_idx, int alt_group) -> bool {
		MOQVideoTrack vt;
		vt.encoder_idx = encoder_idx;

		if (!LoadVideoEncoderSettings(vt))
			return false;

		if (multitrack) {
			const char *role = encoder_idx == kExtraEncoderIdx ? "video_extra_" : "video_main_";
			vt.name = role + std::to_string(vt.conf.video_width) + "x" +
				  std::to_string(vt.conf.video_height);
			vt.has_alt_group = true;
			vt.alt_group = alt_group;
		} else {
			vt.name = "video";
		}

		char alt_group_str[16] = "none";
		if (vt.has_alt_group)
			snprintf(alt_group_str, sizeof(alt_group_str), "%d", vt.alt_group);

		blog(LOG_INFO, "[obs-moq] video track '%s' from encoder slot %zu (%ux%u, altGroup %s)",
		     vt.name.c_str(), vt.encoder_idx, vt.conf.video_width, vt.conf.video_height, alt_group_str);

		video_tracks.push_back(std::move(vt));
		return true;
	};

	if (!add_track(kMainEncoderIdx, 1))
		return false;
	if (multitrack && !add_track(kExtraEncoderIdx, 2))
		return false;

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

	SplitNamespace();

	return true;
}

moq_media_track_t *MOQOutput::CreateVideoTrack(MOQVideoTrack &vt, moq_media_sender_t *new_sender)
{
	moq_media_track_cfg_t tcfg;
	moq_media_track_cfg_init(&tcfg);
	tcfg.name = {(const uint8_t *)vt.name.c_str(), vt.name.size()};
	tcfg.media_type = MOQ_MEDIA_TYPE_VIDEO;
	// todo: make this configurable and add CMAF support
	tcfg.packaging = MOQ_MEDIA_PACKAGING_RAW;
	tcfg.codec = {(const uint8_t *)vt.codec.c_str(), vt.codec.size()};
	tcfg.timescale = VIDEO_TIMESCALE;
	// todo: analyze actual need for this and how it fits w/other codecs
	tcfg.init_data = {vt.init_data.data(), vt.init_data.size()};
	tcfg.is_live = true;
	tcfg.width = vt.conf.video_width;
	tcfg.height = vt.conf.video_height;
	tcfg.framerate_millis = vt.conf.fps_num * 1000 / vt.conf.fps_den;
	tcfg.bitrate = vt.conf.bitrate;
	tcfg.has_alt_group = vt.has_alt_group;
	tcfg.alt_group = vt.alt_group;

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
		return nullptr;
	}

	blog(LOG_INFO, "[obs-moq] track '%s' created from first frame (codec %s, init_data %zu bytes)",
	     vt.name.c_str(), vt.codec.c_str(), vt.init_data.size());
	return new_track;
}

moq_media_track_t *MOQOutput::CreateAudioTrack(moq_media_sender_t *new_sender)
{
	moq_media_track_cfg_t tcfg;
	moq_media_track_cfg_init(&tcfg);
	tcfg.name = {(const uint8_t *)"audio", 5};
	tcfg.media_type = MOQ_MEDIA_TYPE_AUDIO;
	tcfg.packaging = MOQ_MEDIA_PACKAGING_RAW;
	tcfg.codec = {(const uint8_t *)audio_codec.c_str(), audio_codec.size()};
	tcfg.samplerate = audio_conf.samplerate;
	tcfg.channel_config = {(const uint8_t *)audio_conf.channels.c_str(), audio_conf.channels.size()};
	tcfg.bitrate = audio_conf.bitrate;
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
	if (!obs_output_begin_data_capture(self->output, 0)) {
		blog(LOG_WARNING, "[obs-moq] obs_output_begin_data_capture failed");
		return;
	}
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

	// Every video track and the shared audio track go into this one catalog, so a
	// subscriber sees one broadcast and picks a track within it.
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

	const std::string extra_canvas_uuid = read_extra_canvas_uuid(obs_output_get_service(output));
	if (!extra_canvas_uuid.empty() && !SetupExtraVideo(extra_canvas_uuid.c_str())) {
		blog(LOG_ERROR, "[obs-moq] cannot start requested extra video");
		return false;
	}

	if (!obs_output_can_begin_data_capture(output, 0)) {
		blog(LOG_ERROR, "[obs-moq] cannot begin data capture");
		return false;
	}
	if (!obs_output_initialize_encoders(output, 0)) {
		blog(LOG_ERROR, "[obs-moq] failed to initialize encoders");
		return false;
	}

	running.store(false);
	got_ready.store(false);
	stopping.store(false);
	total_bytes_sent.store(0);
	connect_time_ms.store(0);
	start_time_ns = os_gettime_ns();

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
	if (moq_rcbuf_create(moq_alloc_default(), data, size, &payload) != MOQ_OK) {
		blog(LOG_WARNING, "[obs-moq] rcbuf alloc failed");
		return;
	}

	// The encoder's own pts/dts, converted to microseconds with its timebase.
	const uint64_t pts_usec = util_mul_div64((uint64_t)packet->pts, 1000000ull * (uint64_t)packet->timebase_num,
						 (uint64_t)packet->timebase_den);
	const uint64_t dts_usec = util_mul_div64((uint64_t)packet->dts, 1000000ull * (uint64_t)packet->timebase_num,
						 (uint64_t)packet->timebase_den);

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
		vt.track = CreateVideoTrackFromPacket(vt, sender, packet);
		if (!vt.track)
			return;
	}

	struct encoder_packet reframed;
	bool did_reframe = ReframeAnnexB(vt.track_codec, packet, &reframed);
	const uint8_t *payload_data = did_reframe ? reframed.data : packet->data;
	size_t payload_size = did_reframe ? reframed.size : packet->size;

	WriteMediaObject(vt.track, packet, payload_data, payload_size, packet->keyframe, packet->keyframe, false);

	if (did_reframe)
		obs_encoder_packet_release(&reframed);
}

void MOQOutput::SendAudioPacket(struct encoder_packet *packet)
{
	if (!sender || !audio_track)
		return;

	WriteMediaObject(audio_track, packet, packet->data, packet->size, true, true, true);
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
		SendAudioPacket(packet);
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