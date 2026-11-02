#pragma once
#include "lyrics_service.hpp"
#include <optional>
#include <thread>

namespace tl {
class LyricsLoader {
public:
    using Fetch = std::function<LyricsResult(const Track&, const Cancel&)>;
    explicit LyricsLoader(std::size_t cache_size = 64, Fetch fetch = {});
    ~LyricsLoader();
    LyricsLoader(const LyricsLoader&) = delete;
    LyricsLoader& operator=(const LyricsLoader&) = delete;
    void request(const Track& track);
    std::optional<LyricsResult> poll(const Track& track);
    void close();
private:
    struct State;
    std::shared_ptr<State> state_;
    std::thread worker_;
};
}
