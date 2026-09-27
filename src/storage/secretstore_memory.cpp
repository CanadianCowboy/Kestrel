#include "storage/secretstore.h"

#include <algorithm>
#include <utility>

namespace kestrel::storage {

namespace {

std::string key(std::string_view service, std::string_view account) {
    std::string composed;
    composed.reserve(service.size() + 1 + account.size());
    composed.append(service).push_back('\n');
    composed.append(account);
    return composed;
}

} // namespace

std::string InMemorySecretStore::description() const {
    // Spelled out rather than softened, because this store is not encryption
    // and anything that reads it should have to decide what that means.
    return "in-memory (nothing is written to disk)";
}

bool InMemorySecretStore::available(std::string&) const {
    return true;
}

bool InMemorySecretStore::seal(std::string_view account, std::string_view service,
                               const std::vector<std::uint8_t>& blob, std::string&) {
    const std::string composed = key(service, account);
    auto existing = std::find_if(m_entries.begin(), m_entries.end(),
                                 [&composed](const auto& entry) { return entry.first == composed; });
    if (existing != m_entries.end()) {
        existing->second = blob;
        return true;
    }
    m_entries.emplace_back(composed, blob);
    return true;
}

std::optional<std::vector<std::uint8_t>> InMemorySecretStore::sealed(
    std::string_view account, std::string_view service, std::string&) {
    const std::string composed = key(service, account);
    auto existing = std::find_if(m_entries.begin(), m_entries.end(),
                                 [&composed](const auto& entry) { return entry.first == composed; });
    if (existing == m_entries.end()) {
        return std::nullopt;
    }
    return existing->second;
}

bool InMemorySecretStore::forget(std::string_view account, std::string_view service, std::string&) {
    const std::string composed = key(service, account);
    const auto before = m_entries.size();
    m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
                                   [&composed](const auto& entry) { return entry.first == composed; }),
                    m_entries.end());
    // Removing something that was never there is the state the caller asked
    // for, not a failure.
    return m_entries.size() <= before;
}

} // namespace kestrel::storage
