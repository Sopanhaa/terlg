#include "terminal_lyrics/lyrics_service.hpp"
#include "terminal_lyrics/process.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <tuple>

namespace tl {
namespace {
constexpr double request_timeout = 4.0;
constexpr double search_timeout = 15.0;
constexpr std::size_t maximum_json_size = 8 * 1024 * 1024;
const std::regex time_tag(R"(\[(\d{1,2}):(\d{2})(?:\.(\d{1,3}))?\])");

bool cancelled(const Cancel& cancel) { return cancel && cancel->load(); }

void append_utf8(std::string& out, std::uint32_t code) {
    if (code <= 0x7f) out.push_back(static_cast<char>(code));
    else if (code <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    } else if (code <= 0xffff) {
        out.push_back(static_cast<char>(0xe0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    }
}

std::uint32_t read_utf8(const std::string& text, std::size_t& offset) {
    if (offset >= text.size()) throw std::runtime_error("Incomplete UTF-8 text");
    auto first = static_cast<unsigned char>(text[offset++]);
    if (first < 0x80) return first;
    unsigned length = first >= 0xc2 && first <= 0xdf ? 2 :
                      first >= 0xe0 && first <= 0xef ? 3 :
                      first >= 0xf0 && first <= 0xf4 ? 4 : 0;
    if (!length || offset + length - 1 > text.size()) throw std::runtime_error("Invalid UTF-8 text");
    std::uint32_t code = first & ((1u << (7 - length)) - 1);
    for (unsigned i = 1; i < length; ++i) {
        auto next = static_cast<unsigned char>(text[offset++]);
        if ((next & 0xc0) != 0x80) throw std::runtime_error("Invalid UTF-8 continuation");
        code = (code << 6) | (next & 0x3f);
    }
    if ((length == 2 && code < 0x80) || (length == 3 && code < 0x800) ||
        (length == 4 && code < 0x10000) || code > 0x10ffff ||
        (code >= 0xd800 && code <= 0xdfff)) throw std::runtime_error("Invalid UTF-8 code point");
    return code;
}

const std::locale& unicode_locale() {
    static const std::locale locale = [] {
        try { return std::locale("C.UTF-8"); }
        catch (...) { try { return std::locale(""); } catch (...) { return std::locale::classic(); } }
    }();
    return locale;
}

std::string folded(const std::string& text, bool alphanumeric_only = false) {
    std::string result;
    const auto& facet = std::use_facet<std::ctype<wchar_t>>(unicode_locale());
    for (std::size_t offset = 0; offset < text.size();) {
        std::uint32_t code;
        try { code = read_utf8(text, offset); }
        catch (...) { continue; }
        // Common case-fold expansions absent from the standard locale facet.
        if (code == 0xdf || code == 0x1e9e) { result += "ss"; continue; }
        if (code == 0x130) { result += "i"; if (!alphanumeric_only) append_utf8(result, 0x307); continue; }
        if (code == 0x3c2) code = 0x3c3;
        auto letter = static_cast<wchar_t>(code);
        if (alphanumeric_only && !facet.is(std::ctype_base::alnum, letter)) continue;
        append_utf8(result, static_cast<std::uint32_t>(facet.tolower(letter)));
    }
    return result;
}

std::string normalized(const std::string& text) { return folded(text, true); }

bool space(unsigned char ch) { return std::isspace(ch) != 0; }

std::string strip_video_qualifiers(const std::string& text) {
    std::string result;
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '(' || text[i] == '[') {
            auto end = text.find_first_of(")]", i + 1);
            if (end == std::string::npos) { result.append(text, i, std::string::npos); break; }
            else {
                auto inside = lowercase(text.substr(i + 1, end - i - 1));
                if (inside.find("official") != std::string::npos || inside.find("lyric") != std::string::npos ||
                    inside.find("audio") != std::string::npos || inside.find("video") != std::string::npos || inside.find("visualizer") != std::string::npos) {
                    while (!result.empty() && space(static_cast<unsigned char>(result.back()))) result.pop_back();
                    i = end + 1;
                    continue;
                }
                result.append(text, i, end - i + 1);
                i = end + 1;
                continue;
            }
        }
        result += text[i++];
    }
    return result;
}

bool cover_description(const std::string& text) {
    auto lower = lowercase(trim(text));
    auto word = [](unsigned char ch) { return std::isalnum(ch) || ch == '_'; };
    for (std::size_t offset = 0; offset < lower.size(); ++offset) {
        if (offset && word(static_cast<unsigned char>(lower[offset - 1]))) continue;
        std::size_t end;
        if (lower.compare(offset, 5, "cover") == 0) end = offset + 5;
        else if (lower.compare(offset, 9, "rendition") == 0) end = offset + 9;
        else continue;
        if (end == lower.size()) return true;
        if (!space(static_cast<unsigned char>(lower[end]))) continue;
        while (end < lower.size() && space(static_cast<unsigned char>(lower[end]))) ++end;
        if (end == lower.size()) return true;
        auto rest = lower.substr(end);
        for (const auto& prefix : {"ft", "feat", "by"}) {
            std::size_t length = std::char_traits<char>::length(prefix);
            if (rest.compare(0, length, prefix) != 0) continue;
            if (std::string(prefix) != "by" && rest.size() > length && rest[length] == '.') ++length;
            if (rest.size() > length && space(static_cast<unsigned char>(rest[length]))) return true;
        }
    }
    return false;
}

std::vector<std::string> split_dashes(const std::string& text) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (std::size_t i = 1; i < text.size();) {
        auto length = text[i] == '-' ? 1u : text.compare(i, 3, "–") == 0 || text.compare(i, 3, "—") == 0 ? 3u : 0u;
        if (!length || !space(static_cast<unsigned char>(text[i - 1])) || i + length >= text.size() || !space(static_cast<unsigned char>(text[i + length]))) { ++i; continue; }
        parts.push_back(trim(text.substr(start, i - start)));
        i += length;
        while (i < text.size() && space(static_cast<unsigned char>(text[i]))) ++i;
        start = i;
    }
    parts.push_back(trim(text.substr(start)));
    return parts;
}

std::string strip_subtitle(const std::string& text) {
    auto result = trim(text);
    if (result.empty() || (result.back() != ')' && result.back() != ']')) return result;
    auto previous_close = result.find_last_of(")]", result.size() - 2);
    auto start = result.find_first_of("([", previous_close == std::string::npos ? 0 : previous_close + 1);
    return start == std::string::npos ? result : trim(result.substr(0, start));
}

std::set<std::string> artist_keys(const std::string& artist) {
    std::string without_owner = artist;
    for (std::size_t i = 1; i < artist.size(); ++i) {
        auto length = artist[i] == '\'' ? 1u : artist.compare(i, 3, "’") == 0 ? 3u : 0u;
        if (!length || i + length + 1 >= artist.size()) continue;
        auto s = artist[i + length];
        if ((s != 's' && s != 'S') || !space(static_cast<unsigned char>(artist[i + length + 1]))) continue;
        auto start = i + length + 1;
        while (start < artist.size() && space(static_cast<unsigned char>(artist[start]))) ++start;
        without_owner = artist.substr(start);
        break;
    }
    std::set<std::string> keys{normalized(artist), normalized(without_owner)};
    keys.erase("");
    return keys;
}

bool title_matches(const std::string& record, const std::string& title) {
    return normalized(record) == normalized(title) ||
           normalized(strip_subtitle(record)) == normalized(title);
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' || text[i] == '\n') {
            lines.push_back(text.substr(start, i - start));
            if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            start = i + 1;
        }
    }
    if (start < text.size()) lines.push_back(text.substr(start));
    return lines;
}

