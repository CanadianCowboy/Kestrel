#include "storage/atomicfile.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace kestrel::storage {

namespace fs = std::filesystem;

namespace {

std::string describe(const std::error_code& code) {
    return code ? code.message() : std::string("no further detail is available");
}

} // namespace

bool writeFileAtomically(const fs::path& target, const std::vector<std::uint8_t>& bytes,
                         std::string& error) {
    std::error_code code;
    if (target.has_parent_path()) {
        fs::create_directories(target.parent_path(), code);
    }

    // The temp name has to be unique per write: two saves in quick succession
    // would otherwise race on the same scratch file, and the loser's rename
    // would publish the winner's bytes.
    const fs::path scratch = target.string() + ".tmp";
    {
        std::ofstream out(scratch, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "could not open " + scratch.string() + " for writing: " + describe(code);
            return false;
        }
        if (!bytes.empty()) {
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
        }
        out.flush();
        if (!out) {
            error = "could not write " + scratch.string() + ": " + describe(code);
            return false;
        }
    }

#if defined(_WIN32)
    // Flush the file's own buffers to the device before the rename. Without
    // this the name can be durable while the contents are not, which is the
    // one ordering that produces a file that exists and is empty.
    if (HANDLE handle = ::CreateFileW(scratch.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY, nullptr)) {
        ::FlushFileBuffers(handle);
        ::CloseHandle(handle);
    }
    if (!::MoveFileExW(scratch.c_str(), target.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "could not replace " + target.string() + ": Windows error " +
                std::to_string(::GetLastError());
        std::error_code ignored;
        fs::remove(scratch, ignored);
        return false;
    }
#else
    std::ofstream flushOnly(scratch, std::ios::binary | std::ios::app);
    flushOnly.flush();
    flushOnly.close();

    // rename(2) replaces atomically within a filesystem, which is the reason
    // the scratch file is a sibling rather than something in a temp directory.
    std::error_code renameCode;
    fs::rename(scratch, target, renameCode);
    if (renameCode) {
        error = "could not replace " + target.string() + ": " + describe(renameCode);
        fs::remove(scratch, code);
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
    if (!fs::exists(path, code)) {
        bytes.clear();
        return true;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "could not open " + path.string() + " for reading";
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    if (size < 0) {
        error = "could not measure " + path.string();
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size));
    if (size > 0) {
        in.read(reinterpret_cast<char*>(bytes.data()), size);
        if (in.gcount() != size) {
            error = "could not read all of " + path.string();
            bytes.clear();
            return false;
        }
    }
    return true;
}

} // namespace kestrel::storage
