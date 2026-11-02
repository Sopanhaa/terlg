#include "terminal_lyrics/ui.hpp"
#include "terminal_lyrics/lyrics_loader.hpp"
#include "terminal_lyrics/playback_clock.hpp"
#include "terminal_lyrics/player.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <locale.h>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace tl {
namespace {
struct Glyph { std::string text; char32_t code; int width; };

// Use a thread-local UTF-8 locale without changing the locale of lyric workers.
class Utf8Locale {
public:
    Utf8Locale() {
        static locale_t utf8 = [] {
            locale_t locale = newlocale(LC_CTYPE_MASK, "C.UTF-8", nullptr);
            if (!locale) locale = newlocale(LC_CTYPE_MASK, "en_US.UTF-8", nullptr);
            return locale;
        }();
        if (utf8) previous_ = uselocale(utf8);
    }
    ~Utf8Locale() { if (previous_) uselocale(previous_); }
private:
    locale_t previous_ = nullptr;
};

bool wide_character(char32_t c) {
    return c >= 0x1100 && (c <= 0x115f || c == 0x2329 || c == 0x232a ||
        (c >= 0x2e80 && c <= 0xa4cf && c != 0x303f) ||
        (c >= 0xac00 && c <= 0xd7a3) || (c >= 0xf900 && c <= 0xfaff) ||
        (c >= 0xfe10 && c <= 0xfe19) || (c >= 0xfe30 && c <= 0xfe6f) ||
        (c >= 0xff00 && c <= 0xff60) || (c >= 0xffe0 && c <= 0xffe6) ||
        (c >= 0x1f300 && c <= 0x1faff) || (c >= 0x20000 && c <= 0x3fffd));
}

int character_width(char32_t c) {
    int width = ::wcwidth(static_cast<wchar_t>(c));
    if (width >= 0) return width;
    if ((c >= 0x0300 && c <= 0x036f) || (c >= 0x1ab0 && c <= 0x1aff) ||
        (c >= 0x1dc0 && c <= 0x1dff) || (c >= 0x20d0 && c <= 0x20ff) ||
        (c >= 0xfe00 && c <= 0xfe0f) || (c >= 0xfe20 && c <= 0xfe2f) ||
        (c >= 0xe0100 && c <= 0xe01ef)) return 0;
    return wide_character(c) ? 2 : 1;
}

std::vector<Glyph> glyphs(const std::string& text) {
    Utf8Locale locale;
    std::vector<Glyph> output;
    std::size_t column = 0;
    for (std::size_t i = 0; i < text.size();) {
        const std::size_t begin = i;
        auto first = static_cast<unsigned char>(text[i++]);
        char32_t code = first;
        int extra = 0;
        if (first >= 0xc2 && first <= 0xdf) { code &= 0x1f; extra = 1; }
        else if (first >= 0xe0 && first <= 0xef) { code &= 0x0f; extra = 2; }
        else if (first >= 0xf0 && first <= 0xf4) { code &= 0x07; extra = 3; }
        else if (first >= 0x80) code = 0xfffd;
        bool valid = code != 0xfffd;
        if (extra) {
            if (i + extra > text.size()) valid = false;
            else for (int j = 0; j < extra; ++j) {
                auto byte = static_cast<unsigned char>(text[i + j]);
                if ((byte & 0xc0) != 0x80) { valid = false; break; }
                code = (code << 6) | (byte & 0x3f);
            }
            if (valid && ((extra == 1 && code < 0x80) || (extra == 2 && code < 0x800) ||
                (extra == 3 && code < 0x10000) || code > 0x10ffff ||
                (code >= 0xd800 && code <= 0xdfff))) valid = false;
            if (valid) i += static_cast<std::size_t>(extra);
        }
        if (!valid) {
            output.push_back({"\xef\xbf\xbd", 0xfffd, 1});
            ++column;
            continue;
        }
        if (code == '\t') {
            const int spaces = 4 - static_cast<int>(column % 4);
            for (int j = 0; j < spaces; ++j) output.push_back({" ", U' ', 1});
            column += spaces;
            continue;
        }
        // Do not permit remote metadata or lyrics to inject terminal controls.
        bool control = code < 0x20 || (code >= 0x7f && code <= 0x9f) ||
            code == 0x00ad || code == 0x061c || code == 0x180e ||
            (code >= 0x200b && code <= 0x200f) || (code >= 0x2028 && code <= 0x202e) ||
            (code >= 0x2060 && code <= 0x206f) || code == 0xfeff ||
            (code >= 0xfff9 && code <= 0xfffb) || (code >= 0xe0000 && code <= 0xe007f);
        if (control) output.push_back({" ", U' ', 1});
        else output.push_back({text.substr(begin, i - begin), code, character_width(code)});
        column = code == '\n' || code == '\r' ? 0 : column + 1;
    }
    return output;
}

int width_of(const std::vector<Glyph>& text, std::size_t begin, std::size_t end) {
    int width = 0;
    for (std::size_t i = begin; i < end; ++i) width += text[i].width;
    return width;
}

std::string join(const std::vector<Glyph>& text, std::size_t begin, std::size_t end) {
    std::string output;
    for (std::size_t i = begin; i < end; ++i) output += text[i].text;
    return output;
}

volatile std::sig_atomic_t stopping = 0;
volatile std::sig_atomic_t resized = 0;
void signal_handler(int signal) {
    if (signal == SIGWINCH) resized = 1;
    else stopping = signal;
}

class Signals {
public:
    Signals() {
        stopping = resized = 0;
        struct sigaction action{};
        action.sa_handler = signal_handler;
        sigemptyset(&action.sa_mask);
        for (int signal : {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGWINCH}) {
            struct sigaction old{};
            if (sigaction(signal, &action, &old) == -1) {
                restore();
                throw std::runtime_error("Cannot install terminal signal handlers.");
            }
            previous_.push_back({signal, old});
        }
    }
    ~Signals() { restore(); }
private:
    void restore() { for (auto& entry : previous_) sigaction(entry.first, &entry.second, nullptr); }
    std::vector<std::pair<int, struct sigaction>> previous_;
};

void write_terminal(const std::string& text) {
    std::size_t done = 0;
    while (done < text.size()) {
        ssize_t result = ::write(STDOUT_FILENO, text.data() + done, text.size() - done);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) throw std::runtime_error("Cannot write to the terminal.");
        done += static_cast<std::size_t>(result);
    }
}

