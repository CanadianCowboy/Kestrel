#pragma once

#include <QObject>
#include <QString>

#include <atomic>

#include "runtime/modelbackend.h"

namespace kestrel::app {

// Runs ModelBackend::generate() on a dedicated thread and marshals its
// callbacks back to the UI thread as queued signals.
//
// Why this exists: generation is a long-running, blocking operation. Calling
// it directly from a QML-invoked slot froze the window for the entire
// response, which the project's own guidelines forbid.
//
// Threading contract:
//   * start() schedules work; the token and finished signals are emitted from
//     the worker thread. Nothing in generate()'s callback chain may touch
//     QML state directly.
//   * cancel() is the one exception. It is called directly from the UI thread
//     and only performs atomic stores plus ModelBackend::cancel(), so it
//     takes effect immediately even though the worker's event loop is busy
//     inside generate() and cannot service a queued call.
//
// Generation is cooperative: a backend must poll its cancellation flag
// between tokens. A backend blocked in a single long GPU call cannot be
// interrupted until that call returns.
//
// Thread ownership: this class deliberately does not own a QThread. The owner
// creates the thread, calls moveToThread() on this object, and shuts the
// thread down after this object is destroyed. Owning it here would mean
// destroying a QObject from a thread other than its own affinity.
class GenerationWorker final : public QObject {
    Q_OBJECT

public:
    // `backend` is borrowed, not owned. It must outlive the worker.
    explicit GenerationWorker(runtime::ModelBackend* backend, QObject* parent = nullptr);

    // Schedules generation and returns immediately.
    void start(quint64 requestId, const QString& prompt, float temperature, int maxTokens);

    // Safe to call from any thread, including while generation is in flight.
    void cancel();

    // Points the worker at a different backend, for when the user loads a
    // model from disk.
    //
    // Not safe while generate() is running: the worker would still be inside
    // the old backend's call, and the owner is about to destroy that backend.
    // The caller must cancel and wait for the in-flight generation to report
    // completion first.
    void setBackend(runtime::ModelBackend* backend);

signals:
    void tokenReady(quint64 requestId, QString token);
    void finished(quint64 requestId, bool success, QString error);

private:
    runtime::ModelBackend* m_backend = nullptr;
    std::atomic<bool> m_cancelRequested{false};
};

} // namespace kestrel::app
