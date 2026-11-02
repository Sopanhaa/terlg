#include "terminal_lyrics/playback_clock.hpp"

#include <algorithm>
#include <cmath>

namespace tl {
void PlaybackClock::update(const Track& track, std::optional<double> sample,
                           const std::string& status, double now) {
    const auto field = [](const std::string& value) { return std::to_string(value.size()) + ":" + value; };
    const std::string identity = field(track.player) + field(track.track_id) + track.key();
    const auto previous = identity == identity_ ? position(now) : std::nullopt;
    const bool valid = sample && std::isfinite(*sample) && *sample >= 0;
    position_ = valid ? sample : previous;
    identity_ = identity;
    sampled_at_ = now;
    playing_ = lowercase(status) == "playing";
    duration_ = std::isfinite(track.duration) && track.duration > 0 ? track.duration : 0.0;
}

std::optional<double> PlaybackClock::position(double now) const {
    if (!position_) return std::nullopt;
    const double elapsed = playing_ ? std::max(0.0, now - sampled_at_) : 0.0;
    const double value = *position_ + elapsed;
    return duration_ > 0 ? std::min(value, duration_) : value;
}
}
