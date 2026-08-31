#pragma once
#include <obs-module.h>
#include <string>

inline constexpr const char *kSettingSkipTlsVerify = "skip_tls_verify";
inline constexpr const char *kSettingDraftVersion = "draft_version";

class MOQService {
public:
	std::string server;
	std::string moq_namespace;

	MOQService(obs_data_t *settings, obs_service_t *service);

	void Update(obs_data_t *settings);
	static void Defaults(obs_data_t *settings);
	static void ApplyEncoderSettings(obs_data_t *video_settings, obs_data_t *audio_settings);
	static bool Initialize(obs_output_t *output);
	static obs_properties_t *Properties();
	const char *GetConnectInfo(enum obs_service_connect_info info);
	bool CanTryToConnect();
};

void register_moq_service();