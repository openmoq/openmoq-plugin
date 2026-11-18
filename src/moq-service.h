#pragma once
#include <obs-module.h>
#include <string>
#include <vector>

inline constexpr const char *kSettingSkipTlsVerify = "skip_tls_verify";
inline constexpr const char *kSettingDraftVersion = "draft_version";
inline constexpr const char *kSettingContainer = "container";
inline constexpr const char *kSettingExtraCanvases = "extra_canvases";
inline constexpr const char *kSettingExtraCanvasPrefix = "extra_canvas_";

inline constexpr const char *kContainerLOC = "loc";
inline constexpr const char *kContainerCMAF = "cmaf";

inline std::string ExtraCanvasSettingKey(const std::string &uuid)
{
	return kSettingExtraCanvasPrefix + uuid;
}

std::vector<std::string> UnavailableExtraCanvases(obs_data_t *settings);

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