class Terminal {
public:
    Terminal() {
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
            throw std::runtime_error("Live view requires an interactive terminal.");
        if (tcgetattr(STDIN_FILENO, &original_) == -1)
            throw std::runtime_error("Cannot read terminal settings.");
        termios raw = original_;
        raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
        raw.c_oflag &= ~OPOST;
        raw.c_cflag |= CS8;
        raw.c_lflag &= ~(ECHO | ICANON | IEXTEN);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == -1)
            throw std::runtime_error("Cannot set terminal input mode.");
        active_ = true;
        try { write_terminal("\x1b[?1049h\x1b[?25l"); }
        catch (...) { restore(); throw; }
        const char* term = std::getenv("TERM");
        color_ = std::getenv("NO_COLOR") == nullptr && term && std::string(term) != "dumb";
        colors256_ = color_ && (std::string(term).find("256color") != std::string::npos ||
            std::getenv("COLORTERM") != nullptr);
    }
    ~Terminal() { restore(); }
    std::pair<int, int> dimensions() const {
        winsize size{};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == -1 || !size.ws_col || !size.ws_row)
            return {24, 80};
        return {size.ws_row, size.ws_col};
    }
    bool color() const { return color_; }
    bool colors256() const { return colors256_; }
private:
    void restore() noexcept {
        if (!active_) return;
        static constexpr char sequence[] = "\x1b[0m\x1b[?25h\x1b[?1049l";
        // A destructor must restore input settings even if output has failed.
        std::size_t done = 0;
        while (done < sizeof(sequence) - 1) {
            ssize_t count = ::write(STDOUT_FILENO, sequence + done, sizeof(sequence) - 1 - done);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) break;
            done += static_cast<std::size_t>(count);
        }
        while (tcsetattr(STDIN_FILENO, TCSANOW, &original_) == -1 && errno == EINTR) {}
        active_ = false;
    }
    termios original_{};
    bool active_ = false, color_ = false, colors256_ = false;
};

enum Key { Up = 1000, Down, Left, Right, PageUp, PageDown, Home, End, BackTab };

