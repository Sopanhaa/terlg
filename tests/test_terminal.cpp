#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>

namespace {
using Clock = std::chrono::steady_clock;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

class Fixture {
public:
    Fixture() {
        char name[] = "/tmp/terminal-lyrics-pty-XXXXXX";
        char* result = ::mkdtemp(name);
        if (!result) throw std::runtime_error("Cannot create terminal test fixtures");
        directory = result;
        command_log = directory / "commands";
        try {
            script("playerctl",
                "#!/bin/sh\n"
                "if [ \"${1-}\" = '--player' ]; then\n"
                "  [ \"${2-}\" = 'fixture' ] || exit 9\n"
                "  shift 2\n"
                "fi\n"
                "case \"${1-}\" in\n"
                "  metadata)\n"
                "    [ \"${2-}\" = '--format' ] || exit 8\n"
                "    position=1500000; duration=120000000\n"
                "    if [ \"${TL_TEST_EXTREME-}\" = 1 ]; then position=1e308; duration=1e308; fi\n"
                "    printf '%s\\037%s\\037%s\\037%s\\037%s\\037%s\\037%s\\037%s\\n' "
                "'Fixture Song' 'Fixture Artist' 'Fixture Album' 'fixture' 'Playing' "
                "\"$position\" \"$duration\" '/fixture/track'\n"
                "    ;;\n"
                "  play-pause|next|previous) printf '%s\\n' \"$1\" >> \"$TL_TEST_COMMAND_LOG\" ;;\n"
                "  *) exit 7 ;;\n"
                "esac\n");
            std::string synced;
            for (int line = 0; line < 40; ++line) {
                const std::string number = (line < 10 ? "0" : "") + std::to_string(line);
                if (line) synced += "\\n";
                synced += "[00:" + number + ".00]Fixture lyric " + number;
            }
            const std::string json = "[{\"id\":1,\"trackName\":\"Fixture Song\","
                "\"artistName\":\"Fixture Artist\",\"albumName\":\"Fixture Album\","
                "\"duration\":120,\"syncedLyrics\":\"" + synced + "\"}]";
            script("curl", "#!/bin/sh\nprintf '%s\\n200' '" + json + "'\n");
        } catch (...) {
            std::filesystem::remove_all(directory);
            throw;
        }
    }
    ~Fixture() {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
    std::filesystem::path directory, command_log;
private:
    void script(const std::string& name, const std::string& text) {
        const auto path = directory / name;
        std::ofstream output(path);
        output << text;
        output.close();
        require(static_cast<bool>(output), "Cannot write terminal fixture " + name);
        require(::chmod(path.c_str(), 0700) == 0, "Cannot make terminal fixture executable");
    }
};

class Session {
public:
    Session(const std::string& binary, const Fixture& fixture, bool monochrome = false, bool extreme = false) {
        try {
            master_ = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
            require(master_ >= 0, "Cannot open a pseudo-terminal: " + std::string(std::strerror(errno)));
            require(::grantpt(master_) == 0 && ::unlockpt(master_) == 0, "Cannot initialize pseudo-terminal");
            const char* name = ::ptsname(master_);
            require(name != nullptr, "Cannot find pseudo-terminal slave");
            slave_ = ::open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
            require(slave_ >= 0, "Cannot open pseudo-terminal slave");
            require(::tcgetattr(slave_, &original_) == 0, "Cannot read pseudo-terminal attributes");
            original_.c_iflag |= ICRNL;
            original_.c_oflag |= OPOST;
            original_.c_lflag |= ECHO | ICANON | ISIG;
            original_.c_cc[VINTR] = 3;
            original_.c_cc[VMIN] = 1;
            original_.c_cc[VTIME] = 0;
            require(::tcsetattr(slave_, TCSANOW, &original_) == 0, "Cannot set pseudo-terminal attributes");
            resize(24, 120);
            const char* path = std::getenv("PATH");
            const std::string child_path = fixture.directory.string() + ":" + (path ? path : "/usr/bin:/bin");
            child_ = ::fork();
            require(child_ >= 0, "Cannot fork terminal app");
            if (child_ == 0) {
                if (::setsid() == -1 || ::ioctl(slave_, TIOCSCTTY, 0) == -1 ||
                    ::tcsetpgrp(slave_, ::getpgrp()) == -1) ::_exit(120);
                for (int descriptor : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) {
                    if (::dup2(slave_, descriptor) == -1) ::_exit(121);
                }
                ::close(master_);
                if (slave_ > STDERR_FILENO) ::close(slave_);
                ::setenv("PATH", child_path.c_str(), 1);
                ::setenv("TERM", "xterm-256color", 1);
                ::setenv("LC_ALL", "C.UTF-8", 1);
                ::setenv("TL_TEST_COMMAND_LOG", fixture.command_log.c_str(), 1);
                if (extreme) ::setenv("TL_TEST_EXTREME", "1", 1);
                else ::unsetenv("TL_TEST_EXTREME");
                if (monochrome) ::setenv("NO_COLOR", "1", 1);
                else ::unsetenv("NO_COLOR");
                ::execl(binary.c_str(), binary.c_str(), "--player", "fixture", "--refresh", extreme ? "1e308" : "0.1",
                        "--offset", extreme ? "1e308" : "0",
                        static_cast<char*>(nullptr));
                ::_exit(122);
            }
        } catch (...) {
            cleanup();
            throw;
        }
    }
    ~Session() { cleanup(); }

