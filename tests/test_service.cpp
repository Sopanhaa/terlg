#include "terminal_lyrics/lyrics_service.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

tl::Track track(std::string title = "Song", std::string artist = "Artist", double duration = 0) {
    tl::Track result;
    result.title = std::move(title);
    result.artist = std::move(artist);
    result.duration = duration;
    return result;
}

tl::HttpResponse response(const std::string& body) { return {200, body, {}}; }

tl::LyricsResult fetch(const std::string& body, tl::Track item = track()) {
    return tl::fetch_lyrics(item, {}, [&](const std::string&, double timeout, const tl::Cancel&) {
        require(timeout > 0 && timeout <= 4, "request timeout must respect request/search limits");
        return response(body);
    });
}

const std::string exact = R"([{"id":1,"trackName":"Song","artistName":"Artist","syncedLyrics":"[00:01.00]First line"}])";
const std::string cover_title = "Habits - Vintage 1930's Jazz Tove Lo Cover ft. Haley Reinhart";

void test_lrc() {
    auto parsed = tl::parse_lrc("metadata\r\n[00:02.5]Later\n[00:01.001][00:03.00]Repeated\n[ar:Artist]\n[00:04.999]  Last  ");
    require(parsed.size() == 4, "LRC repeats, metadata and CRLF");
    require(std::abs(parsed[0].first - 1.001) < 1e-9 && parsed[0].second == "Repeated", "LRC millisecond precision");
    require(parsed[1] == tl::TimedLine(2.5, "Later") && parsed[2] == tl::TimedLine(3.0, "Repeated"), "LRC order");
    require(parsed[3].second == "Last", "LRC trim");
    require(tl::active_line(parsed, 1.0) == -1 && tl::active_line(parsed, 2.5) == 1 && tl::active_line({}, 100) == -1, "active line boundaries");
    require(tl::parse_lrc("[000:01]bad\n[00:01.1234]bad\nplain").empty(), "malformed tags ignored");
    std::string repeated;
    for (int i = 0; i < 1025; ++i) repeated += "[00:01]";
    require(tl::parse_lrc(repeated + "Repeating lyrics").empty(), "LRC timestamp expansion per line is bounded");
    repeated.clear();
    for (int i = 0; i < 32; ++i) repeated += "[00:01]";
    require(tl::parse_lrc(repeated + std::string(600000, 'x')).empty(), "LRC total expanded lyric bytes are bounded");
}

void test_candidates() {
    auto candidates = tl::search_candidates(track(cover_title, "PostmodernJukebox"));
    require(candidates.size() == 4, "cover avoids noisy description queries");
    require(candidates[0] == std::make_pair(std::string("Habits"), std::string("PostmodernJukebox")), "cover song query first");
    require(candidates[1].second == "Postmodern Jukebox" && candidates[2].second.empty(), "channel spacing before title fallback");
    candidates = tl::search_candidates(track("Artist // Song (Official Audio) – Uploader", "Channel"));
    require(candidates[0] == std::make_pair(std::string("Song"), std::string("Artist")), "double slash video metadata");
    candidates = tl::search_candidates(track("She & him - I Thought I Saw Your Face Today (Official Lyric Video) – SheandHimOfficial", ""));
    require(candidates[0] == std::make_pair(std::string("I Thought I Saw Your Face Today"), std::string("She & him")), "video qualifier stripped");
    candidates = tl::search_candidates(track("Song - Part Two"));
    require(candidates[0].first == "Song - Part Two" && candidates[0].second == "Artist", "ordinary dashed song preserves metadata");
    candidates = tl::search_candidates(track("Artist - Song - Acoustic Cover", "Uploader"));
    require(candidates[0] == std::make_pair(std::string("Song"), std::string("Artist")), "artist-first cover metadata");
}

void test_exact_and_query_encoding() {
    int calls = 0;
    auto item = track("Song & /?+", "Artist");
    item.album = "An Album";
    auto result = tl::fetch_lyrics(item, {}, [&](const std::string& url, double, const tl::Cancel&) {
        ++calls;
        require(url.find("track_name=Song+%26+%2F%3F%2B") != std::string::npos, "query percent encoding");
        require(url.find("album_name=An+Album") != std::string::npos, "reliable album filter");
        return response(R"([{"trackName":"Song & /?+","artistName":"Artist","syncedLyrics":"[00:01]First"}])");
    });
    require(calls == 1 && result.synced == std::vector<tl::TimedLine>{{1, "First"}}, "exact match returns sync in one request");
    result = fetch(R"([{"trackName":"Song","artistName":"Artist"},{"trackName":"Song","artistName":"Artist","syncedLyrics":"[00:01]Readable"}])");
    require(result.synced == std::vector<tl::TimedLine>{{1, "Readable"}}, "ID-less empty duplicate does not hide readable record");
}

