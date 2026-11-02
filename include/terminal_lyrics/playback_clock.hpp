#pragma once
#include "models.hpp"
#include <optional>

namespace tl {
class PlaybackClock {
public:
    void update(const Track& track, std::optional<double> position, const std::string& status,
                double now = monotonic_seconds());
    std::optional<double> position(double now = monotonic_seconds()) const;
private:
    std::string identity_;
    std::optional<double> position_;
    double sampled_at_ = 0, duration_ = 0;
    bool playing_ = false;
};
}
