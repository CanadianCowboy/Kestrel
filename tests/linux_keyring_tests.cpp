#include "storage/secretstore.h"
#include "storage/secrettoolprocess_linux.h"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace kestrel::storage;

int main() {
    char directory[] = "/tmp/kestrel-keyring-tests-XXXXXX";
    assert(::mkdtemp(directory) != nullptr);
    const fs::path root(directory);
    const char* originalPath = std::getenv("PATH");
    const std::string savedPath = originalPath == nullptr ? "" : originalPath;
    const bool pathWasSet = originalPath != nullptr;
    ::setenv("PATH", directory, 1);
    const auto script = [&](std::string body) {
        std::ofstream file(root / "secret-tool");
        file << "#!/bin/sh\n" << body;
        file.close();
        assert(::chmod((root / "secret-tool").c_str(), 0700) == 0);
    };
    auto store = makePlatformSecretStore();
    std::string error = "stale";
    assert(!store->available(error));
    assert(error.find("libsecret-tools") != std::string::npos);
    assert(!store->sealed("a", "s", error));
    assert(!error.empty());

    script("echo 'locked keyring' >&2\nexit 1\n");
    assert(!store->seal("a", "s", {'p'}, error));
    assert(error.find("Unlock") != std::string::npos);
    assert(!store->sealed("a", "s", error));
    assert(!store->forget("a", "s", error));

    // A misleading stderr value must never be decoded as a profile, even when
    // the process claims success. Failure messages must not leak secret bytes.
    script("echo 'secret = 736563726574' >&2\nexit 0\n");
    assert(!store->sealed("a", "s", error));
    assert(error.find("736563726574") == std::string::npos);

    script("case \"$1\" in\nsearch) echo 'secret = 70726f6265';;\nesac\nexit 0\n");
    assert(!store->forget("a", "s", error));
    assert(error.find("still there") != std::string::npos);
    script("echo 'label = locked'\nexit 0\n");
    assert(!store->sealed("a", "s", error));
    assert(error.find("unlock") != std::string::npos);

    script("exit 0\n");
    error = "stale";
    assert(!store->sealed("a", "s", error));
    assert(error.empty());
    error = "stale";
    assert(store->forget("a", "s", error));
    assert(error.empty());

    // Arguments are literal, including shell punctuation and newlines.
    script("printf '%s\\n' \"$@\"\n/bin/cat\n");
    const auto quoted = runSecretTool({"search", "a'; touch injected; '", "two\nlines"}, "private-input", std::chrono::seconds(1));
    assert(quoted.status == 0 && quoted.error.empty());
    assert(quoted.output == "search\na'; touch injected; '\ntwo\nlines\nprivate-input");
    assert(!fs::exists("injected"));

    // A child that exits before reading a large secret must not deliver
    // SIGPIPE to the application or claim the partial input was saved.
    script("exit 0\n");
    const auto closed = runSecretTool({"store"}, std::string(1024 * 1024, 'x'), std::chrono::seconds(1));
    assert(!closed.error.empty());

    script("/bin/dd if=/dev/zero bs=65536 count=160 2>/dev/null\n");
    const auto flooding = runSecretTool({"search"}, {}, std::chrono::seconds(1));
    assert(flooding.error.find("too much data") != std::string::npos);
    assert(flooding.output.empty());

    script("/bin/sleep 30 &\nwait\n");
    const auto start = std::chrono::steady_clock::now();
    const auto stalled = runSecretTool({"search"}, {}, std::chrono::milliseconds(80));
    assert(stalled.error.find("timed out") != std::string::npos);
    assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));

    script("/bin/sleep 30 &\nexit 0\n");
    const auto inherited = runSecretTool({"search"}, {}, std::chrono::milliseconds(80));
    assert(inherited.error.find("timed out") != std::string::npos);
    script("/bin/sleep 30 &\nwait\n");

    // Verify the production store path, not only the process primitive, is
    // bounded. Five seconds is enough for an unlock prompt but not a forever hang.
    const auto storeStart = std::chrono::steady_clock::now();
    assert(!store->seal("a", "s", {'x'}, error));
    assert(error.find("timed out") != std::string::npos);
    assert(std::chrono::steady_clock::now() - storeStart < std::chrono::seconds(7));

    // Round-trip binary bytes, overwrites, availability and verified deletion
    // through the real adapter. The fixture only stores test data in its own
    // scratch directory; never the user's keyring or the application folder.
    script("state='" + (root / "fixture").string() + "'\n"
           "case \"$1\" in\n"
           "store) /bin/cat > \"$state\";;\n"
           "search) if [ -f \"$state\" ]; then printf 'secret = '; /bin/cat \"$state\"; fi;;\n"
           "clear) /bin/rm -f \"$state\";;\n"
           "esac\n");
    assert(store->available(error) && error.empty());
    const std::vector<std::uint8_t> bytes{0, 255, 10, 127};
    assert(store->seal("a", "s", bytes, error));
    assert(store->sealed("a", "s", error) == bytes);
    assert(store->seal("a", "s", {42}, error));
    assert(store->sealed("a", "s", error) == std::vector<std::uint8_t>{42});
    assert(store->forget("a", "s", error));
    assert(!store->sealed("a", "s", error) && error.empty());
    fs::remove_all(root);
    if (pathWasSet) { ::setenv("PATH", savedPath.c_str(), 1); }
    else { ::unsetenv("PATH"); }
    std::cout << "Linux keyring subprocess and storage checks passed\n";
}