void test_cover_and_plain_fallbacks() {
    auto item = track(cover_title, "PostmodernJukebox");
    item.album = "YouTube uploads";
    int calls = 0;
    auto result = tl::fetch_lyrics(item, {}, [&](const std::string& url, double, const tl::Cancel&) {
        ++calls;
        require(url.find("track_name=Habits") != std::string::npos && url.find("album_name") == std::string::npos, "derived title omits album");
        if (calls == 2) return response(R"([{"trackName":"Habits","artistName":"Postmodern Jukebox","syncedLyrics":"[00:02]Cover line"}])");
        return response("[]");
    });
    require(calls == 2 && result.synced == std::vector<tl::TimedLine>{{2, "Cover line"}}, "spaced uploader sync found in two requests");
    calls = 0;
    result = tl::fetch_lyrics(item, {}, [&](const std::string& url, double, const tl::Cancel&) {
        ++calls;
        if (url.find("artist_name") == std::string::npos)
            return response(R"json([{"trackName":"Habits (Stay High)","artistName":"Tove Lo","syncedLyrics":"[00:01]Original timing","plainLyrics":"First verse\n\nSecond verse"}])json");
        return response("[]");
    });
    require(calls == 3 && result.synced.empty() && result.plain == std::vector<std::string>{"First verse", "", "Second verse"}, "title-only subtitle fallback retains verses without unrelated timing");
    result = fetch(R"([{"trackName":"Song","artistName":"Other Artist","syncedLyrics":"[00:01]First\n[00:02]Second"}])");
    require(result.synced.empty() && result.plain == std::vector<std::string>{"First", "Second"}, "derive readable fallback from other artist LRC");
    result = fetch(R"([{"trackName":"Bad Song","artistName":"Artist","plainLyrics":"Unrelated"}])");
    require(!result.has_lyrics() && result.message == "Lyrics were not found.", "reject keyword-only title match");
    result = fetch(R"([{"trackName":"Song","artistName":"Artist Tribute Band","syncedLyrics":"[00:01]Tribute"}])");
    require(result.synced.empty() && result.plain == std::vector<std::string>{"Tribute"}, "reject keyword-only artist sync");
    calls = 0;
    result = tl::fetch_lyrics(track(), {}, [&](const std::string&, double, const tl::Cancel&) {
        return ++calls == 1 ? response(R"json([{"id":1,"trackName":"Song (Live)","artistName":"Artist","syncedLyrics":"[00:01]Live"}])json") : response(exact);
    });
    require(calls == 2 && result.synced == std::vector<tl::TimedLine>{{1, "First line"}}, "later exact title beats same-artist live subtitle");
    result = fetch(R"([{"trackName":"Habits","artistName":"Scott Bradlee's Postmodern Jukebox","syncedLyrics":"[00:07]Cover"}])", item);
    require(result.synced == std::vector<tl::TimedLine>{{7, "Cover"}}, "band owner prefix matches uploader");
}

void test_duration_ranking() {
    auto item = track("Song", "Artist", 180);
    item.album = "Video album";
    auto result = fetch(R"([{"id":1,"trackName":"Song","artistName":"Artist","albumName":"Video album","duration":240,"syncedLyrics":"[00:10]Wrong arrangement"},{"id":2,"trackName":"Song","artistName":"Artist","duration":180.5,"syncedLyrics":"[00:02]Right arrangement"}])", item);
    require(result.synced == std::vector<tl::TimedLine>{{2, "Right arrangement"}}, "duration beats album match");
    result = fetch(R"([{"trackName":"Song","artistName":"Artist","duration":180,"plainLyrics":"Untimed duplicate"},{"trackName":"Song","artistName":"Artist","duration":181,"syncedLyrics":"[00:02]Timed"}])", item);
    require(result.synced == std::vector<tl::TimedLine>{{2, "Timed"}}, "timed within tolerance beats untimed duplicate");
    result = fetch(R"([{"trackName":"Song","artistName":"Other","duration":180,"syncedLyrics":"[00:01]Other"},{"trackName":"Song","artistName":"Artist","duration":181,"syncedLyrics":"[00:03]Expected"}])", item);
    require(result.synced == std::vector<tl::TimedLine>{{3, "Expected"}}, "duration cannot override performer");
    result = fetch(R"([{"trackName":"Song","artistName":"Artist","duration":240,"syncedLyrics":"[00:10]Wrong"},{"trackName":"Song","artistName":"Artist","duration":"nan","syncedLyrics":"[00:02]Unknown"}])", item);
    require(result.synced == std::vector<tl::TimedLine>{{2, "Unknown"}}, "unknown duration beats known mismatch");
    int calls = 0;
    result = tl::fetch_lyrics(item, {}, [&](const std::string&, double, const tl::Cancel&) {
        return ++calls == 1 ? response(R"([{"id":1,"trackName":"Song","artistName":"Artist","duration":240,"syncedLyrics":"[00:10]Wrong"}])") : response(R"([{"id":2,"trackName":"Song","artistName":"Artist","duration":180,"syncedLyrics":"[00:02]Right"}])");
    });
    require(calls == 2 && result.synced == std::vector<tl::TimedLine>{{2, "Right"}}, "mismatched duration continues fallbacks");
}

void test_json_and_unicode() {
    auto result = fetch(R"([{"trackName":"S\u00f3ng","artistName":"\u00c1RTIST","syncedLyrics":"[00:01]\u1781\u17d2\u1798\u17c2\u179a \ud83c\udfb5 \/ \" \\ \t"}])", track("Sóng", "ártist"));
    require(result.synced.size() == 1 && result.synced[0].second == "ខ្មែរ 🎵 / \" \\" , "Unicode escapes, surrogate pair and JSON escapes");
    result = fetch(R"([{"trackName":"Stra\u00dfe","artistName":"Artist","plainLyrics":"Unicode casefold"}])", track("STRASSE"));
    require(result.plain == std::vector<std::string>{"Unicode casefold"}, "Unicode case-fold expansion");
    for (const auto& malformed : {std::string("["), std::string(R"([{"trackName":"Song","plainLyrics":"\ud800"}])"), std::string(R"([{"x":01}])"), std::string(R"([{"x":1e}])"), std::string("[] trailing")}) {
        result = fetch(malformed);
        require(!result.has_lyrics() && result.message.find("response") != std::string::npos, "malformed JSON is reported");
    }
    std::string invalid = "[{\"plainLyrics\":\"";
    invalid += static_cast<char>(0xff);
    invalid += "\"}]";
    require(!fetch(invalid).message.empty(), "invalid UTF-8 rejected");
    std::string nested(70, '['); nested += "null"; nested += std::string(70, ']');
    require(fetch(nested).message.find("complexity") != std::string::npos, "JSON depth bounded");
    require(fetch(std::string(8 * 1024 * 1024 + 1, ' ')).message.find("size limit") != std::string::npos, "JSON response size bounded");
}

