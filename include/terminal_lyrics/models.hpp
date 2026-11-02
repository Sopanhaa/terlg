#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace tl {
using Cancel = std::shared_ptr<std::atomic_bool>;
using TimedLine = std::pair<double, std::string>;
struct Track {
    std::string title, artist, album, player;
    double duration = 0.0;
    std::string track_id;
    std::string key() const;
    std::string lyrics_key() const;
};
struct LyricsResult {
    std::vector<TimedLine> synced;
    std::vector<std::string> plain;
    std::string message;
    bool has_lyrics() const { return !synced.empty() || !plain.empty(); }
};
double monotonic_seconds();
std::string trim(const std::string& text);
std::string lowercase(const std::string& text);
}
