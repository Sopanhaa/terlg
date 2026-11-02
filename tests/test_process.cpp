#include "terminal_lyrics/process.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    try {
        check(tl::command_exists("sh"), "sh should be found in PATH");
        check(!tl::command_exists("/tmp"), "a directory is not an executable file");
        check(!tl::command_exists("terminal-lyrics-nonexistent-command"), "missing executable found");
        auto result = tl::run_process({"/bin/printf", "%s", "$(echo unsafe); 'hello' & literal"});
        check(result.exit_code == 0, "printf failed");
        check(result.output == "$(echo unsafe); 'hello' & literal", "arguments must not pass through a shell");

        result = tl::run_process({"/bin/sh", "-c", "printf output; printf error >&2; exit 7"});
        check(result.exit_code == 7 && result.output == "output" && result.error == "error",
              "stdout/stderr or exit status was lost");
        result = tl::run_process({"terminal-lyrics-nonexistent-command"});
        check(result.exit_code != 0 && !result.error.empty(), "missing executable must report an error");
        result = tl::run_process({});
        check(result.exit_code != 0, "empty command succeeded");
        result = tl::run_process({"/bin/printf", std::string("a\0b", 3)});
        check(result.exit_code != 0, "null bytes must not silently truncate arguments");

        double start = tl::monotonic_seconds();
        result = tl::run_process({"/bin/sh", "-c", "sleep 10"}, 0.06);
        check(result.timed_out && result.exit_code != 0, "subprocess timeout failed");
        check(tl::monotonic_seconds() - start < 1.0, "timeout waited for a descendant process");

        auto cancel = std::make_shared<std::atomic_bool>(false);
        std::thread canceller([cancel] { std::this_thread::sleep_for(std::chrono::milliseconds(30)); cancel->store(true); });
        start = tl::monotonic_seconds();
        result = tl::run_process({"/bin/sh", "-c", "sleep 10"}, 10, cancel);
        canceller.join();
        check(result.cancelled && result.exit_code != 0, "subprocess cancellation failed");
        check(tl::monotonic_seconds() - start < 1.0, "cancellation waited for a descendant process");
        result = tl::run_process({"/bin/printf", "must not run"}, 2, cancel);
        check(result.cancelled && result.output.empty(), "already cancelled process was launched");

        result = tl::run_process({"/bin/sh", "-c", "head -c 8500000 /dev/zero"}, 3);
        check(result.exit_code != 0 && result.error.find("size limit") != std::string::npos,
              "unbounded subprocess output was accepted");
        std::cout << "Process tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Process test failed: " << error.what() << '\n';
        return 1;
    }
}
