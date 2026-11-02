#include "terminal_lyrics/process.hpp"
#include "terminal_lyrics/ui.hpp"
#include <clocale>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace {
void help() {
    std::cout << "Terminal Lyrics — C++17 Linux lyrics viewer\n\n"
              << "Usage: terminal-lyrics [--player NAME] [--refresh SECONDS] [--offset SECONDS]\n\n"
              << "  --player NAME      Choose an MPRIS player, e.g. brave, spotify, or vlc\n"
              << "  --refresh SECONDS  Playback polling interval (default 0.5; minimum 0.1)\n"
              << "  --offset SECONDS   Positive values advance the lyrics (default 0)\n"
              << "  -h, --help         Show this help\n\n"
              << "Keys: q quit, Space play/pause, n/b next/previous, Tab switch view,\n"
              << "      arrows scroll, f follow, ]/[ adjust timing, 0 reset timing.\n";
}

double number(const std::string& option, const std::string& value) {
    try {
        std::size_t end = 0;
        const double parsed = std::stod(value, &end);
        if (end != value.size() || !std::isfinite(parsed)) throw std::invalid_argument("number");
        return parsed;
    } catch (const std::exception&) {
        throw std::runtime_error(option + " requires a finite number of seconds.");
    }
}
}

int main(int argc, char** argv) {
    std::setlocale(LC_CTYPE, "");
    tl::Options options;
    try {
        for (int index = 1; index < argc; ++index) {
            std::string argument = argv[index];
            if (argument == "--help" || argument == "-h") { help(); return 0; }
            const auto equals = argument.find('=');
            const std::string name = argument.substr(0, equals);
            if (name != "--player" && name != "--refresh" && name != "--offset")
                throw std::runtime_error("Unknown option: " + argument + ". Use --help for usage.");
            std::string value;
            if (equals != std::string::npos) value = argument.substr(equals + 1);
            else if (++index < argc) value = argv[index];
            else throw std::runtime_error(name + " requires a value.");
            if (name == "--player") {
                if (value.empty()) throw std::runtime_error("--player requires a nonempty player name.");
                options.player = value;
            } else if (name == "--refresh") options.refresh = number(name, value);
            else options.offset = number(name, value);
        }
        if (options.refresh < 0.1) throw std::runtime_error("--refresh must be at least 0.1 seconds.");
        for (const auto* dependency : {"playerctl", "curl"}) {
            if (!tl::command_exists(dependency))
                throw std::runtime_error(std::string(dependency) + " is missing. Install it first; see README.md.");
        }
        if (!::isatty(STDIN_FILENO) || !::isatty(STDOUT_FILENO))
            throw std::runtime_error("Run Terminal Lyrics in an interactive terminal.");
        return tl::live(options);
    } catch (const std::exception& error) {
        std::cerr << "terminal-lyrics: " << error.what() << '\n';
        return 1;
    }
}
