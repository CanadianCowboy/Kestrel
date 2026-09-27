#include "storage/secretsearch.h"

#include <optional>
#include <utility>

namespace kestrel::storage {

namespace {

// Trailing newlines and carriage returns, which is all a shell pipeline adds.
std::string_view trimmed(std::string_view text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

// The value from a report, or nullopt when the report holds none.
//
// `search` prints one "secret = " line per item it managed to read. The line is
// a literal in libsecret, and the value after it is written with no trailing
// newline of its own, so the line runs to the end of that line and no further.
std::optional<std::string> secretValue(std::string_view report) {
    static constexpr std::string_view kPrefix = "secret = ";
    std::size_t at = 0;
    while (at < report.size()) {
        const std::size_t end = report.find('\n', at);
        const std::size_t stop = end == std::string_view::npos ? report.size() : end;
        const std::string_view line = report.substr(at, stop - at);
        if (line.substr(0, kPrefix.size()) == kPrefix) {
            return std::string(line.substr(kPrefix.size()));
        }
        at = stop + 1;
    }
    return std::nullopt;
}

} // namespace

SecretSearchReport classifySecretSearch(int status, std::string_view report) {
    SecretSearchReport result;
    const std::string_view said = trimmed(report);

    // Taken before anything else, and never softened. `search` exits 0 for a
    // miss, so a non-zero status cannot mean "nothing stored" -- and this is
    // also what makes a failure that said nothing still a failure, rather than
    // an empty profile with a save behind it.
    if (status != 0) {
        result.outcome = SecretSearchOutcome::failed;
        result.reason = said.empty() ? "secret-tool failed without saying why"
                                     : std::string(said);
        return result;
    }

    if (const auto secret = secretValue(said)) {
        result.outcome = SecretSearchOutcome::readable;
        result.secret = std::move(*secret);
        return result;
    }

    // Exit 0, and nothing to read. An empty report means no item matched, which
    // is libsecret saying so rather than a guess from silence. An empty value
    // is a stored profile whose value happened to be nothing, and the caller
    // gets it as a readable report with an empty secret.
    if (said.empty()) {
        result.outcome = SecretSearchOutcome::absent;
        return result;
    }

    // The item was reported and its value was not. It is there, and it cannot
    // be read, and saying that is the entire reason the store asks `search`
    // rather than `lookup`.
    result.outcome = SecretSearchOutcome::unreadable;
    result.reason = std::string(said);
    return result;
}

} // namespace kestrel::storage
