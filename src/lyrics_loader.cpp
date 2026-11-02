#include "terminal_lyrics/lyrics_loader.hpp"
#include <condition_variable>
#include <list>
#include <mutex>
#include <unordered_map>

namespace tl {
struct LyricsLoader::State {
    explicit State(std::size_t size, Fetch function) : cache_size(size), fetch(std::move(function)) {}
    std::mutex mutex;
    std::condition_variable condition;
    std::size_t cache_size;
    Fetch fetch;
    bool closed = false;
    std::optional<Track> pending;
    Cancel active;
    std::optional<std::pair<std::string, LyricsResult>> result;
    std::list<std::string> order;
    struct CacheItem { LyricsResult result; std::list<std::string>::iterator order; };
    std::unordered_map<std::string, CacheItem> cache;
};

LyricsLoader::LyricsLoader(std::size_t cache_size, Fetch fetch) {
    if (!fetch) fetch = [](const Track& track, const Cancel& cancel) { return fetch_lyrics(track, cancel); };
    state_ = std::make_shared<State>(cache_size, std::move(fetch));
    worker_ = std::thread([state = state_] {
        while (true) {
            Track track;
            Cancel cancel;
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                state->condition.wait(lock, [&] { return state->closed || state->pending.has_value(); });
                if (state->closed) return;
                track = std::move(*state->pending);
                state->pending.reset();
                cancel = state->active;
            }
            LyricsResult result;
            try { result = state->fetch(track, cancel); }
            catch (const std::exception& error) { result.message = std::string("Could not load lyrics: ") + error.what(); }
            catch (...) { result.message = "Could not load lyrics."; }
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->closed) return;
            if (cancel->load()) continue;
            const auto key = track.lyrics_key();
            if (result.has_lyrics() && state->cache_size > 0) {
                auto existing = state->cache.find(key);
                if (existing != state->cache.end()) state->order.erase(existing->second.order);
                state->order.push_back(key);
                state->cache[key] = {result, std::prev(state->order.end())};
                while (state->cache.size() > state->cache_size) {
                    state->cache.erase(state->order.front());
                    state->order.pop_front();
                }
            }
            state->result = std::make_pair(key, std::move(result));
        }
    });
}

LyricsLoader::~LyricsLoader() { close(); }

void LyricsLoader::request(const Track& track) {
    const auto key = track.lyrics_key();
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->closed) return;
    if (state_->active) state_->active->store(true);
    state_->pending.reset();
    state_->result.reset();
    const auto cached = state_->cache.find(key);
    if (cached != state_->cache.end()) {
        state_->order.splice(state_->order.end(), state_->order, cached->second.order);
        state_->result = std::make_pair(key, cached->second.result);
        return;
    }
    state_->active = std::make_shared<std::atomic_bool>(false);
    state_->pending = track;
    state_->condition.notify_one();
}

std::optional<LyricsResult> LyricsLoader::poll(const Track& track) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->result || state_->result->first != track.lyrics_key()) return std::nullopt;
    auto result = std::move(state_->result->second);
    state_->result.reset();
    return result;
}

void LyricsLoader::close() {
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->closed = true;
        state_->pending.reset();
        if (state_->active) state_->active->store(true);
    }
    state_->condition.notify_one();
    // HTTP and retry waits observe cancellation; joining reaps the child process
    // and finishes the worker before any application state is destroyed.
    if (worker_.joinable()) worker_.join();
}
}