// A bounded JSON reader keeps the runtime dependency limited to curl. It rejects
// malformed encodings, excessive nesting, and oversized responses before use.
struct Json {
    enum class Kind { null, boolean, number, string, array, object } kind = Kind::null;
    std::string value;
    std::vector<Json> items;
    std::map<std::string, Json> members;
    const Json* get(const std::string& name) const {
        auto found = members.find(name);
        return found == members.end() ? nullptr : &found->second;
    }
    std::string string(const std::string& name) const {
        const auto* entry = get(name);
        return entry && entry->kind == Kind::string ? entry->value : "";
    }
};

class JsonReader {
    const std::string& text_;
    Cancel cancel_;
    std::size_t offset_ = 0, nodes_ = 0;
    [[noreturn]] void fail() const { throw std::runtime_error("Invalid JSON response at byte " + std::to_string(offset_)); }
    void check_cancel() const { if (cancelled(cancel_)) throw std::runtime_error("Lyrics search cancelled"); }
    void whitespace() {
        while (offset_ < text_.size() && (text_[offset_] == ' ' || text_[offset_] == '\r' || text_[offset_] == '\n' || text_[offset_] == '\t')) {
            if (offset_ % 4096 == 0) check_cancel();
            ++offset_;
        }
    }
    bool take(char ch) { whitespace(); if (offset_ < text_.size() && text_[offset_] == ch) { ++offset_; return true; } return false; }
    std::uint32_t hex4() {
        if (offset_ + 4 > text_.size()) fail();
        std::uint32_t code = 0;
        for (unsigned i = 0; i < 4; ++i) {
            auto c = static_cast<unsigned char>(text_[offset_++]);
            unsigned digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
            if (digit == 16) fail();
            code = code * 16 + digit;
        }
        return code;
    }
    std::string string() {
        if (!take('"')) fail();
        std::string out;
        while (offset_ < text_.size()) {
            if (offset_ % 4096 == 0) check_cancel();
            auto c = static_cast<unsigned char>(text_[offset_++]);
            if (c == '"') return out;
            if (c < 0x20) fail();
            if (c == '\\') {
                if (offset_ == text_.size()) fail();
                char escaped = text_[offset_++];
                switch (escaped) {
                case '"': case '\\': case '/': out += escaped; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    auto code = hex4();
                    if (code >= 0xd800 && code <= 0xdbff) {
                        if (offset_ + 2 > text_.size() || text_[offset_++] != '\\' || text_[offset_++] != 'u') fail();
                        auto low = hex4();
                        if (low < 0xdc00 || low > 0xdfff) fail();
                        code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                    } else if (code >= 0xdc00 && code <= 0xdfff) fail();
                    append_utf8(out, code);
                    break;
                }
                default: fail();
                }
            } else if (c < 0x80) out += static_cast<char>(c);
            else {
                --offset_;
                append_utf8(out, read_utf8(text_, offset_));
            }
        }
        fail();
    }
    Json value(unsigned depth) {
        if (depth > 64 || ++nodes_ > 200000) throw std::runtime_error("JSON response exceeds complexity limit");
        if (nodes_ % 256 == 0) check_cancel();
        whitespace();
        if (offset_ == text_.size()) fail();
        Json result;
        char ch = text_[offset_];
        if (ch == '"') { result.kind = Json::Kind::string; result.value = string(); }
        else if (take('[')) {
            result.kind = Json::Kind::array;
            if (!take(']')) {
                do { result.items.push_back(value(depth + 1)); } while (take(','));
                if (!take(']')) fail();
            }
        } else if (take('{')) {
            result.kind = Json::Kind::object;
            if (!take('}')) {
                do {
                    auto name = string();
                    if (!take(':')) fail();
                    result.members[std::move(name)] = value(depth + 1);
                } while (take(','));
                if (!take('}')) fail();
            }
        } else if (text_.compare(offset_, 4, "null") == 0) offset_ += 4;
        else if (text_.compare(offset_, 4, "true") == 0) { offset_ += 4; result.kind = Json::Kind::boolean; result.value = "true"; }
        else if (text_.compare(offset_, 5, "false") == 0) { offset_ += 5; result.kind = Json::Kind::boolean; result.value = "false"; }
        else {
            auto begin = offset_;
            if (text_[offset_] == '-') ++offset_;
            if (offset_ == text_.size()) fail();
            if (text_[offset_] == '0') ++offset_;
            else {
                if (text_[offset_] < '1' || text_[offset_] > '9') fail();
                while (offset_ < text_.size() && text_[offset_] >= '0' && text_[offset_] <= '9') ++offset_;
            }
            if (offset_ < text_.size() && text_[offset_] == '.') {
                auto first = ++offset_;
                while (offset_ < text_.size() && text_[offset_] >= '0' && text_[offset_] <= '9') ++offset_;
                if (first == offset_) fail();
            }
            if (offset_ < text_.size() && (text_[offset_] == 'e' || text_[offset_] == 'E')) {
                ++offset_;
                if (offset_ < text_.size() && (text_[offset_] == '+' || text_[offset_] == '-')) ++offset_;
                auto first = offset_;
                while (offset_ < text_.size() && text_[offset_] >= '0' && text_[offset_] <= '9') ++offset_;
                if (first == offset_) fail();
            }
            result.kind = Json::Kind::number;
            result.value = text_.substr(begin, offset_ - begin);
        }
        return result;
    }