    void send(const std::string& keys) {
        std::size_t written = 0;
        const auto deadline = Clock::now() + std::chrono::seconds(2);
        while (written < keys.size()) {
            const ssize_t count = ::write(master_, keys.data() + written, keys.size() - written);
            if (count > 0) written += static_cast<std::size_t>(count);
            else if (count < 0 && (errno == EAGAIN || errno == EINTR)) {
                require(Clock::now() < deadline, "Timed out writing terminal keys");
                pump();
            } else throw std::runtime_error("Cannot write terminal keys");
        }
    }

    void expect(const std::string& text, std::size_t after = 0) {
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        while (output.find(text, after) == std::string::npos) {
            require(Clock::now() < deadline, "Timed out waiting for terminal text: " + text);
            pump();
            require(!exited_, "Terminal app exited before displaying: " + text);
        }
    }

    void key_expect(const std::string& keys, const std::string& text) {
        const auto after = output.size();
        send(keys);
        expect(text, after);
    }

    void resize(unsigned short rows, unsigned short columns) {
        winsize dimensions{};
        dimensions.ws_row = rows;
        dimensions.ws_col = columns;
        require(::ioctl(master_, TIOCSWINSZ, &dimensions) == 0, "Cannot resize pseudo-terminal");
    }

    void signal(int number) { require(::kill(child_, number) == 0, "Cannot signal terminal app"); }

    void wait_exit(int expected) {
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        while (!exited_) {
            require(Clock::now() < deadline, "Terminal app did not quit promptly");
            pump();
        }
        pump();
        require(WIFEXITED(status_) && WEXITSTATUS(status_) == expected,
                "Unexpected terminal app exit status: " + std::to_string(status_));
        require(output.find("\x1b[?1049h") != std::string::npos &&
                output.find("\x1b[?25l") != std::string::npos, "App did not enter terminal display mode");
        require(output.find("\x1b[?25h") != std::string::npos &&
                output.find("\x1b[?1049l") != std::string::npos, "App did not restore cursor/alternate screen");
        termios restored{};
        require(::tcgetattr(slave_, &restored) == 0, "Cannot inspect restored terminal attributes");
        require(restored.c_iflag == original_.c_iflag && restored.c_oflag == original_.c_oflag &&
                restored.c_cflag == original_.c_cflag && restored.c_lflag == original_.c_lflag &&
                std::equal(std::begin(restored.c_cc), std::end(restored.c_cc), std::begin(original_.c_cc)) &&
                ::cfgetispeed(&restored) == ::cfgetispeed(&original_) &&
                ::cfgetospeed(&restored) == ::cfgetospeed(&original_), "Terminal input settings were not restored");
    }

    void require_raw() const {
        termios current{};
        require(::tcgetattr(slave_, &current) == 0 && !(current.c_lflag & (ICANON | ECHO)) &&
                !(current.c_oflag & OPOST), "App did not enable immediate unechoed input");
    }

