#pragma once
#include "models.hpp"
#include <functional>

namespace tl {
struct HttpResponse { int status = 0; std::string body, error; };
using HttpGet = std::function<HttpResponse(const std::string&, double, const Cancel&)>;
std::vector<TimedLine> parse_lrc(const std::string& text);
int active_line(const std::vector<TimedLine>& lyrics, double position);
std::vector<std::pair<std::string, std::string>> search_candidates(const Track& track);
LyricsResult fetch_lyrics(const Track& track, const Cancel& cancel = {}, HttpGet get = {});
}
