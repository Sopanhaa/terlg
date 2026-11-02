#include "terminal_lyrics/ui.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    try {
        expect(tl::display_width("") == 0, "empty text width");
        expect(tl::display_width("hello") == 5, "ASCII text width");
        expect(tl::display_width("世界") == 4, "CJK characters occupy two cells");
        expect(tl::display_width("e\xcc\x81") == 1, "combining accents occupy no cells");
        expect(tl::display_width("🎵") == 2, "emoji occupies two cells");
        expect(tl::clip_text("世界abc", 3) == "世", "clip does not split wide characters");
        expect(tl::clip_text("世界abc", 3, true) == "世…", "ellipsis reserves one terminal cell");
        expect(tl::clip_text("hello", 1, true) == "…", "one-cell ellipsis");
        expect(tl::clip_text("hello", 0).empty(), "zero-width clip");
        expect(tl::clip_text("hello", -5, true).empty(), "negative-width clip");
        expect(tl::clip_text("e\xcc\x81x", 1) == "e\xcc\x81", "clip preserves combining accent");
        expect(tl::clip_text("a\tb", 20) == "a   b", "tabs expand to four-column stops");
        expect(tl::clip_text("a\n\rb", 20) == "a  b", "newlines cannot move the cursor");
        expect(tl::clip_text("\x1b[31mred", 40).find('\x1b') == std::string::npos,
            "ANSI escape injection is sanitized");
        expect(tl::clip_text(std::string("a\0b", 3), 10) == "a b", "NUL is sanitized");
        expect(tl::clip_text("a\xc2\x9b" "31m", 20) == "a 31m", "C1 CSI injection is sanitized");
        expect(tl::clip_text("a\xe2\x80\xae" "b", 20).find("\xe2\x80\xae") == std::string::npos,
            "bidirectional override is sanitized");
        expect(tl::clip_text("\xffx", 20) == "\xef\xbf\xbdx", "invalid UTF-8 is replaced");
        expect(tl::clip_text("\xe4\xb8", 20) == "\xef\xbf\xbd\xef\xbf\xbd", "truncated UTF-8 is replaced");
        expect(tl::wrap_text("one two three", 7) == std::vector<std::string>({"one", "two", "three"}),
            "wrapping preserves complete words");
        expect(tl::wrap_text("abcdefgh", 3) == std::vector<std::string>({"abc", "def", "gh"}),
            "long words wrap without losing letters");
        expect(tl::wrap_text("世界世界", 4) == std::vector<std::string>({"世界", "世界"}),
            "CJK wrapping respects cell widths");
        expect(tl::wrap_text("世", 1) == std::vector<std::string>({"?"}),
            "too-wide glyph cannot cause a wrapping loop");
        expect(tl::wrap_text("  ", 8) == std::vector<std::string>({""}), "blank stanza line is preserved");
        expect(tl::wrap_text("text", 0) == std::vector<std::string>({""}), "zero-width wrapping");
        for (const auto& line : tl::wrap_text("世界 e\xcc\x81 sample lyrics 🎵", 8))
            expect(tl::display_width(line) <= 8, "wrapped Unicode line fits terminal");
        expect(tl::playback_command(' ') == "play-pause", "space toggles playback");
        expect(tl::playback_command('n') == "next" && tl::playback_command('N') == "next", "next track shortcuts");
        expect(tl::playback_command('b') == "previous" && tl::playback_command('B') == "previous", "previous track shortcuts");
        for (int key : std::vector<int>{'x', 'p', 'q', '\t', '[', ']', 1000})
            expect(tl::playback_command(key).empty(), "unrelated keys cannot control playback");
        std::cout << "UI helper tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "UI test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
