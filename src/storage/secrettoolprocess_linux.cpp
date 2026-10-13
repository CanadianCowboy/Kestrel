#include "storage/secrettoolprocess_linux.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace kestrel::storage {
namespace {

constexpr std::size_t kOutputLimit = 8 * 1024 * 1024;

class Descriptor {
public:
    explicit Descriptor(int value = -1) : value(value) {}
    ~Descriptor() { close(); }
    void close() { if (value >= 0) { ::close(value); value = -1; } }
    int value;
};

// Blocking SIGPIPE only on this thread avoids changing application-wide signal
// behavior. Consume only the signal this write may have generated, not one
// already pending for the caller.
ssize_t writeSafely(int descriptor, const char* data, std::size_t size) {
    sigset_t blocked;
    sigset_t previous;
    sigset_t pending;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    const int maskError = pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    if (maskError != 0) { errno = maskError; return -1; }
    sigpending(&pending);
    const bool wasPending = sigismember(&pending, SIGPIPE) == 1;
    const ssize_t count = ::write(descriptor, data, size);
    const int saved = errno;
    if (count < 0 && saved == EPIPE && !wasPending) {
        const timespec immediate{0, 0};
        while (sigtimedwait(&blocked, nullptr, &immediate) < 0 && errno == EINTR) {}
    }
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    errno = saved;
    return count;
}

} // namespace

