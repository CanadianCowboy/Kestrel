#include "app/generationworker.h"

#include <QMetaObject>
#include <QStringDecoder>

namespace kestrel::app {

GenerationWorker::GenerationWorker(runtime::ModelBackend* backend, QObject* parent)
    : QObject(parent), m_backend(backend) {}

void GenerationWorker::start(quint64 requestId,
                             const std::vector<runtime::ChatMessage>& messages,
                             float temperature,
                             int maxTokens, bool addAssistantCue) {
    m_cancelRequested.store(false, std::memory_order_release);

    // Queued so this returns immediately; the backend call happens on the
    // worker thread once its event loop picks the call up. The messages are
    // copied into the lambda because the caller's vector outlives neither the
    // queue nor this thread.
    QMetaObject::invokeMethod(
        this,
        [this, requestId, messages, temperature, maxTokens, addAssistantCue] {
            if (m_cancelRequested.load(std::memory_order_acquire)) {
                emit finished(requestId, false, QStringLiteral("Generation stopped"));
                return;
            }
            runtime::GenerationRequest request{messages, temperature, maxTokens, addAssistantCue};
            QStringDecoder decoder(QStringDecoder::Utf8);
            bool succeeded = false;
            QString failure = QStringLiteral("Backend returned without completion");

            // Emit on the worker thread. Receivers queue the signals to the UI;
            // queuing onto this busy worker would hold every token until the
            // blocking generate() call returns. Preserve split UTF-8 characters
            // across native token pieces rather than decoding each in isolation.
            m_backend->generate(
                request,
                [this, requestId, &decoder](std::string_view token) {
                    if (m_cancelRequested.load(std::memory_order_acquire)) {
                        return;
                    }
                    const QString text = decoder(QByteArrayView(token.data(), token.size()));
                    if (!text.isEmpty()) {
                        emit tokenReady(requestId, text);
                    }
                },
                [&succeeded, &failure](bool success, std::string_view error) {
                    succeeded = success;
                    failure = QString::fromUtf8(error.data(), error.size());
                });
            // Only release the owner's busy/lifetime guard after generate has
            // returned and its internal locks are released. Emitting from the
            // completion callback can otherwise race a backend replacement.
            emit finished(requestId, succeeded, failure);
        },
        Qt::QueuedConnection);
}

void GenerationWorker::start(quint64 requestId, const QString& prompt,
                             float temperature, int maxTokens) {
    start(requestId, {runtime::ChatMessage{runtime::Role::User, prompt.toStdString()}},
          temperature, maxTokens);
}

void GenerationWorker::cancel() {
    // Called directly from the UI thread, deliberately not queued: the worker
    // is blocked inside generate() and would not service a queued call until
    // generation returned, which is exactly when cancelling is too late.
    m_cancelRequested.store(true, std::memory_order_release);
    if (m_backend != nullptr) {
        m_backend->cancel();
    }
}

/// Replaces the borrowed backend pointer; the caller must ensure no generation is in flight.
/// The replacement must remain alive for all worker calls that use it.
void GenerationWorker::setBackend(runtime::ModelBackend* backend) {
    // Borrowed, exactly as in the constructor. The caller owns the lifetime and
    // is responsible for having no generation in flight.
    m_backend = backend;
}

} // namespace kestrel::app
