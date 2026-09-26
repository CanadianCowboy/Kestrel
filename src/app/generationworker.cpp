#include "app/generationworker.h"

#include <QMetaObject>

namespace kestrel::app {

GenerationWorker::GenerationWorker(runtime::ModelBackend* backend, QObject* parent)
    : QObject(parent), m_backend(backend) {}

void GenerationWorker::start(quint64 requestId,
                             const std::vector<runtime::ChatMessage>& messages,
                             float temperature,
                             int maxTokens) {
    m_cancelRequested.store(false, std::memory_order_release);

    // Queued so this returns immediately; the backend call happens on the
    // worker thread once its event loop picks the call up. The messages are
    // copied into the lambda because the caller's vector outlives neither the
    // queue nor this thread.
    QMetaObject::invokeMethod(
        this,
        [this, requestId, messages, temperature, maxTokens] {
            runtime::GenerationRequest request{messages, temperature, maxTokens};

            // The callbacks run on the worker thread. Each hop back to the UI
            // thread is an explicit queued invocation: touching QML state from
            // here would be a cross-thread write.
            m_backend->generate(
                request,
                [this, requestId](std::string_view token) {
                    if (m_cancelRequested.load(std::memory_order_acquire)) {
                        return;
                    }
                    const QString text =
                        QString::fromUtf8(token.data(), static_cast<int>(token.size()));
                    QMetaObject::invokeMethod(
                        this,
                        [this, requestId, text] { emit tokenReady(requestId, text); },
                        Qt::QueuedConnection);
                },
                [this, requestId](bool success, std::string_view error) {
                    const QString message = QString::fromStdString(std::string(error));
                    QMetaObject::invokeMethod(
                        this,
                        [this, requestId, success, message] {
                            emit finished(requestId, success, message);
                        },
                        Qt::QueuedConnection);
                });
        },
        Qt::QueuedConnection);
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

void GenerationWorker::setBackend(runtime::ModelBackend* backend) {
    // Borrowed, exactly as in the constructor. The caller owns the lifetime and
    // is responsible for having no generation in flight.
    m_backend = backend;
}

} // namespace kestrel::app
