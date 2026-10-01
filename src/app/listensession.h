#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>
#include <string>

#include "runtime/speechrecognizer.h"

namespace kestrel::app {

// Turns a stream of recognition events into one thing the app cares about: the
// user said something.
//
// It exists because the interesting part of speech input is not the audio, it is
// the relationship with everything else that is already running. A spoken
// request has to interrupt a reply mid-sentence, has to show partial words as
// they arrive so the user knows the microphone works, and has to stay out of the
// way when the user is halfway through typing. None of that belongs in the
// recognizer, and none of it belongs in the controller either; it is the seam
// between the two.
class ListenSession final : public QObject {
    Q_OBJECT

public:
    // `recognizer` is borrowed. The mock is used by tests and by the preview
    // build; a platform adapter can be supplied without changing anything here.
    explicit ListenSession(runtime::SpeechRecognizer& recognizer, QObject* parent = nullptr);
    // Stops the borrowed recognizer and waits for any callback already in
    // flight before this QObject receiver is destroyed. A session token also
    // makes already-queued Qt deliveries inert after stop/restart.
    ~ListenSession() override;

    // Begins listening. Refuses when the recognizer is unavailable, and says
    // why in `error`, because "the microphone is not working" and "something is
    // already listening" need different things said to the user.
    bool startListening(QString& error);

    // Stops listening and discards any partial text. This is the explicit
    // cancel: a half-spoken phrase is never submitted.
    void stopListening();

    // Abandons the phrase in progress because the user did something else --
    // started typing, or pressed Escape. Distinct from stopListening because the
    // text is thrown away rather than submitted.
    void abandon();

    [[nodiscard]] bool listening() const noexcept;

    // Drives a pull-style recognizer. A push-style one needs no timer and no
    // call to this, which is the point of keeping the session ignorant of which
    // kind it has.
    void poll();

    // Partial text as recognized so far. Shown to the user so they can see and
    // correct their own words before committing to them.
    [[nodiscard]] QString partialText() const;

    // How often partials are re-requested from a pull-style recognizer. Short
    // enough to feel live, long enough not to spin the interface.
    void setPartialIntervalMs(int ms);

signals:
    // A complete phrase was recognized and should be treated exactly as if the
    // user had typed it: same barge-in, same turn, same audit trail.
    void utteranceFinal(const QString& text, double confidence);
    // Recognition stopped for a reason that is not a completed phrase. The
    // session has already stopped listening by the time this is emitted.
    void listeningEnded(const QString& reason);
    // Partial text changed, including being cleared.
    void partialChanged();

private:
    // Both run on this object's thread, whatever thread the recognizer
    // delivers on. See startListening(). Stale queued callbacks are discarded
    // by their session id.
    void onResult(quint64 sessionId, const runtime::RecognitionResult& result);
    void onEnd(quint64 sessionId, runtime::RecognitionEnd reason,
               const std::string& detail);

    runtime::SpeechRecognizer& m_recognizer;
    QTimer m_poll;
    QString m_partial;
    bool m_listening = false;
    // Set once a phrase is complete. Guards the final signal against a
    // recognizer that reports both a final result and an end event: the phrase
    // is submitted once, not twice.
    bool m_submitted = false;
    quint64 m_sessionId = 0;
};

} // namespace kestrel::app
