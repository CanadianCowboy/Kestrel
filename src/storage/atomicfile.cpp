#include "core/pathtext.h"
#include "storage/atomicfile.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace kestrel::storage {

namespace fs = std::filesystem;

namespace {

std::string describe(const std::error_code& code) {
    return code ? code.message() : std::string("no further detail is available");
}

// A scratch suffix no other writer, in this process or any other, can be
// holding at this moment. The process id keeps a second Kestrel -- or a backup
// tool that happens to use the same name -- off this write's scratch file; the
// counter keeps two saves inside one process off each other's. Sharing a
// single fixed name is what makes two overlapping saves interleave, after
// which one rename can publish a mixture of both payloads.
std::string scratchSuffix() {
    static std::atomic<unsigned long> sequence{0};
    const unsigned long unique = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(_WIN32)
    const auto pid = static_cast<unsigned long>(::GetCurrentProcessId());
#else
    const auto pid = static_cast<unsigned long>(::getpid());
#endif
    return std::to_string(pid) + "-" + std::to_string(unique);
}

// Pushes a file's own buffers out to the device before its name is published.
// Without this the ordering "name durable, contents not" is still reachable,
// and it produces a file that exists and is empty.
bool flushToDevice(const fs::path& path, std::string& error) {
#if defined(_WIN32)
    // Two things are wrong with the obvious spelling of this call. CreateFile
    // returns INVALID_HANDLE_VALUE on failure, not nullptr, so a truth test on
    // the handle is always true and goes on to flush an invalid one. And
    // FlushFileBuffers needs GENERIC_WRITE: opened GENERIC_READ it fails with
    // ERROR_ACCESS_DENIED, which is a no-op that looks like it worked.
    HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = "could not reopen " + core::pathText(path) + " in order to flush it: Windows error " +
                std::to_string(::GetLastError());
        return false;
    }
    const BOOL flushed = ::FlushFileBuffers(handle);
    const DWORD flushError = ::GetLastError();
    ::CloseHandle(handle);
    if (!flushed) {
        error = "could not flush " + core::pathText(path) + " to the device: Windows error " +
                std::to_string(flushError);
        return false;
    }
    return true;
#else
    // A C++ stream flush stops at the library's own buffer, which is not the
    // device. fsync is the only call here that reaches the disk.
    const int descriptor = ::open(path.c_str(), O_WRONLY);
    if (descriptor < 0) {
        error = "could not reopen " + core::pathText(path) + " in order to flush it: " +
                std::string(std::strerror(errno));
        return false;
    }
    const bool synced = ::fsync(descriptor) == 0;
    const int syncError = errno;
    ::close(descriptor);
    if (!synced) {
        error = "could not flush " + core::pathText(path) + " to the device: " +
                std::string(std::strerror(syncError));
        return false;
    }
    return true;
#endif
}

// Pushes a directory's own entries out to the device.
//
// rename(2) does not rewrite the file; it edits the parent directory, and that
// edit is a write of its own. Syncing the file made its contents durable but
// left the name it was published under in the same state the old name was in,
// so a power cut could still bring the previous contents back. Windows gets
// this from MOVEFILE_WRITE_THROUGH, which the rename is already carrying; POSIX
// has no equivalent, so it has to be asked for by hand.
bool flushDirectory(const fs::path& path, std::string& error) {
#if defined(_WIN32)
    // Reached only by the POSIX branch, but the function is compiled
    // unconditionally, so it needs a body here. The Windows path never calls
    // it: MOVEFILE_WRITE_THROUGH has already done this work.
    (void)path;
    (void)error;
    return true;
#else
    int flags = O_RDONLY;
    // O_DIRECTORY is Linux's way of refusing to open anything that is not a
    // directory, which turns a typo into an error instead of a sync of some
    // other file. It does not exist on macOS or the BSDs, where opening a
    // directory read-only and syncing it is the documented spelling.
#if defined(O_DIRECTORY)
    flags |= O_DIRECTORY;
#endif
    const int descriptor = ::open(path.c_str(), flags);
    if (descriptor < 0) {
        error = "could not open " + core::pathText(path) + " in order to flush it: " +
                std::string(std::strerror(errno));
        return false;
    }
    const bool synced = ::fsync(descriptor) == 0;
    const int syncError = errno;
    ::close(descriptor);
    if (!synced) {
        error = "could not flush " + core::pathText(path) + " to the device: " +
                std::string(std::strerror(syncError));
        return false;
    }
    return true;
#endif
}

} // namespace

