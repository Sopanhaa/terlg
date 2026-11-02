#include "terminal_lyrics/models.hpp"

#include <chrono>
#include <cmath>
#include <cctype>
#include <climits>
#include <cwchar>
#include <cwctype>
#include <iomanip>
#include <locale>
#include <sstream>

namespace tl {
namespace {
std::string field(const std::string& value) {
    return std::to_string(value.size()) + ":" + value;
}

bool whitespace(wchar_t c) {
    return std::iswspace(static_cast<wint_t>(c)) || c == 0x85 || c == 0xa0 ||
           c == 0x1680 || (c >= 0x2000 && c <= 0x200a) || c == 0x2028 ||
           c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000 ||
           (c >= 0x1c && c <= 0x1f);
}
}

double monotonic_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string trim(const std::string& text) {
    std::size_t begin = text.size(), end = 0;
    std::mbstate_t state{};
    for (std::size_t i = 0; i < text.size();) {
        wchar_t c = 0;
        std::size_t count = std::mbrtowc(&c, text.data() + i, text.size() - i, &state);
        if (count == static_cast<std::size_t>(-1) || count == static_cast<std::size_t>(-2)) {
            state = {};
            c = static_cast<unsigned char>(text[i]);
            count = 1;
        } else if (count == 0) {
            count = 1;
        }
        if (!whitespace(c)) {
            if (begin == text.size()) begin = i;
            end = i + count;
        }
        i += count;
    }
    return begin == text.size() ? "" : text.substr(begin, end - begin);
}

std::string lowercase(const std::string& text) {
    std::string result;
    std::mbstate_t input_state{}, output_state{};
    for (std::size_t i = 0; i < text.size();) {
        wchar_t c = 0;
        std::size_t count = std::mbrtowc(&c, text.data() + i, text.size() - i, &input_state);
        if (count == static_cast<std::size_t>(-1) || count == static_cast<std::size_t>(-2)) {
            input_state = {};
            result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(text[i]))));
            ++i;
            continue;
        }
        if (count == 0) count = 1;
        // Common case-fold expansions not supplied by towlower.
        if (c == 0xdf || c == 0x1e9e) {
            result += "ss";
        } else {
            c = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c)));
            if (c == 0x3c2) c = 0x3c3;
            char encoded[MB_LEN_MAX];
            const auto written = std::wcrtomb(encoded, c, &output_state);
            if (written == static_cast<std::size_t>(-1)) {
                output_state = {};
                result.append(text, i, count);
            } else {
                result.append(encoded, written);
            }
        }
        i += count;
    }
    return result;
}

std::string Track::key() const {
    return field(lowercase(title)) + field(lowercase(artist)) + field(lowercase(album));
}

std::string Track::lyrics_key() const {
    double rounded = 0;
    if (std::isfinite(duration) && duration > 0) {
        rounded = std::floor(duration);
        const double fraction = duration - rounded;
        if (fraction > 0.5 || (fraction == 0.5 && std::fmod(rounded, 2.0) != 0)) ++rounded;
    }
    std::ostringstream value;
    value.imbue(std::locale::classic());
    value << std::fixed << std::setprecision(0) << rounded;
    return key() + field(value.str());
}
}
