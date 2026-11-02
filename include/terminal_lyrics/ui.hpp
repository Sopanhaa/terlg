#pragma once
#include "models.hpp"

namespace tl {
struct Options { std::string player; double refresh = 0.5, offset = 0.0; };
int live(const Options& options);
int display_width(const std::string& text);
std::string clip_text(const std::string& text, int width, bool ellipsis = false);
std::vector<std::string> wrap_text(const std::string& text, int width);
std::string playback_command(int key);
}
