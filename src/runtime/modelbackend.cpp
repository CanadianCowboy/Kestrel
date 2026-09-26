#include "runtime/modelbackend.h"

#include <algorithm>
#include <string_view>

namespace kestrel::runtime {

namespace {

// Characters per token for English prose. BPE vocabularies used by current
// models average close to this, so the approximation is within a useful
// margin -- but it is still an approximation, which is why a backend with a
// real tokenizer overrides it.
constexpr double kApproxCharsPerToken = 4.0;

} // namespace

std::size_t ModelBackend::countTokens(std::string_view text) const {
    if (text.empty()) {
        return 0;
    }
    const auto estimate =
        static_cast<std::size_t>(static_cast<double>(text.size()) / kApproxCharsPerToken);
    // Never report zero for non-empty text: a caller treating 0 as "no cost"
    // would silently under-count the whole prompt.
    return std::max<std::size_t>(1, estimate);
}

void ModelBackend::resetContextUsage() {
    // Backends that accumulate per-session state override this.
}

void ModelBackend::setSystemPrompt(std::string_view text) {
    m_systemPrompt = text;
}

std::string_view ModelBackend::systemPrompt() const noexcept {
    return m_systemPrompt;
}

void ModelBackend::clearSharedPrefix() {
    m_systemPrompt.clear();
}

std::size_t ModelBackend::cachedPrefixTokens() const {
    // No cache here: this backend re-sends the prefix on every request, so
    // report what the prefix actually costs rather than implying a saving.
    return countTokens(m_systemPrompt);
}

} // namespace kestrel::runtime
