#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace kestrel::storage {

struct SecretToolResult {
    int status = -1;
    std::string output;
    std::string diagnostics;
    std::string error;
};

// Secrets travel on stdin, never argv or a scratch file. Both pipes are drained
// concurrently; exit, IO, deadline and output limits all fail closed.
[[nodiscard]] SecretToolResult runSecretTool(const std::vector<std::string>& arguments,
    std::string_view input, std::chrono::milliseconds timeout);

} // namespace kestrel::storage
