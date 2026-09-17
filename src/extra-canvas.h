#pragma once
#include <obs-module.h>

#include <string>
#include <vector>

// A second OBS canvas, published alongside the main one as its own video track.
struct extra_canvas_info {
	video_t *video = nullptr;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t fps_num = 0;
	uint32_t fps_den = 1;
};

struct extra_canvas_entry {
	std::string name;
	std::string uuid;
};

std::vector<extra_canvas_entry> extra_canvas_list();

// Resolves a canvas UUID to its video mix. False if the canvas is no longer registered
bool extra_canvas_resolve(const char *uuid, extra_canvas_info *info);