public:
    explicit JsonReader(const std::string& text, const Cancel& cancel) : text_(text), cancel_(cancel) {}
    Json parse() {
        if (text_.size() > maximum_json_size) throw std::runtime_error("Lyrics response exceeds size limit");
        auto result = value(0);
        whitespace();
        if (offset_ != text_.size()) fail();
        return result;
    }
};

std::string identity(const Json& json) {
    // Length prefixes make this a canonical, unambiguous internal identity.
    std::string result = std::to_string(static_cast<int>(json.kind)) + ":" + std::to_string(json.value.size()) + ":" + json.value;
    for (const auto& item : json.items) { auto part = identity(item); result += std::to_string(part.size()) + ":" + part; }
    for (const auto& item : json.members) {
        auto part = identity(item.second);
        result += std::to_string(item.first.size()) + ":" + item.first + std::to_string(part.size()) + ":" + part;
    }
    return result;
}

std::string url_encode(const std::string& text) {
    static const char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (unsigned char ch : text) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' || ch == '_' || ch == '~') encoded += static_cast<char>(ch);
        else if (ch == ' ') encoded += '+';
        else { encoded += '%'; encoded += hex[ch >> 4]; encoded += hex[ch & 15]; }
    }
    return encoded;
}

HttpResponse http_get(const std::string& url, double timeout, const Cancel& cancel) {
    auto result = run_process({"curl", "--silent", "--show-error", "--location", "--max-time", std::to_string(timeout),
                               "--user-agent", "TerminalLyrics/0.1.0 (https://lrclib.net/docs)", "--header", "Accept: application/json",
                               "--write-out", "\n%{http_code}", "--", url}, timeout + 0.1, cancel);
    if (result.cancelled) return {};
    if (result.timed_out) return {0, {}, "Request timed out"};
    if (result.exit_code != 0) return {0, {}, trim(result.error).empty() ? "curl failed (exit " + std::to_string(result.exit_code) + ")" : trim(result.error)};
    auto separator = result.output.rfind('\n');
    if (separator == std::string::npos) return {0, {}, "Missing HTTP response status"};
    auto code = result.output.substr(separator + 1);
    if (code.size() != 3 || !std::all_of(code.begin(), code.end(), [](unsigned char c) { return c >= '0' && c <= '9'; }))
        return {0, {}, "Invalid HTTP response status"};
    return {std::stoi(code), result.output.substr(0, separator), {}};
}

