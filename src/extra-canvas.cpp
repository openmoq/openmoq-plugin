#include "extra-canvas.h"

static bool collect_selectable_canvas(void *param, obs_canvas_t *canvas)
{
	auto *entries = (std::vector<extra_canvas_entry> *)param;

	const uint32_t flags = obs_canvas_get_flags(canvas);
	if (flags & MAIN)
		return true; // skip the main canvas (it's not "extra")
	if (flags & EPHEMERAL)
		return true; // skip previews 
	if (!obs_canvas_has_video(canvas))
		return true;

	const char *name = obs_canvas_get_name(canvas);
	const char *uuid = obs_canvas_get_uuid(canvas);
	if (name && uuid)
		entries->push_back({name, uuid});

	return true;
}

std::vector<extra_canvas_entry> extra_canvas_list()
{
	std::vector<extra_canvas_entry> entries;
	obs_enum_canvases(collect_selectable_canvas, &entries);
	return entries;
}

bool extra_canvas_resolve(const char *uuid, extra_canvas_info *info)
{
	if (!info || !uuid || !*uuid)
		return false;

	*info = {};

	obs_canvas_t *canvas = obs_get_canvas_by_uuid(uuid);
	if (!canvas) {
		blog(LOG_WARNING, "[obs-moq] extra canvas %s is not registered", uuid);
		return false;
	}

	video_t *video = obs_canvas_get_video(canvas);
	struct obs_video_info ovi = {};
	obs_canvas_get_video_info(canvas, &ovi);
	obs_canvas_release(canvas);

	if (!video) {
		blog(LOG_WARNING, "[obs-moq] extra canvas %s has no video mix", uuid);
		return false;
	}

	info->video = video;
	info->width = video_output_get_width(video);
	info->height = video_output_get_height(video);
	info->fps_num = ovi.fps_num;
	info->fps_den = ovi.fps_den;

	return info->width && info->height && info->fps_num && info->fps_den;
}
