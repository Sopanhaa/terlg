#include "terminal_lyrics/player.hpp"
#include "terminal_lyrics/process.hpp"

#include <cmath>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace tl {
namespace {
constexpr char separator = '\x1f';

std::optional<double> seconds(const std::string& text) {
    if (text.empty()) return std::nullopt;
    std::istringstream input(text);
    input.imbue(std::locale::classic());
    double value = 0;
    input >> std::noskipws >> value;
    if (!input || !input.eof() || !std::isfinite(value) || value < 0) return std::nullopt;
    return value / 1'000'000.0;
}
}

std::string run_playerctl(const std::vector<std::string>& args, const std::string& player) {
    std::vector<std::string> command{"playerctl"};
    if (!player.empty()) {
        command.push_back("--player");
        command.push_back(player);
    }
    command.insert(command.end(), args.begin(), args.end());
    auto result = run_process(command, 2.0);
    if (result.timed_out) throw std::runtime_error("The music player did not respond within 2 seconds.");
    if (result.exit_code != 0) {
        auto message = trim(result.error);
        throw std::runtime_error(message.empty() ? "No active music player found." : message);
    }
    while (!result.output.empty() && (result.output.back() == '\r' || result.output.back() == '\n')) {
        result.output.pop_back();
    }
    return result.output;
}

PlayerSample parse_metadata(const std::string& metadata, const std::string& player) {
    std::vector<std::string> fields;
    std::size_t begin = 0;
    for (;;) {
        const auto end = metadata.find(separator, begin);
        fields.push_back(trim(metadata.substr(begin, end == std::string::npos ? end : end - begin)));
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    if (fields.front().empty()) throw std::runtime_error("No song is currently playing.");
    if (fields.size() != 8) throw std::runtime_error("Could not read the player's song metadata.");
    return {{fields[0], fields[1], fields[2], fields[3].empty() ? player : fields[3],
             seconds(fields[6]).value_or(0.0), fields[7]}, seconds(fields[5]), fields[4]};
}

PlayerSample current_track(const std::string& player) {
    const std::vector<std::string> fields{"title", "artist", "album", "playerName", "status",
                                          "position", "mpris:length", "mpris:trackid"};
    std::string format;
    for (const auto& field : fields) {
        if (!format.empty()) format.push_back(separator);
        format += "{{" + field + "}}";
    }
    return parse_metadata(run_playerctl({"metadata", "--format", format}, player), player);
}
}
