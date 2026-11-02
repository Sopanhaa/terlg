#include "terminal_lyrics/models.hpp"
#include "terminal_lyrics/player.hpp"
#include "terminal_lyrics/playback_clock.hpp"

#include <clocale>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
void near(std::optional<double> value, double expected, const std::string& message) {
    require(value && std::abs(*value - expected) < 1e-8, message);
}
std::string metadata(std::vector<std::string> fields = {
    "Song", "Artist", "Album", "brave", "Playing", "12345678", "240000000", "/track/1"}) {
    std::string result;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i) result += '\x1f';
        result += fields[i];
    }
    return result;
}
void throws(const std::function<void()>& operation, const std::string& message) {
    try { operation(); } catch (const std::runtime_error& error) {
        require(std::string(error.what()).find(message) != std::string::npos, "Incorrect metadata error");
        return;
    }
    throw std::runtime_error("Expected metadata error: " + message);
}
}

int main() {
    std::setlocale(LC_ALL, "");
    try {
        const auto sample = tl::parse_metadata(metadata());
        require(sample.track.title == "Song" && sample.track.artist == "Artist" &&
                sample.track.album == "Album" && sample.track.player == "brave" &&
                sample.track.track_id == "/track/1", "Snapshot track fields");
        require(sample.track.duration == 240.0 && sample.status == "Playing", "Snapshot duration/status");
        near(sample.position, 12.345678, "Positions use microseconds");

        const auto empty = tl::parse_metadata(metadata({"Song", "", "", "", "Paused", "", "", ""}), "vlc");
        require(empty.track.artist.empty() && empty.track.album.empty() && empty.track.track_id.empty(),
                "Empty metadata fields stay empty");
        require(empty.track.player == "vlc" && empty.track.duration == 0.0 && !empty.position,
                "Explicit player fallback and unknown timing");
        near(tl::parse_metadata(metadata({"Song", "Artist", "", "", "Playing", "0", "0", ""})).position,
             0, "Zero is a known position");
        for (const auto& invalid : {"", "unknown", "nan", "inf", "-inf", "-1000000", "1oops", "0x10"}) {
            const auto bad = tl::parse_metadata(metadata({"Song", "Artist", "", "", "Playing", invalid, invalid, ""}));
            require(!bad.position && bad.track.duration == 0.0, "Invalid timing: " + std::string(invalid));
        }
        require(tl::parse_metadata(metadata({"First\nSecond", "Artist", "", "brave", "Playing", "1e6", "2.5e6", ""})).track.title ==
                "First\nSecond", "Multiline titles preserve fields");
        near(tl::parse_metadata(metadata({"Song", "Artist", "", "", "Playing", "1e6", "", ""})).position,
             1, "Scientific numeric metadata");
        throws([] { tl::parse_metadata(metadata({"", "Artist", "", "", "Playing", "0", "0", ""})); }, "No song");
        throws([] { tl::parse_metadata("Song\x1f" "Artist"); }, "metadata");
        throws([] { tl::parse_metadata(metadata() + "\x1f" "extra"); }, "metadata");

        tl::Track first{"Song", "Artist", "Album", "brave", 200, "/track/1"};
        tl::Track second{"SONG", "ARTIST", "ALBUM", "vlc", 201, "/track/2"};
        require(first.key() == second.key(), "Track identity ignores case, player and timing");
        require(first.lyrics_key() != second.lyrics_key(), "Cache separates recording duration");
        second.duration = 200.49;
        require(first.lyrics_key() == second.lyrics_key(), "Cache rounds durations");
        second.duration = 200.5;
        require(first.lyrics_key() == second.lyrics_key(), "Duration ties round to even");
        first.duration = 202;
        second.duration = 201.5;
        require(first.lyrics_key() == second.lyrics_key(), "Odd duration ties round up");
        first.duration = 0;
        for (const auto duration : {-1.0, std::numeric_limits<double>::infinity(),
                                   std::numeric_limits<double>::quiet_NaN()}) {
            second.duration = duration;
            require(first.lyrics_key() == second.lyrics_key(), "Invalid durations use zero cache key");
        }
        require(tl::Track{"a:b", "c", "", "", 0, ""}.key() !=
                tl::Track{"a", "b:c", "", "", 0, ""}.key(), "Track key avoids field collisions");
        require(tl::trim(" \t First\nSecond \r\n") == "First\nSecond", "Trim preserves internal newlines");
        if (MB_CUR_MAX > 1) {
            require(tl::lowercase("CAFÉ Straße") == "café strasse", "UTF-8 artist case folding");
            require(tl::trim("\u3000Song\u00a0") == "Song", "Unicode whitespace trimming");
        }

        const tl::Track track{"Song", "Artist", "", "brave", 0, "/track/1"};
        tl::PlaybackClock clock;
        clock.update(track, 10, "Playing", 100);
        near(clock.position(102.75), 12.75, "Playing clock interpolation");
        near(clock.position(99), 10, "Clock does not run backwards before anchor");
        clock.update(track, std::nullopt, "Playing", 105);
        near(clock.position(107), 17, "Missing samples preserve timeline");
        clock.update(track, std::nullopt, "Paused", 107);
        near(clock.position(110), 17, "Pause preserves interpolation and freezes clock");
        clock.update(track, std::nullopt, "Playing", 110);
        near(clock.position(112), 19, "Resume preserves missing position");
        for (const auto& status : {"Paused", "Stopped"}) {
            clock.update(track, 15, status, 100);
            near(clock.position(120), 15, "Inactive playback freezes clock");
        }
        clock.update(track, 30, "Playing", 100);
        clock.update(track, 2, "Playing", 105);
        near(clock.position(106), 3, "Backward seek is authoritative");
        clock.update(track, 60, "Playing", 107);
        near(clock.position(108), 61, "Forward seek is authoritative");
        clock.update(track, 10, "Playing", 110);
        clock.update(track, 10, "Playing", 111);
        near(clock.position(112), 11, "Repeated valid sample is authoritative");
        for (auto changed : {tl::Track{"Other", "Artist", "", "brave", 0, "/track/1"},
                             tl::Track{"Song", "Artist", "", "vlc", 0, "/track/1"},
                             tl::Track{"Song", "Artist", "", "brave", 0, "/track/2"}}) {
            clock.update(track, 10, "Playing", 100);
            clock.update(changed, std::nullopt, "Playing", 105);
            require(!clock.position(110), "New song/player/track clears unavailable timing");
        }
        tl::PlaybackClock unknown;
        unknown.update(track, std::nullopt, "Playing", 100);
        require(!unknown.position(110), "Unknown position never invents a start");
        auto short_track = track;
        short_track.duration = 12;
        clock.update(short_track, 10, "Playing", 100);
        near(clock.position(110), 12, "Clock clamps to known track duration");
        clock.update(short_track, std::numeric_limits<double>::quiet_NaN(), "Paused", 111);
        near(clock.position(115), 12, "Invalid sample preserves known timeline");
        require(tl::monotonic_seconds() > 0, "Monotonic clock available");
        std::cout << "Core metadata, cache identity, and playback clock tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Core test failed: " << error.what() << '\n';
        return 1;
    }
}
