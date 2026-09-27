// The portable half of the platform speech recognizer.
//
// Compiled wherever the Windows Runtime speech headers are not available, so
// the app still builds and still reports honestly. It exists for the same
// reason as the CUDA discovery stub: a missing optional dependency should
// change what the app can do, never whether it compiles.

#include "runtime/winrtspeechrecognizer.h"

namespace kestrel::runtime {

// No platform recognizer, so there is nothing to hold.
class WinRtSpeechRecognizer::Impl {};

WinRtSpeechRecognizer::WinRtSpeechRecognizer()
    : m_impl(std::make_unique<Impl>()) {}

WinRtSpeechRecognizer::~WinRtSpeechRecognizer() = default;

bool WinRtSpeechRecognizer::available() const {
    return false;
}

std::string WinRtSpeechRecognizer::detail() const {
    return "this build has no platform speech recognition";
}

bool WinRtSpeechRecognizer::start(ResultCallback, EndCallback, std::string& error) {
    error = detail();
    return false;
}

void WinRtSpeechRecognizer::stop() {}

bool WinRtSpeechRecognizer::listening() const {
    return false;
}

std::unique_ptr<SpeechRecognizer> makePlatformSpeechRecognizer() {
    return std::make_unique<WinRtSpeechRecognizer>();
}

} // namespace kestrel::runtime