void test_errors_retries_and_cancellation() {
    int calls = 0;
    auto result = tl::fetch_lyrics(track(), {}, [&](const std::string&, double, const tl::Cancel&) {
        return ++calls == 1 ? tl::HttpResponse{503, {}, {}} : response(exact);
    });
    require(calls == 2 && result.has_lyrics(), "temporary HTTP error retried");
    calls = 0;
    result = tl::fetch_lyrics(track(), {}, [&](const std::string&, double, const tl::Cancel&) { ++calls; return tl::HttpResponse{503, {}, {}}; });
    require(calls == 3 && result.message.find("503") != std::string::npos, "retry attempts bounded");
    calls = 0;
    result = tl::fetch_lyrics(track(), {}, [&](const std::string&, double, const tl::Cancel&) { ++calls; return tl::HttpResponse{429, {}, {}}; });
    require(calls == 1 && result.message.find("rate limiting") != std::string::npos, "rate limit does not retry");
    result = tl::fetch_lyrics(track(), {}, [](const std::string&, double, const tl::Cancel&) { return tl::HttpResponse{404, {}, {}}; });
    require(result.message == "Lyrics were not found.", "404 is empty result");
    calls = 0;
    result = tl::fetch_lyrics(track(), {}, [&](const std::string&, double, const tl::Cancel&) {
        return ++calls == 1 ? response(R"([{"trackName":"Song","artistName":"Other","plainLyrics":"Readable fallback"}])") : tl::HttpResponse{503, {}, {}};
    });
    require(calls == 2 && result.plain == std::vector<std::string>{"Readable fallback"}, "optional failure preserves fallback and skips retry");
    auto cancel = std::make_shared<std::atomic_bool>(true);
    calls = 0;
    result = tl::fetch_lyrics(track(), cancel, [&](const std::string&, double, const tl::Cancel&) { ++calls; return response(exact); });
    require(calls == 0 && !result.has_lyrics() && result.message.empty(), "pre-cancellation makes no request");
    cancel->store(false);
    result = tl::fetch_lyrics(track(), cancel, [&](const std::string&, double, const tl::Cancel&) { cancel->store(true); return response(exact); });
    require(!result.has_lyrics() && result.message.empty(), "cancellation during response discards result");
    cancel->store(false);
    auto start = std::chrono::steady_clock::now();
    std::thread stop([cancel] { std::this_thread::sleep_for(std::chrono::milliseconds(50)); cancel->store(true); });
    calls = 0;
    result = tl::fetch_lyrics(track(), cancel, [&](const std::string&, double, const tl::Cancel&) { ++calls; return tl::HttpResponse{503, {}, {}}; });
    stop.join();
    require(calls == 1 && result.message.empty() && std::chrono::steady_clock::now() - start < std::chrono::milliseconds(300), "retry backoff responds promptly to cancellation");
}
}

int main() {
    try {
        test_lrc();
        test_candidates();
        test_exact_and_query_encoding();
        test_cover_and_plain_fallbacks();
        test_duration_ranking();
        test_json_and_unicode();
        test_errors_retries_and_cancellation();
        std::cout << "lyrics service tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "lyrics service test failure: " << error.what() << '\n';
        return 1;
    }
}
