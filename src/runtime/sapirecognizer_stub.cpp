// The portable half of the SAPI 5 speech recognizer.
//
// Compiled wherever SAPI is not available, so the app still builds and still
// says plainly that it cannot listen. It exists for the same reason as the CUDA
// discovery stub: a missing optional dependency should change what the app can
// do, never whether it compiles.
//
// Nothing here pretends. available() is false, detail() names the reason, and
// makePlatformSpeechRecognizer() hands back this object rather than the mock --
// substituting one for the other here is precisely the dishonesty the selection
// logic in speechrecognizer.cpp is written to avoid.

#include "runtime/sapirecognizer.h"

namespace kestrel::runtime {

class SapiSpeechRecognizer::Impl {};

SapiSpeechRecognizer::SapiSpeechRecognizer()
    : m_impl(std::make_unique<Impl>()) {}

SapiSpeechRecognizer::~SapiSpeechRecognizer() = default;

bool SapiSpeechRecognizer::available() const {
    return false;
}

std::string SapiSpeechRecognizer::detail() const {
    return "this build has no SAPI speech recognition";
}

bool SapiSpeechRecognizer::start(ResultCallback, EndCallback, std::string& error) {
    error = detail();
    return false;
}

void SapiSpeechRecognizer::stop() {}

bool SapiSpeechRecognizer::listening() const {
    return false;
}

unsigned microphoneDeviceCount() {
    return 0;
}

Microphone probeMicrophone() {
    // Not "absent" because it was checked: on a platform with no SAPI there is
    // nothing to check with, and saying so is more useful than a device count
    // of zero that reads like a machine with the microphone unplugged.
    return Microphone::Absent;
}

std::unique_ptr<SpeechRecognizer> makePlatformSpeechRecognizer() {
    return std::make_unique<SapiSpeechRecognizer>();
}

} // namespace kestrel::runtime