bool wait_retry(double delay, const Cancel& cancel) {
    auto stop = std::chrono::steady_clock::now() + std::chrono::duration<double>(delay);
    while (!cancelled(cancel)) {
        auto remaining = stop - std::chrono::steady_clock::now();
        if (remaining <= decltype(remaining)::zero()) return true;
        std::this_thread::sleep_for(std::min(remaining, std::chrono::duration_cast<decltype(remaining)>(std::chrono::milliseconds(25))));
    }
    return false;
}

struct Record {
    Json json;
    bool artist_matches = false;
    std::string title() const { return json.string("trackName"); }
    std::string synced() const { return json.string("syncedLyrics"); }
    std::string plain() const { return json.string("plainLyrics"); }
    bool has_text() const { return !synced().empty() || !plain().empty(); }
    bool readable() const { return !trim(plain()).empty() || !parse_lrc(synced()).empty(); }
};

double duration_difference(const Record& record, const Track& track) {
    if (!std::isfinite(track.duration) || track.duration <= 0) return -1;
    const auto* value = record.json.get("duration");
    if (!value || (value->kind != Json::Kind::number && value->kind != Json::Kind::string)) return -1;
    try {
        auto text = trim(value->value);
        std::size_t consumed = 0;
        double duration = std::stod(text, &consumed);
        if (consumed != text.size() || !std::isfinite(duration) || duration <= 0) return -1;
        return std::abs(duration - track.duration);
    } catch (...) { return -1; }
}

