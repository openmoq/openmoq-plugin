#pragma once
#include <obs-module.h>

#include <string>
#include <vector>

struct extra_canvas_info {
	std::string name;
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

bool extra_canvas_resolve(const char *uuid, extra_canvas_info *info);
