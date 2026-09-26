#include "app/appcontroller.h"

#include "app/generationworker.h"
#include "runtime/backendregistry.h"

#include <QVariantMap>

#include <string_view>

namespace kestrel::app {

namespace {

QString fromStd(const std::string& text) {
    return QString::fromStdString(text);
}

// Rough characters-per-token ratio for English text. Used only for the
// context-fill indicator, which is explicitly an estimate rather than a
// tokenizer count; a real backend's own accounting is preferred when it
// reports a non-zero value.
constexpr int kApproxCharsPerToken = 4;

// Minimum gap between metricsChanged emissions while streaming. Emitting on
// every token would flood the binding with updates faster than the UI repaints.
constexpr qint64 kMetricsIntervalMs = 100;

} // namespace

AppController::AppController(QObject* parent)
    : QObject(parent), m_backend(runtime::selectBackend(runtime::BackendKind::Mock)) {
    m_probe = runtime::probeCuda();

    m_worker = new GenerationWorker(m_backend.get());
    m_worker->moveToThread(&m_generationThread);
    m_generationThread.start();

    // The worker emits from its own thread, so these connections are queued
    // and the slots below run on the UI thread where the QML state lives.
    connect(m_worker, &GenerationWorker::tokenReady,
            this, &AppController::onGenerationToken, Qt::QueuedConnection);
    connect(m_worker, &GenerationWorker::finished,
            this, &AppController::onGenerationFinished, Qt::QueuedConnection);

    rebuildDiagnostics();
}

AppController::~AppController() {
    // Order matters: cancel so a blocked generate() returns, then stop the
    // event loop, then wait. Only once the thread is idle is it safe to
    // destroy an object whose affinity was that thread.
    m_worker->cancel();
    m_generationThread.quit();
    m_generationThread.wait();
    delete m_worker;
}

QVariantList AppController::messages() const {
    return m_messages;
}

QString AppController::conversationTitle() const {
    return m_conversationTitle;
}

QString AppController::backendName() const {
    return fromStd(m_backend->status().backendName);
}

QString AppController::modelName() const {
    return fromStd(m_backend->status().modelName);
}

QString AppController::runtimeDetail() const {
    return fromStd(m_backend->status().detail);
}

bool AppController::generating() const noexcept {
    return m_generating;
}

bool AppController::sidebarOpen() const noexcept {
    return m_sidebarOpen;
}

bool AppController::diagnosticsOpen() const noexcept {
    return m_diagnosticsOpen;
}

double AppController::tokensPerSecond() const noexcept {
    return m_tokensPerSecond;
}

int AppController::tokensGenerated() const noexcept {
    return m_tokensGenerated;
}

QString AppController::contextSummary() const {
    const runtime::RuntimeStatus status = m_backend->status();
    if (status.contextLimit == 0) {
        return QStringLiteral("not reported by this backend");
    }

    int characters = 0;
    for (const QVariant& entry : m_messages) {
        characters +=
            entry.toMap().value(QStringLiteral("content")).toString().size();
    }
    const auto generated =
        static_cast<std::size_t>(characters / kApproxCharsPerToken);
    const std::size_t used = status.contextUsed + generated;

    return QStringLiteral("%1 / %2 tokens")
        .arg(static_cast<qulonglong>(used))
        .arg(static_cast<qulonglong>(status.contextLimit));
}

QString AppController::voiceState() const {
    const core::VoiceResponse* response = m_voice.find(m_activeResponse);
    if (response == nullptr) {
        return QStringLiteral("Idle");
    }
    return QString::fromLatin1(core::toString(response->state()));
}

bool AppController::canPause() const noexcept {
    const core::VoiceResponse* response = m_voice.find(m_activeResponse);
    if (response == nullptr) {
        return false;
    }
    return response->state() == core::ResponseState::Generating ||
           response->state() == core::ResponseState::Speaking;
}

bool AppController::canResume() const noexcept {
    const core::VoiceResponse* response = m_voice.find(m_activeResponse);
    return response != nullptr && response->state() == core::ResponseState::Paused;
}

bool AppController::canBargeIn() const noexcept {
    const core::VoiceResponse* response = m_voice.find(m_activeResponse);
    if (response == nullptr) {
        return false;
    }
    return response->state() == core::ResponseState::Generating ||
           response->state() == core::ResponseState::Speaking;
}

bool AppController::gpuAvailable() const {
    return m_probe.runtime.available && m_probe.selectedDevice() != nullptr;
}

QString AppController::gpuName() const {
    const runtime::CudaDeviceInfo* device = m_probe.selectedDevice();
    if (device == nullptr) {
        return QStringLiteral("No GPU detected");
    }
    return fromStd(device->name);
}

QString AppController::gpuSummary() const {
    const runtime::CudaDeviceInfo* device = m_probe.selectedDevice();
    if (device == nullptr) {
        return QStringLiteral("unavailable");
    }
    return QStringLiteral("sm_%1 · %2")
        .arg(QString::fromStdString(device->computeCapability()),
             QString::fromStdString(device->memorySummary()));
}

QString AppController::gpuDetail() const {
    const runtime::CudaDeviceInfo* device = m_probe.selectedDevice();
    if (device == nullptr) {
        return fromStd(m_probe.runtime.detail);
    }
    return fromStd(runtime::describeDevice(*device));
}

QString AppController::computeCapability() const {
    const runtime::CudaDeviceInfo* device = m_probe.selectedDevice();
    if (device == nullptr) {
        return QString();
    }
    return QStringLiteral("sm_%1")
        .arg(QString::fromStdString(device->computeCapability()));
}

int AppController::gpuDeviceCount() const {
    return static_cast<int>(m_probe.devices.size());
}

QVariantList AppController::runtimeDiagnostics() const {
    return m_runtimeDiagnostics;
}

void AppController::setSidebarOpen(bool open) {
    if (m_sidebarOpen == open) {
        return;
    }
    m_sidebarOpen = open;
    emit sidebarOpenChanged();
}

void AppController::setDiagnosticsOpen(bool open) {
    if (m_diagnosticsOpen == open) {
        return;
    }
    m_diagnosticsOpen = open;
    emit diagnosticsOpenChanged();
}

void AppController::appendMessage(const QString& role, const QString& content) {
    QVariantMap message;
    message.insert(QStringLiteral("role"), role);
    message.insert(QStringLiteral("content"), content);
    m_messages.append(message);
}

void AppController::appendToLastAssistantMessage(const QString& text) {
    if (m_messages.isEmpty()) {
        return;
    }
    QVariantMap assistant = m_messages.last().toMap();
    assistant.insert(QStringLiteral("content"),
                     assistant.value(QStringLiteral("content")).toString() + text);
    m_messages.replace(m_messages.size() - 1, assistant);
    emit messagesChanged();
}

void AppController::resetMetrics() {
    m_tokensGenerated = 0;
    m_tokensPerSecond = 0.0;
    m_lastMetricsPublish = 0;
    m_generationClock.invalidate();
    emit metricsChanged();
}

void AppController::publishMetrics() {
    if (m_generationClock.isValid()) {
        const qint64 elapsed = m_generationClock.elapsed();
        if (elapsed > 0) {
            m_tokensPerSecond =
                static_cast<double>(m_tokensGenerated) * 1000.0 / static_cast<double>(elapsed);
        }
    }
    emit metricsChanged();
}

void AppController::sendMessage(const QString& text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }

    // A new prompt arriving mid-response is a barge-in: the machine preserves
    // what was already spoken, invalidates the in-flight generation so late
    // tokens are rejected as stale, and lets the new response take over.
    if (m_generating) {
        m_voice.interrupt(m_activeResponse, trimmed.toStdString());
        // Replacement: the interrupted response is abandoned rather than
        // resumed, so the new prompt owns the single active timeline.
        const auto replacement =
            m_voice.resolveInterruption(m_activeResponse, core::InterruptionIntent::Replacement);
        if (replacement.has_value() && *replacement != core::kInvalidGenerationId) {
            // Defensive: Replacement currently always discards, so this branch
            // should stay unreachable. If the machine ever changes to preserve
            // the response, its new generation token is adopted here.
            m_activeGeneration = *replacement;
        }
        m_worker->cancel();
        emit voiceChanged();
    }

    appendMessage(QStringLiteral("user"), trimmed);
    appendMessage(QStringLiteral("assistant"), QString());

    if (m_messages.size() == 2) {
        m_conversationTitle = trimmed.left(34);
        emit conversationTitleChanged();
    }
    emit messagesChanged();

    resetMetrics();
    m_generationClock.start();

    m_activeResponse = m_voice.queueResponse(trimmed.toStdString());
    m_activeGeneration = m_voice.beginGeneration(m_activeResponse);
    if (m_activeGeneration == core::kInvalidGenerationId) {
        // Another response is still holding the single active timeline. The
        // text stays visible; the user can retry once it settles.
        appendToLastAssistantMessage(
            QStringLiteral("Kestrel is still finishing an earlier response."));
        emit voiceChanged();
        return;
    }

    m_generating = true;
    m_activeRequestId = m_nextRequestId++;
    emit generatingChanged();
    emit voiceChanged();

    m_worker->start(m_activeRequestId, trimmed, 0.7F, 512);
}

