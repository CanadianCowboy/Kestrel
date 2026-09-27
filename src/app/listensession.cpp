#include "app/listensession.h"

namespace kestrel::app {

ListenSession::ListenSession(runtime::SpeechRecognizer& recognizer, QObject* parent)
    : QObject(parent), m_recognizer(recognizer) {
    m_poll.setInterval(80);
    connect(&m_poll, &QTimer::timeout, this, &ListenSession::poll);
}

bool ListenSession::startListening(QString& error) {
    if (m_listening) {
        error = tr("Already listening.");
        return false;
    }
    if (!m_recognizer.available()) {
        error = QString::fromStdString(m_recognizer.detail());
        return false;
    }

    m_partial.clear();
    m_submitted = false;
    std::string failure;
    if (!m_recognizer.start([this](const runtime::RecognitionResult& result) { onResult(result); },
                           [this](runtime::RecognitionEnd reason, std::string_view detail) {
                               onEnd(reason, detail);
                           },
                           failure)) {
        error = QString::fromStdString(failure);
        return false;
    }

    m_listening = true;
    m_poll.start();
    emit partialChanged();
    return true;
}

void ListenSession::stopListening() {
    if (!m_listening) {
        return;
    }
    m_poll.stop();
    m_recognizer.stop();
    // stop() may or may not produce an end event depending on the recognizer, so
    // the state is settled here rather than waiting to be told.
    m_listening = false;
    m_partial.clear();
    m_submitted = false;
    emit partialChanged();
}

void ListenSession::abandon() {
    // The phrase is discarded, not submitted: a user who starts typing has
    // already said what they wanted, and submitting a half-spoken sentence on
    // top of it would be worse than losing the words.
    stopListening();
}

bool ListenSession::listening() const noexcept {
    return m_listening;
}

void ListenSession::poll() {
    if (!m_listening) {
        return;
    }
    // Only the mock has anything to advance; a push-style recognizer ignores
    // this call, which is why the session does not need to know which it has.
    if (auto* mock = dynamic_cast<runtime::MockSpeechRecognizer*>(&m_recognizer)) {
        mock->emitNextPartial();
    }
}

QString ListenSession::partialText() const {
    return m_partial;
}

void ListenSession::setPartialIntervalMs(int ms) {
    m_poll.setInterval(std::max(1, ms));
}

void ListenSession::onResult(const runtime::RecognitionResult& result) {
    if (!m_listening) {
        return;
    }
    if (result.isFinal) {
        if (m_submitted || result.text.empty()) {
            return;
        }
        m_submitted = true;
        const QString text = QString::fromStdString(result.text);
        m_partial.clear();
        emit partialChanged();
        // Emitted before the state is torn down, so a slot that starts a new
        // turn is not immediately contradicted by a listening-ended signal.
        emit utteranceFinal(text, result.confidence);
        return;
    }

    const QString text = QString::fromStdString(result.text);
    if (text == m_partial) {
        return;
    }
    m_partial = text;
    emit partialChanged();
}

void ListenSession::onEnd(runtime::RecognitionEnd reason, std::string_view detail) {
    if (!m_listening) {
        return;
    }
    m_poll.stop();
    m_listening = false;
    m_partial.clear();
    m_submitted = false;
    emit partialChanged();

    if (reason == runtime::RecognitionEnd::Cancelled) {
        return; // the user asked for this; it is not a failure worth reporting
    }
    emit listeningEnded(QString::fromStdString(detail.empty()
                                                   ? std::string(runtime::toString(reason))
                                                   : std::string(detail)));
}

} // namespace kestrel::app