class Keyboard {
public:
    int next(int timeout_ms) {
        if (pending_.empty()) read_input(timeout_ms);
        if (pending_.empty()) return -1;
        if (static_cast<unsigned char>(pending_.front()) != 0x1b) {
            int key = static_cast<unsigned char>(pending_.front());
            pending_.erase(0, 1);
            return key;
        }
        if (pending_.size() == 1 || ((pending_[1] == '[' || pending_[1] == 'O') && pending_.size() < 3))
            read_input(15);
        if (pending_.size() == 1) { pending_.erase(0, 1); return 27; }
        if (pending_[1] != '[' && pending_[1] != 'O') { pending_.erase(0, 1); return 27; }
        std::size_t end = 2;
        while (end < pending_.size() && !(pending_[end] >= '@' && pending_[end] <= '~')) ++end;
        if (end == pending_.size()) {
            read_input(15);
            while (end < pending_.size() && !(pending_[end] >= '@' && pending_[end] <= '~')) ++end;
        }
        if (end == pending_.size()) { pending_.clear(); return -1; }
        std::string sequence = pending_.substr(0, end + 1);
        pending_.erase(0, end + 1);
        switch (sequence.back()) {
            case 'A': return Up;
            case 'B': return Down;
            case 'C': return Right;
            case 'D': return Left;
            case 'H': return Home;
            case 'F': return End;
            case 'Z': return BackTab;
            case '~': {
                std::string number = sequence.substr(2, sequence.size() - 3);
                auto separator = number.find(';');
                if (separator != std::string::npos) number.resize(separator);
                if (number == "1" || number == "7") return Home;
                if (number == "4" || number == "8") return End;
                if (number == "5") return PageUp;
                if (number == "6") return PageDown;
            }
        }
        return -1;
    }
private:
    void read_input(int timeout_ms) {
        pollfd input{STDIN_FILENO, POLLIN, 0};
        int result = ::poll(&input, 1, timeout_ms);
        if (result < 0 && errno != EINTR) throw std::runtime_error("Cannot poll terminal input.");
        if (result > 0 && (input.revents & (POLLHUP | POLLERR | POLLNVAL))) {
            stopping = SIGHUP;
            return;
        }
        if (result > 0 && (input.revents & POLLIN)) {
            char bytes[128];
            ssize_t count = ::read(STDIN_FILENO, bytes, sizeof(bytes));
            if (count > 0) pending_.append(bytes, static_cast<std::size_t>(count));
            else if (count < 0 && errno != EINTR && errno != EAGAIN)
                throw std::runtime_error("Cannot read terminal input.");
        }
    }
    std::string pending_;
};

enum Style { Text, Muted, Artist, ArtistEnd, Pill, Selected, Current, Rule };

class Canvas {
public:
    explicit Canvas(const Terminal& terminal) : terminal_(terminal) {
        auto size = terminal.dimensions();
        height = size.first;
        width = size.second;
        output_ = style(Text) + "\x1b[2J\x1b[H";
    }
    void put(int row, int column, const std::string& text, Style kind = Text, bool bold = false) {
        const int available = width - column - (row == height - 1 ? 1 : 0);
        if (row < 0 || row >= height || column < 0 || column >= width || available < 1) return;
        output_ += "\x1b[" + std::to_string(row + 1) + ";" + std::to_string(column + 1) + "H";
        output_ += style(kind);
        if (bold) output_ += "\x1b[1m";
        output_ += clip_text(text, available);
    }
    void finish() { output_ += "\x1b[0m"; write_terminal(output_); }
    int height, width;
private:
    std::string style(Style kind) const {
        std::string result = "\x1b[0m";
        if (!terminal_.color()) {
            if (kind == Selected || kind == Current) result += "\x1b[7m";
            return result;
        }
        if (terminal_.colors256()) {
            static constexpr int foreground[] = {255, 245, 140, 175, 252, 255, 255, 241};
            const int background = kind == Pill ? 239 : kind == Current ? 242 : 235;
            return result + "\x1b[38;5;" + std::to_string(foreground[kind]) + "m\x1b[48;5;" +
                std::to_string(background) + "m";
        }
        if (kind == Current || kind == Selected) return result + "\x1b[30;47m";
        if (kind == Artist || kind == ArtistEnd) return result + "\x1b[35;40m";
        return result + "\x1b[37;40m";
    }
    const Terminal& terminal_;
    std::string output_;
};

std::string time_label(std::optional<double> position) {
    if (!position || !std::isfinite(*position) || *position < 0) return "--:--";
    const double minutes = std::floor(*position / 60);
    char label[64];
    std::snprintf(label, sizeof(label), "%02.0f:%02d", minutes, static_cast<int>(std::fmod(*position, 60)));
    return label;
}