void AppController::onGenerationToken(quint64 requestId, const QString& token) {
    // Ignore anything that is not the response the UI is currently showing.
    if (requestId != m_activeRequestId) {
        return;
    }

    // The state machine decides whether this token is still live. A token that
    // arrives after an interruption is dropped here rather than appended to
    // text the user has already moved past.
    const QByteArray utf8 = token.toUtf8();
    if (!m_voice.appendText(m_activeResponse, m_activeGeneration,
                           std::string_view(utf8.constData(),
                                            static_cast<std::size_t>(utf8.size())))) {
        return;
    }

    appendToLastAssistantMessage(token);
    ++m_tokensGenerated;

    if (m_generationClock.elapsed() - m_lastMetricsPublish >= kMetricsIntervalMs) {
        m_lastMetricsPublish = m_generationClock.elapsed();
        publishMetrics();
    }
}

void AppController::onGenerationFinished(quint64 requestId,
                                         bool success,
                                         const QString& error) {
    if (requestId != m_activeRequestId) {
        return;
    }

    publishMetrics();
    m_generating = false;
    m_generationClock.invalidate();
    emit generatingChanged();

    const core::VoiceResponse* response = m_voice.find(m_activeResponse);
    if (response != nullptr &&
        response->state() == core::ResponseState::Paused) {
        // Paused deliberately: the worker was stopped so playback could halt,
        // but the response is preserved for resume() and must not complete.
        emit voiceChanged();
        return;
    }

    if (!success) {
        if (!error.isEmpty() && error != QStringLiteral("Generation stopped")) {
            m_voice.fail(m_activeResponse, error.toStdString());
            appendToLastAssistantMessage(
                QStringLiteral("\n\n[Generation failed: %1]").arg(error));
        }
        emit voiceChanged();
        return;
    }

    m_voice.finishGeneration(m_activeResponse, m_activeGeneration);

    // There is no audio engine yet, so playback is treated as delivered as
    // soon as generation finishes. That is the text-only fallback the
    // VoiceSession contract describes for exactly this situation.
    m_voice.complete(m_activeResponse);
    emit voiceChanged();
}

void AppController::stopGeneration() {
    if (!m_generating) {
        return;
    }
    m_worker->cancel();
    m_voice.cancel(m_activeResponse);
    m_generating = false;
    m_generationClock.invalidate();
    emit generatingChanged();
    emit voiceChanged();
}

void AppController::pauseConversation() {
    if (!canPause()) {
        return;
    }
    // Pause halts delivery, so the in-flight generation is stopped. The
    // response itself is preserved: resume() reopens generation for whatever
    // text had not been produced yet.
    m_voice.pause(m_activeResponse, QStringLiteral("user paused").toStdString());
    m_worker->cancel();
    emit voiceChanged();
}

void AppController::resumeConversation() {
    if (!canResume()) {
        return;
    }
    const auto generation = m_voice.resume(m_activeResponse);
    if (!generation.has_value()) {
        // Refused because another response owns the timeline.
        emit voiceChanged();
        return;
    }

    m_activeGeneration = *generation;
    if (*generation != core::kInvalidGenerationId) {
        resetMetrics();
        m_generationClock.start();
        m_generating = true;
        m_activeRequestId = m_nextRequestId++;
        emit generatingChanged();
        emit voiceChanged();

        // Continue from the text the response still owes rather than
        // regenerating from scratch.
        const core::VoiceResponse* response = m_voice.find(m_activeResponse);
        const std::string_view remainder =
            response != nullptr ? response->unspokenText() : std::string_view{};
        const QString prompt = QString::fromStdString(std::string(remainder));
        m_worker->start(m_activeRequestId, prompt, 0.7F, 512);
        return;
    }

    // Everything the response needed was already generated; nothing to restart.
    m_voice.complete(m_activeResponse);
    emit voiceChanged();
}

void AppController::newConversation() {
    m_worker->cancel();
    m_messages.clear();
    m_conversationTitle = QStringLiteral("New conversation");
    m_activeResponse = core::kInvalidResponseId;
    m_activeGeneration = core::kInvalidGenerationId;
    m_generating = false;
    m_generationClock.invalidate();
    m_voice = core::VoiceSession{};
    resetMetrics();
    emit messagesChanged();
    emit conversationTitleChanged();
    emit generatingChanged();
    emit voiceChanged();
}

void AppController::refreshRuntime() {
    m_probe = runtime::probeCuda();
    rebuildDiagnostics();
    emit runtimeChanged();
}

void AppController::rebuildDiagnostics() {
    m_runtimeDiagnostics.clear();
    for (const runtime::RuntimeDiagnostic& row : runtime::runtimeDiagnostics(m_probe)) {
        QVariantMap entry;
        entry.insert(QStringLiteral("label"), fromStd(row.label));
        entry.insert(QStringLiteral("value"), fromStd(row.value));
        entry.insert(QStringLiteral("ok"), row.ok);
        m_runtimeDiagnostics.append(entry);
    }
}

} // namespace kestrel::app
