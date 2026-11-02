#pragma once
#include "models.hpp"

namespace tl {
struct ProcessResult {
    int exit_code = -1;
    std::string output, error;
    bool timed_out = false, cancelled = false;
};
ProcessResult run_process(const std::vector<std::string>& args, double timeout = 2.0,
                          const Cancel& cancel = {});
bool command_exists(const std::string& name);
}