std::pair<int, int> draw(const Terminal& terminal, const Track& track, const LyricsResult& lyrics,
        std::optional<double> position, const std::string& status, const std::string& message,
        bool overview, int scroll, const std::string& notice, double offset) {
    Canvas canvas(terminal);
    if (position && (!std::isfinite(*position) || *position < 0)) position.reset();
    if (canvas.height < 8 || canvas.width < 30) {
        canvas.put(0, 0, "Please make the terminal larger (at least 30x8).");
        canvas.finish();
        return {0, 0};
    }
    int padding = canvas.width >= 70 ? 4 : 2;
    int width = std::min(100, canvas.width - padding * 2);
    int left = (canvas.width - width) / 2;
    bool compact = canvas.height < 18;
    int top = compact ? 0 : 1;
    auto title = wrap_text(track.title, width);
    int title_limit = canvas.height >= 24 ? 2 : 1;
    int title_count = std::min(static_cast<int>(title.size()), title_limit);
    for (int i = 0; i < title_count; ++i) {
        std::string line = title[i];
        if (i == title_limit - 1 && static_cast<int>(title.size()) > title_limit)
            line = clip_text(line + " …", width, true);
        canvas.put(top + i, left, line, Text, true);
    }
    int artist_row = top + title_count + (canvas.height >= 24 ? 1 : 0);
    std::string artist = clip_text("Song by " + (track.artist.empty() ? "Unknown artist" : track.artist), width, true);
    auto artist_glyphs = glyphs(artist);
    std::size_t split = 0;
    int artist_half = std::max(8, display_width(artist) / 2), used = 0;
    for (std::size_t i = 0; i < artist_glyphs.size(); ++i) {
        if (used >= artist_half) break;
        if (artist_glyphs[i].code == U' ') split = i + 1;
        used += artist_glyphs[i].width;
    }
    auto first = join(artist_glyphs, 0, split);
    canvas.put(artist_row, left, first, Artist);
    canvas.put(artist_row, left + display_width(first), join(artist_glyphs, split, artist_glyphs.size()), ArtistEnd);
    int tab_row = artist_row + (compact ? 1 : 2);
    bool tall_tabs = canvas.height >= 26;
    int column = left;
    for (bool tab_overview : {true, false}) {
        bool selected = overview == tab_overview;
        std::string label = tab_overview ? " Overview " : " Lyrics ";
        Style kind = selected ? Selected : Pill;
        if (tall_tabs) {
            std::string rule;
            for (std::size_t i = 0; i < label.size(); ++i) rule += "─";
            canvas.put(tab_row, column, "╭" + rule + "╮", kind, selected);
            canvas.put(tab_row + 1, column, "│" + label + "│", kind, selected);
            canvas.put(tab_row + 2, column, "╰" + rule + "╯", kind, selected);
        } else canvas.put(tab_row, column, selected ? "(" + label + ")" : " " + label + " ", kind, selected);
        column += static_cast<int>(label.size()) + 4;
    }
    int divider_row = tab_row + (tall_tabs ? 3 : 1) + (compact ? 0 : 1);
    std::string rule;
    for (int i = 0; i < width; ++i) rule += "─";
    canvas.put(divider_row, left, rule, Rule);
    int body_row = divider_row + (canvas.height >= 24 ? 2 : 1);
    int status_row = canvas.height - (canvas.height >= 14 ? 3 : 2);
    int available = std::max(1, status_row - body_row - (compact ? 0 : 1));
    int focused = !lyrics.synced.empty() && position ? active_line(lyrics.synced, *position + offset) : -1;
    std::vector<std::pair<int, std::string>> rows;
    auto append = [&](int index, const std::string& text) {
        for (const auto& line : wrap_text(text, width)) rows.push_back({index, line});
    };
    if (overview) {
        append(-1, "Now playing"); append(-1, "");
        append(-1, "Status    " + (status.empty() ? "Stopped" : status));
        append(-1, "Position  " + time_label(position));
        append(-1, "Player    " + (track.player.empty() ? "MPRIS" : track.player));
        append(-1, "Album     " + (track.album.empty() ? "Unknown album" : track.album));
        append(-1, "Lyrics    " + std::to_string(lyrics.synced.empty() ? lyrics.plain.size() : lyrics.synced.size()) +
            (lyrics.synced.empty() ? " plain text lines" : " synchronized lines"));
        if (!message.empty()) { append(-1, ""); append(-1, message); }
    } else if (!message.empty() || !lyrics.has_lyrics()) {
        append(-1, message.empty() ? "No lyrics to display." : message);
    } else if (!lyrics.synced.empty()) {
        for (std::size_t i = 0; i < lyrics.synced.size(); ++i) append(static_cast<int>(i), lyrics.synced[i].second);
    } else {
        for (std::size_t i = 0; i < lyrics.plain.size(); ++i) append(static_cast<int>(i), lyrics.plain[i]);
    }
    int maximum = std::max(0, static_cast<int>(rows.size()) - available);
    int focus_row = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) if (rows[i].first == focused) { focus_row = static_cast<int>(i); break; }
    int automatic = !lyrics.synced.empty() && !overview ? std::clamp(focus_row - available / 3, 0, maximum) : 0;
    int start = std::clamp(scroll < 0 ? automatic : scroll, 0, maximum);
    for (int row = 0; row < available && start + row < static_cast<int>(rows.size()); ++row) {
        const auto& line = rows[start + row];
        bool current = !overview && !lyrics.synced.empty() && focused >= 0 && line.first == focused;
        if (current && !line.second.empty()) canvas.put(body_row + row, left - 1, " " + line.second + " ", Current, true);
        else canvas.put(body_row + row, left, line.second);
    }
    std::string playback = (status.empty() ? "Stopped" : status) + "  ·  " + time_label(position) +
        "  ·  " + (track.player.empty() ? "MPRIS" : track.player);
    canvas.put(status_row, left, clip_text(notice.empty() ? playback : notice, width, true), Muted);
    std::string mode = !lyrics.synced.empty() ? "Synced lyrics" : !lyrics.plain.empty() ? "Plain lyrics" : "Waiting for lyrics";
    if (!lyrics.synced.empty() && !position) mode = "Playback time unavailable";
    else if (!lyrics.synced.empty() && offset != 0) {
        char label[96]; std::snprintf(label, sizeof(label), "Lyrics %+.2fs", offset); mode = label;
    }
    if (scroll >= 0) mode = std::to_string(start + 1) + "–" + std::to_string(std::min(start + available, static_cast<int>(rows.size()))) + " / " + std::to_string(rows.size());
    if (notice.empty() && display_width(playback) + display_width(mode) + 4 <= width)
        canvas.put(status_row, left + width - display_width(mode), mode, Muted);
    std::string footer = "Space pause  n/b tracks  ↑↓ scroll  Tab view  f follow  [/] sync  q quit";
    if (display_width(footer) > width) footer = "Space pause  ↑↓ scroll  Tab view  q quit";
    if (display_width(footer) > width) footer = "Space pause  Tab  q quit";
    canvas.put(canvas.height - 1, left, clip_text(footer, width), Muted);
    canvas.finish();
    return {start, maximum};
}
} // namespace