bool timing_compatible(const Record& record, const Track& track) {
    return duration_difference(record, track) <= 2.0;
}

} // namespace

std::vector<TimedLine> parse_lrc(const std::string& text) {
    if (text.size() > maximum_json_size) return {};
    constexpr std::size_t maximum_lines = 100000;
    constexpr std::size_t maximum_line_tags = 1024;
    constexpr std::size_t maximum_expanded_bytes = 16 * 1024 * 1024;
    std::vector<TimedLine> lines;
    std::size_t expanded_bytes = 0;
    for (const auto& raw : split_lines(text)) {
        std::string lyric;
        std::size_t start = 0;
        std::vector<double> timestamps;
        for (std::sregex_iterator it(raw.begin(), raw.end(), time_tag), end; it != end; ++it) {
            if (timestamps.size() >= maximum_line_tags) return {};
            const auto& tag = *it;
            auto position = static_cast<std::size_t>(tag.position());
            lyric.append(raw, start, position - start);
            start = position + static_cast<std::size_t>(tag.length());
            double seconds = std::stoi(tag[1].str()) * 60 + std::stoi(tag[2].str());
            if (tag[3].matched) seconds += std::stoi(tag[3].str()) / std::pow(10.0, static_cast<double>(tag[3].length()));
            timestamps.push_back(seconds);
        }
        if (timestamps.empty()) continue;
        lyric.append(raw, start, std::string::npos);
        lyric = trim(lyric);
        if (lines.size() + timestamps.size() > maximum_lines ||
            (!lyric.empty() && timestamps.size() > (maximum_expanded_bytes - expanded_bytes) / lyric.size())) return {};
        expanded_bytes += timestamps.size() * lyric.size();
        for (double timestamp : timestamps) lines.emplace_back(timestamp, lyric);
    }
    std::sort(lines.begin(), lines.end());
    return lines;
}

int active_line(const std::vector<TimedLine>& lyrics, double position) {
    int current = -1;
    for (std::size_t i = 0; i < lyrics.size(); ++i) {
        if (lyrics[i].first > position) break;
        current = static_cast<int>(i);
    }
    return current;
}