bool writeFileAtomically(const fs::path& target, const std::vector<std::uint8_t>& bytes,
                         std::string& error) {
    std::error_code code;
    if (target.has_parent_path()) {
        fs::create_directories(target.parent_path(), code);
    }

    // The scratch name has to be unique per write: two saves in quick succession
    // would otherwise race on the same scratch file, and the loser's rename
    // would publish the winner's bytes.
    // Appended to the path rather than to its text form, so the name keeps its
    // native encoding: going through string() would both throw on a name the ANSI
    // code page cannot hold and, where it did not throw, rewrite the path in
    // whatever that code page says the name is.
    fs::path scratch = target;
    scratch += "." + scratchSuffix() + ".tmp";
    {
        std::ofstream out(scratch, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "could not open " + core::pathText(scratch) + " for writing: " + describe(code);
            return false;
        }
        if (!bytes.empty()) {
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
        }
        out.flush();
        if (!out) {
            error = "could not write " + core::pathText(scratch) + ": " + describe(code);
            std::error_code ignored;
            fs::remove(scratch, ignored);
            return false;
        }
    }

    // Content before name. Bailing out here leaves the previous file exactly
    // as it was, which is the entire reason this function exists.
    if (!flushToDevice(scratch, error)) {
        std::error_code ignored;
        fs::remove(scratch, ignored);
        return false;
    }

#if defined(_WIN32)
    // MOVEFILE_WRITE_THROUGH because the rename itself is the second half of
    // the ordering, and it is the one Windows will otherwise skip.
    if (!::MoveFileExW(scratch.c_str(), target.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "could not replace " + core::pathText(target) + ": Windows error " +
                std::to_string(::GetLastError());
        std::error_code ignored;
        fs::remove(scratch, ignored);
        return false;
    }
#else
    // rename(2) replaces atomically within a filesystem, which is the reason
    // the scratch file is a sibling rather than something in a temp directory.
    std::error_code renameCode;
    fs::rename(scratch, target, renameCode);
    if (renameCode) {
        error = "could not replace " + core::pathText(target) + ": " + describe(renameCode);
        std::error_code ignored;
        fs::remove(scratch, ignored);
        return false;
    }
    // The rename is already visible at this point, so a failure here is not a
    // failed write: the file is there, with the right bytes, and only its name
    // is not yet durable. It is reported rather than swallowed because the
    // whole point of this function is that a closed laptop is survivable, and
    // "it usually is" is not a claim worth making quietly. Rolling back would be
    // worse than the risk: the new contents are already the ones on screen.
    if (!flushDirectory(target.has_parent_path() ? target.parent_path() : fs::path("."), error)) {
        error += " (the new contents are in place; only the name may not survive a power cut)";
        return false;
    }
#endif
    return true;
}

bool writeFileAtomically(const fs::path& target, std::string_view text, std::string& error) {
    return writeFileAtomically(
        target, std::vector<std::uint8_t>(text.begin(), text.end()), error);
}

bool readWholeFile(const fs::path& path, std::vector<std::uint8_t>& bytes, std::string& error) {
    std::error_code code;
    const bool present = fs::exists(path, code);
    // exists() answers false for a file that is not there and for one that
    // cannot be reached -- a directory the process may not enter, a dead mount,
    // a path with a component that is not a directory. Only the first of those
    // is a first run. Folding the second into it would report an unreadable
    // profile as an absent one, and the next save would then overwrite whatever
    // was really there.
    if (code && code != std::errc::no_such_file_or_directory) {
        error = "could not reach " + core::pathText(path) + ": " + describe(code);
        bytes.clear();
        return false;
    }
    if (!present) {
        bytes.clear();
        return true;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "could not open " + core::pathText(path) + " for reading";
        bytes.clear();
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    if (size < 0) {
        error = "could not measure " + core::pathText(path);
        bytes.clear();
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size));
    if (size > 0) {
        in.read(reinterpret_cast<char*>(bytes.data()), size);
        if (in.gcount() != size) {
            error = "could not read all of " + core::pathText(path);
            bytes.clear();
            return false;
        }
    }
    return true;
}

} // namespace kestrel::storage
