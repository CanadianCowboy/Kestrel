#include "app/generationworker.h"

#include <algorithm>

#include <QMetaObject>
#include <QStringDecoder>

#include "core/agenttools.h"

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
            {
                std::lock_guard<std::mutex> lock(m_toolCallMutex);
                m_toolCall.reset();
            }
            // Tool calls are hidden from the chat and the voice. Visible text is
            // emitted up to `emitted`; a partial tag at the end of a token is held
            // back, so a tag split across tokens is still hidden. Emitted slices
            // end at an ASCII '<' or at the end of the text. One decoder spans the
            // whole reply, so a character split across two token pieces is
            // completed in the next slice rather than replaced.
            QStringDecoder decoder(QStringDecoder::Utf8);
            std::string written;
            std::size_t emitted = 0;
            bool sawCall = false;
            constexpr std::string_view kOpenTag = "<tool_call>";
            const auto emitSlice = [this, requestId, &decoder](std::string_view slice) {
                if (slice.empty()) {
                    return;
                }
                const QString text = decoder(QByteArrayView(slice.data(), slice.size()));
                if (!text.isEmpty()) {
                    emit tokenReady(requestId, text);
                }
            };
            bool succeeded = false;
            bool stoppedForToolCall = false;
            QString failure = QStringLiteral("Backend returned without completion");

            // Emit on the worker thread. Receivers queue the signals to the UI;
            // queuing onto this busy worker would hold every token until the
            // blocking generate() call returns. Preserve split UTF-8 characters
            // across native token pieces rather than decoding each in isolation.
            m_backend->generate(
                request,
                [this, &written, &emitted, &sawCall, &stoppedForToolCall, kOpenTag, emitSlice](std::string_view token) {
                    if (m_cancelRequested.load(std::memory_order_acquire) || stoppedForToolCall) {
                        return;
                    }
                    written.append(token.data(), token.size());
                    const auto checkCompleteCall = [this, &written, &emitted, &stoppedForToolCall]() {
                        if (auto call = core::findModelToolCall(std::string_view(written).substr(emitted))) {
                            // Stop the backend as soon as the tool call is whole so
                            // the tool executes immediately instead of waiting for
                            // trailing tokens the model would otherwise invent.
                            std::lock_guard<std::mutex> lock(m_toolCallMutex);
                            m_toolCall = std::move(call);
                            stoppedForToolCall = true;
                            m_backend->cancel();
                            return true;
                        }
                        return false;
                    };
                    if (sawCall) {
                        checkCompleteCall();
                        return;
                    }
                    // A base model often writes the call as a bare JSON object
                    // rather than in the tagged form. A reply that starts with
                    // an object is held until the object closes, so it is either
                    // executed or shown as the text it is: streaming it as prose
                    // and then acting on it would show an answer that is later
                    // replaced. Only the reply's first object is considered.
                    if (emitted == 0) {
                        if (checkCompleteCall()) {
                            sawCall = true;
                            return;
                        }
                        if (core::modelReplyNeedsMoreJson(written)) {
                            return;
                        }
                    }
                    const std::size_t open = written.find(kOpenTag, emitted);
                    if (open != std::string::npos) {
                        emitSlice(std::string_view(written).substr(emitted, open - emitted));
                        emitted = open;
                        sawCall = true;
                        checkCompleteCall();
                        return;
                    }
                    std::size_t safe = written.size();
                    const std::size_t longest = std::min(kOpenTag.size() - 1, written.size() - emitted);
                    for (std::size_t k = longest; k > 0; --k) {
                        if (written.compare(written.size() - k, k, kOpenTag.data(), k) == 0) {
                            safe = written.size() - k;
                            break;
                        }
                    }
                    emitSlice(std::string_view(written).substr(emitted, safe - emitted));
                    emitted = safe;
                },
                [&succeeded, &failure](bool success, std::string_view error) {
                    succeeded = success;
                    failure = QString::fromUtf8(error.data(), error.size());
                });
            if (m_cancelRequested.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lock(m_toolCallMutex);
                m_toolCall.reset();
                succeeded = false;
                failure = QStringLiteral("Generation stopped");
            } else if (stoppedForToolCall) {
                succeeded = true;
                failure.clear();
            } else if (sawCall) {
                if (auto call = core::findModelToolCall(std::string_view(written).substr(emitted))) {
                    // Nothing after a call is shown, so a model that appends a
                    // made-up result to its own call cannot present it as real.
                    std::lock_guard<std::mutex> lock(m_toolCallMutex);
                    m_toolCall = std::move(call);
                }
            } else {
                emitSlice(std::string_view(written).substr(emitted));
            }
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

std::optional<core::ModelToolCall> GenerationWorker::takeToolCall() {
    std::lock_guard<std::mutex> lock(m_toolCallMutex);
    std::optional<core::ModelToolCall> call = std::move(m_toolCall);
    m_toolCall.reset();
    return call;
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