std::vector<std::pair<std::string, std::string>> search_candidates(const Track& track) {
    auto cleaned = trim(strip_video_qualifiers(track.title));
    std::vector<std::pair<std::string, std::string>> derived, searches;
    bool video_metadata = cleaned != track.title || cleaned.find("//") != std::string::npos ||
                          cover_description(cleaned) || trim(track.artist).empty();
    auto slash = cleaned.find("//");
    if (slash != std::string::npos) {
        auto artist_hint = trim(cleaned.substr(0, slash));
        auto song_and_channel = trim(cleaned.substr(slash + 2));
        auto song = split_dashes(song_and_channel).front();
        if (!song.empty()) {
            if (!artist_hint.empty()) derived.emplace_back(song, artist_hint);
            derived.emplace_back(song, track.artist);
        }
    } else {
        auto parts = split_dashes(cleaned);
        if (parts.size() > 1) {
            auto first = parts[0];
            if (normalized(first) == normalized(track.artist)) video_metadata = true;
            if (parts.size() == 2 && cover_description(parts[1])) derived.emplace_back(first, track.artist);
            else {
                auto song = parts[1];
                derived.emplace_back(song, first);
                derived.emplace_back(song, track.artist);
                if (!track.artist.empty() && normalized(first) != normalized(track.artist)) derived.emplace_back(first, track.artist);
            }
        }
    }
    if (derived.empty()) derived.emplace_back(cleaned, track.artist);
    if (!video_metadata) searches.emplace_back(track.title, track.artist);
    for (const auto& candidate : derived) {
        if (candidate.second.empty()) continue;
        searches.push_back(candidate);
        std::string spaced;
        for (std::size_t i = 0; i < candidate.second.size(); ++i) {
            unsigned char ch = candidate.second[i];
            unsigned char before = i ? candidate.second[i - 1] : 0;
            if (i && ((before >= 'a' && before <= 'z') || (before >= '0' && before <= '9')) && ch >= 'A' && ch <= 'Z') spaced += ' ';
            spaced += static_cast<char>(ch);
        }
        if (spaced != candidate.second) searches.emplace_back(candidate.first, spaced);
    }
    for (const auto& candidate : derived) searches.emplace_back(candidate.first, "");
    searches.emplace_back(track.title, track.artist);
    std::vector<std::pair<std::string, std::string>> unique;
    std::set<std::pair<std::string, std::string>> seen;
    for (auto candidate : searches) {
        candidate.first = trim(candidate.first);
        candidate.second = trim(candidate.second);
        if (!candidate.first.empty() && seen.emplace(folded(candidate.first), folded(candidate.second)).second) unique.push_back(std::move(candidate));
    }
    return unique;
}

