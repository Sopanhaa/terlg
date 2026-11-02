#include "terminal_lyrics/lyrics_loader.hpp"
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
tl::Track track(const std::string& title, double duration = 180) {
    return {title, "Artist", "", "player", duration, ""};
}
tl::LyricsResult wait_result(tl::LyricsLoader& loader, const tl::Track& current) {
    const double deadline = tl::monotonic_seconds() + 2;
    while (tl::monotonic_seconds() < deadline) {
        if (auto result = loader.poll(current)) return *result;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    throw std::runtime_error("background result was not delivered");
}
void wait_flag(const std::atomic_bool& flag) {
    const double deadline = tl::monotonic_seconds() + 2;
    while (!flag.load() && tl::monotonic_seconds() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    check(flag.load(), "worker did not start");
}
}

int main() {
    try {
        std::atomic_int calls{0};
        tl::LyricsLoader cached(2, [&](const tl::Track& current, const tl::Cancel&) {
            ++calls;
            return tl::LyricsResult{{}, {current.title}, ""};
        });
        for (const auto& title : {"First", "Second", "First", "Third", "Second"}) {
            auto current = track(title);
            cached.request(current);
            check(wait_result(cached, current).plain.front() == title, "cache returned another song");
        }
        check(calls == 4, "least recently used cache eviction failed");
        auto other_player = track("SECOND");
        other_player.player = "other";
        cached.request(other_player);
        check(wait_result(cached, other_player).has_lyrics() && calls == 4,
              "same song should be reused across players/case changes");
        const auto extended = track("Second", 240);
        cached.request(extended);
        wait_result(cached, extended);
        check(calls == 5, "recordings of different durations shared a cache entry");
        cached.close();
        cached.close();

        std::atomic_int attempts{0};
        tl::LyricsLoader transient(64, [&](const tl::Track&, const tl::Cancel&) {
            if (++attempts == 1) throw std::runtime_error("temporary failure");
            return tl::LyricsResult{{{1, "Loaded"}}, {}, ""};
        });
        auto current = track("Song");
        transient.request(current);
        check(wait_result(transient, current).message.find("temporary failure") != std::string::npos,
              "worker exception was not converted into a result");
        transient.request(current);
        check(wait_result(transient, current).has_lyrics() && attempts == 2, "transient errors must not be cached");
        transient.close();

        std::mutex mutex;
        std::condition_variable condition;
        bool released = false;
        std::atomic_bool entered{false}, first_cancelled{false};
        std::atomic_int sequence_calls{0};
        tl::LyricsLoader superseded(0, [&](const tl::Track& song, const tl::Cancel& cancel) {
            ++sequence_calls;
            if (song.title == "First") {
                entered = true;
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait_for(lock, std::chrono::seconds(2), [&] { return released; });
                first_cancelled = cancel->load();
            }
            return tl::LyricsResult{{}, {song.title}, ""};
        });
        superseded.request(track("First"));
        wait_flag(entered);
        check(!superseded.poll(track("First")), "poll blocked or fabricated a result");
        superseded.request(track("Skipped"));
        superseded.request(track("Latest"));
        {
            std::lock_guard<std::mutex> lock(mutex);
            released = true;
        }
        condition.notify_one();
        check(wait_result(superseded, track("Latest")).plain.front() == "Latest", "stale result replaced the latest track");
        check(first_cancelled && sequence_calls == 2, "obsolete work was not cancelled/coalesced");
        check(!superseded.poll(track("First")), "stale track result was published");
        superseded.close();

        entered = false;
        std::atomic_bool stopped{false};
        tl::LyricsLoader closing(0, [&](const tl::Track&, const tl::Cancel& cancel) {
            entered = true;
            while (!cancel->load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            stopped = true;
            return tl::LyricsResult{};
        });
        closing.request(track("Song"));
        wait_flag(entered);
        const double start = tl::monotonic_seconds();
        closing.close();
        check(stopped && tl::monotonic_seconds() - start < 0.25, "closing failed to cancel and reap the worker promptly");
        std::cout << "Loader tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Loader test failed: " << error.what() << '\n';
        return 1;
    }
}
