#pragma once
#include "models.hpp"
#include <optional>

namespace tl {
struct PlayerSample {
    Track track;
    std::optional<double> position;
    std::string status;
};
std::string run_playerctl(const std::vector<std::string>& args, const std::string& player = "");
PlayerSample current_track(const std::string& player = "");
PlayerSample parse_metadata(const std::string& metadata, const std::string& player = "");
}