LyricsResult fetch_lyrics(const Track& track, const Cancel& cancel, HttpGet get) {
    if (!get) get = http_get;
    const double deadline = monotonic_seconds() + search_timeout;
    const auto searches = search_candidates(track);
    std::vector<std::pair<std::string, std::set<std::string>>> known_artists;
    for (const auto& candidate : searches) if (!candidate.second.empty()) known_artists.emplace_back(candidate.first, artist_keys(candidate.second));
    std::vector<Record> records;
    std::map<std::string, std::size_t> identities;
    auto have_lyrics = [&] { return std::any_of(records.begin(), records.end(), [](const Record& record) { return record.readable(); }); };
    std::string error;
    for (const auto& candidate : searches) {
        if (cancelled(cancel)) return {};
        if (monotonic_seconds() >= deadline) { error = "Lyrics search timed out. Please try again."; break; }
        const auto& title = candidate.first;
        const auto& artist = candidate.second;
        auto url = "https://lrclib.net/api/search?track_name=" + url_encode(title);
        if (!artist.empty()) url += "&artist_name=" + url_encode(artist);
        if (!track.album.empty() && candidate == std::make_pair(track.title, track.artist)) url += "&album_name=" + url_encode(track.album);
        Json results;
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (cancelled(cancel)) return {};
            double remaining = deadline - monotonic_seconds();
            if (remaining <= 0) { error = "Lyrics search timed out. Please try again."; break; }
            bool retry = false;
            try {
                auto response = get(url, std::min(request_timeout, remaining), cancel);
                if (cancelled(cancel)) return {};
                if (!response.error.empty() || response.status == 0) {
                    error = "Could not reach the lyrics service: " + (response.error.empty() ? std::string("No HTTP response") : response.error);
                    retry = true;
                } else if (response.status == 404) { results.kind = Json::Kind::array; error.clear(); break; }
                else if (response.status == 429) error = "Lyrics service is rate limiting requests. Please wait a little and retry.";
                else if (response.status < 200 || response.status >= 300) {
                    error = "Lyrics service returned HTTP " + std::to_string(response.status) + ".";
                    retry = response.status == 500 || response.status == 502 || response.status == 503 || response.status == 504;
                } else { results = JsonReader(response.body, cancel).parse(); error.clear(); break; }
            } catch (const std::exception& exception) {
                error = std::string("Could not read the lyrics service response: ") + exception.what();
            }
            if (cancelled(cancel)) return {};
            if (!retry || attempt == 2 || have_lyrics()) break;
            remaining = deadline - monotonic_seconds();
            if (remaining <= 0) { error = "Lyrics search timed out. Please try again."; break; }
            if (!wait_retry(std::min(0.5 * (1 << attempt), remaining), cancel)) return {};
            error.clear();
        }
        if (cancelled(cancel)) return {};
        if (!error.empty()) break;
        if (results.kind != Json::Kind::array) continue;
        for (auto& json : results.items) {
            if (cancelled(cancel)) return {};
            if (json.kind != Json::Kind::object || !title_matches(json.string("trackName"), title)) continue;
            auto keys = artist_keys(json.string("artistName"));
            bool matches = false;
            for (const auto& known : known_artists) {
                if (!title_matches(json.string("trackName"), known.first)) continue;
                for (const auto& key : known.second) if (keys.count(key)) { matches = true; break; }
                if (matches) break;
            }
            const auto* id = json.get("id");
            auto key = id && (id->kind == Json::Kind::string || id->kind == Json::Kind::number || id->kind == Json::Kind::boolean)
                       ? "id:" + id->value : "record:" + identity(json);
            auto found = identities.find(key);
            if (found == identities.end()) {
                identities[key] = records.size();
                records.push_back({std::move(json), matches});
            } else records[found->second] = {std::move(json), matches};
        }
        bool exact = std::any_of(records.begin(), records.end(), [&](const Record& record) {
            return record.artist_matches && normalized(record.title()) == normalized(title) && record.has_text() && timing_compatible(record, track);
        });
        if (exact) break;
        if (artist.empty() && have_lyrics() && std::any_of(records.begin(), records.end(), [&](const Record& record) { return timing_compatible(record, track) && record.has_text(); })) break;
    }
    if (!error.empty() && !have_lyrics()) return {{}, {}, error};
    if (records.empty()) return {{}, {}, "Lyrics were not found."};
    auto exact_title = [&](const Record& record) {
        return std::any_of(searches.begin(), searches.end(), [&](const auto& candidate) { return normalized(record.title()) == normalized(candidate.first); });
    };
    auto rank = [&](const Record& record) {
        const double difference = duration_difference(record, track);
        auto duration = std::make_tuple(difference < 0 ? 0 : difference <= 2 ? 1 : -1,
                                       difference >= 0 && difference <= 2 && !record.synced().empty(), difference < 0 ? 0.0 : -difference);
        return std::make_tuple(record.has_text(), (exact_title(record) ? 2 : 0) + (record.artist_matches ? 4 : 0), duration,
                               !track.album.empty() && folded(record.json.string("albumName")) == folded(track.album), !record.synced().empty());
    };
    auto best = std::max_element(records.begin(), records.end(), [&](const Record& left, const Record& right) { return rank(left) < rank(right); });
    auto synced = parse_lrc(best->synced());
    auto plain = split_lines(trim(best->plain()));
    if (!synced.empty() && best->artist_matches && exact_title(*best)) return {std::move(synced), {}, {}};
    if (!synced.empty() && plain.empty()) {
        std::string text;
        for (const auto& line : synced) { if (!text.empty()) text += '\n'; text += line.second; }
        plain = split_lines(trim(text));
    }
    if (!plain.empty()) return {{}, std::move(plain), {}};
    return {{}, {}, "This match does not include readable lyrics."};
}

} // namespace tl
