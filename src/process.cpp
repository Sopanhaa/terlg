#include "terminal_lyrics/process.hpp"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace tl {
namespace {
class Pipe {
public:
    int read_fd = -1, write_fd = -1;
    Pipe() {
        int fds[2];
        if (pipe2(fds, O_CLOEXEC) != 0)
            throw std::runtime_error(std::string("Could not create a subprocess pipe: ") + std::strerror(errno));
        read_fd = fds[0];
        write_fd = fds[1];
    }
    ~Pipe() { close_read(); close_write(); }
    void close_read() { if (read_fd >= 0) { ::close(read_fd); read_fd = -1; } }
    void close_write() { if (write_fd >= 0) { ::close(write_fd); write_fd = -1; } }
    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;
};

bool read_available(Pipe& pipe, std::string& text, std::size_t limit) {
    if (pipe.read_fd < 0) return true;
    char buffer[8192];
    // Bound each pass so a continuously writing process cannot starve cancellation.
    for (int attempt = 0; attempt < 32; ++attempt) {
        const auto size = ::read(pipe.read_fd, buffer, sizeof buffer);
        if (size > 0) {
            if (text.size() + static_cast<std::size_t>(size) > limit) return false;
            text.append(buffer, static_cast<std::size_t>(size));
        } else if (size == 0) {
            pipe.close_read();
            break;
        } else if (errno != EINTR) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) pipe.close_read();
            break;
        }
    }
    return true;
}

bool executable_file(const std::string& path) {
    struct stat info {};
    return ::access(path.c_str(), X_OK) == 0 && ::stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}
}

bool command_exists(const std::string& name) {
    if (name.empty()) return false;
    if (name.find('/') != std::string::npos) return executable_file(name);
    const char* environment_path = std::getenv("PATH");
    const std::string path = environment_path ? environment_path : "/bin:/usr/bin";
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find(':', start);
        auto directory = path.substr(start, end == std::string::npos ? end : end - start);
        if (directory.empty()) directory = ".";
        if (executable_file(directory + "/" + name)) return true;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

ProcessResult run_process(const std::vector<std::string>& args, double timeout, const Cancel& cancel) {
    ProcessResult result;
    if (args.empty() || args.front().empty()) {
        result.error = "No subprocess command supplied.";
        return result;
    }
    for (const auto& arg : args) {
        if (arg.find('\0') != std::string::npos) {
            result.error = "A subprocess argument contains a null byte.";
            return result;
        }
    }
    if (cancel && cancel->load()) { result.cancelled = true; return result; }
    if (!std::isfinite(timeout) || timeout <= 0) { result.timed_out = true; return result; }

    Pipe output, error;
    std::vector<char*> argv;
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    int setup_error = posix_spawn_file_actions_init(&actions);
    if (setup_error) { result.error = std::strerror(setup_error); return result; }
    setup_error = posix_spawnattr_init(&attributes);
    if (setup_error) {
        posix_spawn_file_actions_destroy(&actions);
        result.error = std::strerror(setup_error);
        return result;
    }
    auto setup = [&](int status) { if (!setup_error && status) setup_error = status; };
    setup(posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0));
    setup(posix_spawn_file_actions_adddup2(&actions, output.write_fd, STDOUT_FILENO));
    setup(posix_spawn_file_actions_adddup2(&actions, error.write_fd, STDERR_FILENO));
    setup(posix_spawn_file_actions_addclose(&actions, output.read_fd));
    setup(posix_spawn_file_actions_addclose(&actions, error.read_fd));
    setup(posix_spawn_file_actions_addclose(&actions, output.write_fd));
    setup(posix_spawn_file_actions_addclose(&actions, error.write_fd));
    setup(posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP));
    setup(posix_spawnattr_setpgroup(&attributes, 0));
    pid_t child = -1;
    const int status = setup_error ? setup_error :
        posix_spawnp(&child, argv[0], &actions, &attributes, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (status != 0) {
        result.error = args.front() + ": " + std::strerror(status);
        return result;
    }
    output.close_write();
    error.close_write();
    ::fcntl(output.read_fd, F_SETFL, O_NONBLOCK);
    ::fcntl(error.read_fd, F_SETFL, O_NONBLOCK);
    const double deadline = monotonic_seconds() + timeout;
    int child_status = 0;
    bool exited = false, killed = false, output_limited = false;
    while (!exited || output.read_fd >= 0 || error.read_fd >= 0) {
        const bool too_much_output = !read_available(output, result.output, 8 * 1024 * 1024 + 4096)
                                  || !read_available(error, result.error, 64 * 1024);
        if (!killed && ((cancel && cancel->load()) || monotonic_seconds() >= deadline || too_much_output)) {
            result.cancelled = cancel && cancel->load();
            result.timed_out = !result.cancelled && !too_much_output;
            if (too_much_output) {
                output_limited = true;
                result.error = "Subprocess output exceeded its size limit.";
            }
            ::kill(-child, SIGKILL);
            killed = true;
        }
        if (!exited) {
            const auto waited = ::waitpid(child, &child_status, WNOHANG);
            exited = waited == child || (waited < 0 && errno != EINTR);
        }
        if (killed) {
            if (!exited) {
                while (::waitpid(child, &child_status, 0) < 0 && errno == EINTR) {}
                exited = true;
            }
            // All writers in the child's process group have been terminated.
            read_available(output, result.output, 8 * 1024 * 1024 + 4096);
            read_available(error, result.error, 64 * 1024);
            break;
        }
        if (exited && output.read_fd < 0 && error.read_fd < 0) break;
        pollfd fds[] = {{output.read_fd, POLLIN, 0}, {error.read_fd, POLLIN, 0}};
        ::poll(fds, 2, 20);
    }
    if (result.timed_out || result.cancelled || output_limited) return result;
    if (WIFEXITED(child_status)) result.exit_code = WEXITSTATUS(child_status);
    else if (WIFSIGNALED(child_status)) result.exit_code = 128 + WTERMSIG(child_status);
    return result;
}
}