int display_width(const std::string& text) {
    auto clean = glyphs(text);
    return width_of(clean, 0, clean.size());
}

std::string clip_text(const std::string& text, int width, bool ellipsis) {
    if (width < 1) return {};
    auto clean = glyphs(text);
    if (width_of(clean, 0, clean.size()) <= width) return join(clean, 0, clean.size());
    int available = width - (ellipsis ? 1 : 0), used = 0;
    std::size_t end = 0;
    while (end < clean.size() && used + clean[end].width <= available) used += clean[end++].width;
    return join(clean, 0, end) + (ellipsis ? "…" : "");
}

std::vector<std::string> wrap_text(const std::string& text, int width) {
    if (width < 1) return {""};
    auto clean = glyphs(text);
    std::size_t begin = 0, end = clean.size();
    while (begin < end && clean[begin].code == U' ') ++begin;
    while (end > begin && clean[end - 1].code == U' ') --end;
    if (begin == end) return {""};
    std::vector<std::string> lines;
    while (begin < end) {
        std::size_t stop = begin, space = begin;
        int used = 0;
        while (stop < end && used + clean[stop].width <= width) {
            if (clean[stop].code == U' ') space = stop;
            used += clean[stop++].width;
        }
        if (stop == end) { lines.push_back(join(clean, begin, end)); break; }
        if (space > begin) stop = space;
        if (stop == begin) { lines.push_back("?"); ++begin; }
        else { lines.push_back(join(clean, begin, stop)); begin = stop; }
        while (begin < end && clean[begin].code == U' ') ++begin;
    }
    return lines;
}