SecretToolResult runSecretTool(const std::vector<std::string>& arguments,
                               std::string_view input, std::chrono::milliseconds timeout) {
    SecretToolResult result;
    std::array<std::array<int, 2>, 3> pipes{{{{-1, -1}}, {{-1, -1}}, {{-1, -1}}}};
    for (auto& pipe : pipes) {
        if (::pipe2(pipe.data(), O_CLOEXEC) != 0) {
            for (auto& open : pipes) {
                if (open[0] >= 0) { ::close(open[0]); }
                if (open[1] >= 0) { ::close(open[1]); }
            }
            result.error = "Could not create secure keyring pipes: " + std::string(std::strerror(errno));
            return result;
        }
    }
    // Spawn dup actions must not alias stdio, even when the launching process
    // inherited closed standard descriptors.
    for (auto& pipe : pipes) {
        for (int& descriptor : pipe) {
            if (descriptor <= STDERR_FILENO) {
                const int moved = fcntl(descriptor, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
                if (moved < 0) {
                    for (auto& open : pipes) { for (int fd : open) { ::close(fd); } }
                    result.error = "Could not reserve secure keyring descriptors.";
                    return result;
                }
                ::close(descriptor);
                descriptor = moved;
            }
        }
    }
    Descriptor inRead(pipes[0][0]), inWrite(pipes[0][1]);
    Descriptor outRead(pipes[1][0]), outWrite(pipes[1][1]);
    Descriptor errRead(pipes[2][0]), errWrite(pipes[2][1]);
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>("secret-tool"));
    for (const auto& argument : arguments) {
        if (argument.find('\0') != std::string::npos) {
            result.error = "Keyring attributes cannot contain a NUL byte.";
            return result;
        }
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    int status = posix_spawn_file_actions_init(&actions);
    if (status != 0) {
        result.error = "Could not initialize the keyring process.";
        return result;
    }
    status = posix_spawnattr_init(&attributes);
    if (status != 0) {
        posix_spawn_file_actions_destroy(&actions);
        result.error = "Could not initialize the keyring process attributes.";
        return result;
    }
    const auto add = [&status](int code) { if (status == 0) { status = code; } };
    add(posix_spawn_file_actions_adddup2(&actions, inRead.value, STDIN_FILENO));
    add(posix_spawn_file_actions_adddup2(&actions, outWrite.value, STDOUT_FILENO));
    add(posix_spawn_file_actions_adddup2(&actions, errWrite.value, STDERR_FILENO));
    add(posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP));
    add(posix_spawnattr_setpgroup(&attributes, 0));
    pid_t pid = -1;
    if (status == 0) {
        status = posix_spawnp(&pid, "secret-tool", &actions, &attributes, argv.data(), environ);
    }
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (status != 0) {
        result.error = status == ENOENT
            ? "secret-tool is missing. Install libsecret-tools and unlock a Secret Service keyring in your desktop session."
            : "Could not start secret-tool: " + std::string(std::strerror(status));
        return result;
    }
    inRead.close(); outWrite.close(); errWrite.close();
    for (const int descriptor : {inWrite.value, outRead.value, errRead.value}) {
        const int flags = fcntl(descriptor, F_GETFL);
        if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0) {
            result.error = "Could not configure nonblocking keyring pipes.";
        }
    }
    if (input.empty()) { inWrite.close(); }
    std::size_t written = 0;
    bool exited = false;
    int exitStatus = 0;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::array<char, 4096> buffer{};
    while (result.error.empty() && (!exited || outRead.value >= 0 || errRead.value >= 0)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            result.error = "The keyring request timed out. Unlock your keyring and check the desktop D-Bus session before retrying; no plaintext fallback was saved.";
            break;
        }
        std::array<pollfd, 3> descriptors{{
            {inWrite.value, POLLOUT, 0}, {outRead.value, POLLIN, 0}, {errRead.value, POLLIN, 0}}};
        const int ready = ::poll(descriptors.data(), descriptors.size(), 20);
        if (ready < 0 && errno != EINTR) {
            result.error = "Could not poll the keyring process.";
            break;
        }
        if (inWrite.value >= 0 && (descriptors[0].revents & (POLLOUT | POLLHUP | POLLERR))) {
            const ssize_t count = writeSafely(inWrite.value, input.data() + written,
                                               std::min<std::size_t>(4096, input.size() - written));
            if (count > 0) { written += static_cast<std::size_t>(count); }
            else if (count < 0 && errno != EINTR && errno != EAGAIN) {
                result.error = "secret-tool closed its input before accepting the complete profile. Unlock the keyring and retry.";
            }
            if (written == input.size()) { inWrite.close(); }
        }
        const auto drain = [&](Descriptor& descriptor, std::string& output) {
            if (descriptor.value < 0) { return; }
            // Bounded per poll cycle: a child flooding output cannot prevent
            // checking cancellation/deadline or draining the other pipe.
            for (int chunk = 0; chunk < 16; ++chunk) {
                const ssize_t count = ::read(descriptor.value, buffer.data(), buffer.size());
                if (count > 0) {
                    if (result.output.size() + result.diagnostics.size() + static_cast<std::size_t>(count) > kOutputLimit) {
                        result.error = "The keyring returned too much data. Refusing the response; inspect the Secret Service installation.";
                        return;
                    }
                    output.append(buffer.data(), static_cast<std::size_t>(count));
                } else if (count == 0) { descriptor.close(); return; }
                else if (errno == EINTR) { continue; }
                else if (errno == EAGAIN) { return; }
                else { result.error = "Could not read the keyring response."; return; }
            }
        };
        drain(outRead, result.output);
        drain(errRead, result.diagnostics);
        if (!exited) {
            const pid_t reaped = ::waitpid(pid, &exitStatus, WNOHANG);
            if (reaped == pid) { exited = true; }
            else if (reaped < 0 && errno != EINTR) {
                result.error = "Could not collect the keyring process status.";
            }
        }
    }
    if (!result.error.empty()) {
        // Also kill descendants holding inherited pipes open, even if the
        // direct child exited. No stale helper should survive a timed-out call.
        ::kill(-pid, SIGKILL);
        if (!exited) {
            while (::waitpid(pid, &exitStatus, 0) < 0 && errno == EINTR) {}
        }
        result.output.clear();
        result.diagnostics.clear(); // Search output can contain secrets.
        return result;
    }
    if (written != input.size()) {
        result.error = "The keyring process exited before accepting the complete profile.";
    } else if (WIFEXITED(exitStatus)) {
        result.status = WEXITSTATUS(exitStatus);
    } else {
        result.error = "The keyring process terminated unexpectedly. Check the Secret Service and retry.";
    }
    return result;
}

} // namespace kestrel::storage
