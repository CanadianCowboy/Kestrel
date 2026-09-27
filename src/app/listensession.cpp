#include "app/listensession.h"

namespace kestrel::app {

ListenSession::ListenSession(runtime::SpeechRecognizer& recognizer, QObject* parent)
    : QObject(parent), m_recognizer(recognizer) {
    m_poll.setInterval(80);
    connect(&m_poll, &QTimer::timeout, this, &ListenSession::poll);
}

ListenSession::~ListenSession() {
    // Unconditional, and before anything of this object is gone. stop() is
    // cheap when the session already ended -- the callbacks have been taken
    // and cleared, so there is nothing to wait for -- and it is the only thing
    // that makes the recognizer's use of `this` safe from here on.
    m_recognizer.stop();
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
    // The recognizer's contract says its callbacks arrive on its own thread, so
    // they are handed straight back to this object's thread rather than acted
    // on there. A push-style recognizer -- which the SAPI adapter is, and the
    // scripted preview recognizer is not -- would otherwise touch a QString and
    // emit signals from off the GUI thread.
    //
    // The result and the end reason are copied into the queued call, because
    // the recognizer owns them and may reuse the storage as soon as it returns.
    //
    // The queued call being addressed to this object is half of the safety
    // story and not all of it. A posted event whose receiver has been destroyed
    // is discarded by Qt, so nothing lands on freed memory -- but only once the
    // destructor has run. The dangerous moment is earlier: the recognizer's
    // worker thread executing this lambda, and calling invokeMethod on a `this`
    // that is being destroyed underneath it.
    //
    // That is why the destructor stops the recognizer rather than relying on
    // this, and why AppController releases the recognizer before the session.
    // A returned stop() is the boundary; everything after it is arithmetic.
    if (!m_recognizer.start(
            [this](const runtime::RecognitionResult& result) {
                QMetaObject::invokeMethod(
                    this,
                    [this, result] {
                        onResult(result);
                    },
                    Qt::QueuedConnection);
            },
            [this](runtime::RecognitionEnd reason, std::string_view detail) {
                const std::string reason_text(detail);
                QMetaObject::invokeMethod(
                    this,
                    [this, reason, reason_text] {
                        onEnd(reason, reason_text);
                    },
                    Qt::QueuedConnection);
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
    // The mock is driven from this thread, so its results arrive here directly
    // and the queued hop the start callbacks make is a no-op for them.
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

void ListenSession::onEnd(runtime::RecognitionEnd reason, const std::string& detail) {
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
    // Every end reason is reported, including the ones that are not failures.
    // A session that ends without saying why leaves the microphone button in a
    // state the user cannot interpret, which is worse than an error message
    // about something that did not go wrong.
    emit listeningEnded(QString::fromStdString(
        detail.empty() ? std::string(runtime::toString(reason)) : detail));
}

} // namespace kestrel::app