std::string playback_command(int key) {
    if (key == ' ') return "play-pause";
    if (key == 'n' || key == 'N') return "next";
    if (key == 'b' || key == 'B') return "previous";
    return {};
}

int live(const Options& options) {
    Signals signals;
    Terminal terminal;
    Keyboard keyboard;
    LyricsLoader loader;
    PlaybackClock clock;
    Track track;
    track.title = "Waiting for music";
    track.artist = "Play a song in Spotify, VLC, or another MPRIS player";
    LyricsResult lyrics;
    std::string last_key, status, message = "Waiting for a song...", playback_error, notice;
    // A negative row selects automatic following; nonnegative rows are manual.
    int scroll = -1;
    int viewport_start = 0, viewport_maximum = 0;
    bool overview = false, initial = true;
    double offset = options.offset, next_poll = 0, notice_until = 0;
    std::optional<long long> last_second;
    int last_active = -2;
    while (!stopping) {
        const double now = monotonic_seconds();
        int timeout = initial ? 0 : static_cast<int>(std::clamp((next_poll - now) * 1000.0, 0.0, 50.0));
        int key = keyboard.next(timeout);
        if (stopping || key == 'q' || key == 'Q' || key == 3) break;
        bool redraw = initial || key != -1 || resized;
        resized = 0;
        initial = false;
        if (key == '\t' || key == BackTab || key == Left || key == Right) { overview = !overview; scroll = -1; }
        else if (key == '1' || key == '2') { overview = key == '1'; scroll = -1; }
        else if (key == Up || key == PageUp) scroll = std::max(0, viewport_start - (key == Up ? 1 : 5));
        else if (key == Down || key == PageDown) scroll = std::min(viewport_maximum, viewport_start + (key == Down ? 1 : 5));
        else if (key == Home) scroll = 0;
        else if (key == End) scroll = viewport_maximum;
        else if (key == 'f' || key == 'F') scroll = -1;
        else if (key == ']' || key == '+' || key == '=') {
            offset += 0.25;
            if (std::abs(offset) < 1e12) offset = std::round(offset * 100) / 100;
        } else if (key == '[' || key == '-' || key == '_') {
            offset -= 0.25;
            if (std::abs(offset) < 1e12) offset = std::round(offset * 100) / 100;
        }
        else if (key == '0') offset = 0;
        std::string command = playback_command(key);
        if (!command.empty()) {
            try { run_playerctl({command}, options.player); notice.clear(); }
            catch (const std::exception& error) { notice = "Playback control failed: " + std::string(error.what()); notice_until = monotonic_seconds() + 4; }
        }
        if (!command.empty() || monotonic_seconds() >= next_poll) {
            try {
                double started = monotonic_seconds();
                auto sample = current_track(options.player);
                double sampled_at = (started + monotonic_seconds()) / 2;
                if (sample.track.player != track.player || sample.track.track_id != track.track_id) scroll = -1;
                clock.update(sample.track, sample.position, sample.status, sampled_at);
                status = sample.status;
                playback_error.clear();
                track = sample.track;
                if (track.lyrics_key() != last_key) {
                    lyrics = {};
                    message = "Loading lyrics...";
                    loader.request(track);
                    last_key = track.lyrics_key();
                    scroll = -1;
                }
            } catch (const std::exception& error) { playback_error = error.what(); }
            next_poll = monotonic_seconds() + options.refresh;
            redraw = true;
        }
        if (auto result = loader.poll(track)) { lyrics = std::move(*result); message = lyrics.message; redraw = true; }
        auto position = clock.position();
        std::optional<long long> second;
        if (position && std::isfinite(*position) && *position >= 0 && *position < 9e18) second = static_cast<long long>(*position);
        int focused = !lyrics.synced.empty() && position ? active_line(lyrics.synced, *position + offset) : -1;
        if (second != last_second || focused != last_active) { redraw = true; last_second = second; last_active = focused; }
        if (!notice.empty() && monotonic_seconds() >= notice_until) { notice.clear(); redraw = true; }
        if (redraw) {
            auto viewport = draw(terminal, track, lyrics, position, status, playback_error.empty() ? message : playback_error,
                overview, scroll, notice, offset);
            viewport_start = viewport.first;
            viewport_maximum = viewport.second;
        }
    }
    loader.close();
    return stopping ? 128 + stopping : 0;
}
} // namespace tl