    void expect_commands(const Fixture& fixture) {
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        for (;;) {
            const auto commands = read_file(fixture.command_log);
            if (commands.find("play-pause\nnext\nprevious\n") != std::string::npos) return;
            require(Clock::now() < deadline, "Playback shortcuts did not reach playerctl");
            pump();
        }
    }

    std::string output;
private:
    void pump() {
        pollfd descriptor{master_, POLLIN, 0};
        const int result = ::poll(&descriptor, 1, 20);
        require(result >= 0 || errno == EINTR, "Cannot poll terminal output");
        for (;;) {
            char buffer[16384];
            const ssize_t count = ::read(master_, buffer, sizeof(buffer));
            if (count > 0) {
                output.append(buffer, static_cast<std::size_t>(count));
                require(output.size() <= 4 * 1024 * 1024, "Terminal app produced excessive output");
            } else if (count < 0 && errno == EINTR) continue;
            else {
                require(count == 0 || errno == EAGAIN || errno == EIO, "Cannot read terminal output");
                break;
            }
        }
        if (!exited_) {
            const pid_t result = ::waitpid(child_, &status_, WNOHANG);
            require(result >= 0 || errno == EINTR, "Cannot inspect terminal child status");
            if (result == child_) exited_ = true;
        }
    }

    void cleanup() noexcept {
        if (child_ > 0 && !exited_) {
            ::kill(child_, SIGKILL);
            while (::waitpid(child_, &status_, 0) < 0 && errno == EINTR) {}
        }
        if (slave_ >= 0) ::close(slave_);
        if (master_ >= 0) ::close(master_);
    }

    int master_ = -1, slave_ = -1, status_ = 0;
    pid_t child_ = -1;
    bool exited_ = false;
    termios original_{};
};
}

int main() {
    try {
        const std::string binary = std::filesystem::absolute("build/terminal-lyrics").string();
        require(::access(binary.c_str(), X_OK) == 0, "Build terminal-lyrics before running terminal tests");
        Fixture fixture;
        {
            Session session(binary, fixture);
            session.expect("Fixture lyric 00");
            session.expect("Fixture Song");
            // The artist is drawn in two color segments, so inspect the stable suffix.
            session.expect("Fixture Artist");
            session.require_raw();
            session.key_expect("\t", "Now playing");
            session.expect("Fixture Album");
            session.key_expect("\x1b[Z", "Fixture lyric 00");
            session.key_expect("\x1b[6~", " / 40");
            session.key_expect("\x1b[F", "Fixture lyric 39");
            session.key_expect("\x1b[H", "Fixture lyric 00");
            session.key_expect("\x1b[B", " / 40");
            session.key_expect("\x1b[C", "Now playing");
            session.key_expect("\x1b[D", "Fixture lyric");
            session.key_expect("f+", "Lyrics +0.25s");
            session.key_expect("-", "Synced lyrics");
            session.key_expect("]", "Lyrics +0.25s");
            session.key_expect("[", "Synced lyrics");
            session.key_expect("=", "Lyrics +0.25s");
            session.key_expect("0", "Synced lyrics");
            session.send(" nb");
            session.expect_commands(fixture);
            auto after = session.output.size();
            session.resize(6, 20);
            session.expect("Please make", after);
            after = session.output.size();
            session.resize(28, 120);
            session.expect("Fixture lyric", after);
            session.send("q");
            session.wait_exit(0);
        }
        {
            Session session(binary, fixture, true);
            session.expect("Fixture lyric 01");
            require(session.output.find("\x1b[7m") != std::string::npos,
                    "Monochrome mode must visibly select the active lyric");
            session.send(std::string(1, '\x03'));
            session.wait_exit(128 + SIGINT);
        }
        {
            Session session(binary, fixture);
            session.expect("Fixture lyric 00");
            session.signal(SIGTERM);
            session.wait_exit(128 + SIGTERM);
        }
        {
            Session session(binary, fixture, false, true);
            session.expect("Fixture lyric 39");
            session.key_expect("+", "Fixture lyric 39");
            session.key_expect("0", "Fixture lyric 39");
            session.send("q");
            session.wait_exit(0);
        }
        std::cout << "Offline terminal loading, keyboard, resize, and signal cleanup tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Terminal integration test failed: " << error.what() << '\n';
        return 1;
    }
}
