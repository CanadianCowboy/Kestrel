#include "app/appcontroller.h"

#include "app/generationworker.h"
#include "runtime/backendregistry.h"
#include "runtime/llamacppbackend.h"

#include <QClipboard>
#include <QDateTime>
#include <QEventLoop>
#include <QFileInfo>
#include <QGuiApplication>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

#include <algorithm>
#include <string_view>

namespace kestrel::app {

namespace {

constexpr int kTitleLimit = 42;

// The shared prefix: every turn starts with this text, so it is the one part of
// the prompt the backend can decode once and keep resident instead of resending
// with each request.
//
// It is written to make the *shape* of an answer good rather than to make the
// model sound knowledgeable. Two deliberate constraints:
//
//   * Structure is asked for explicitly, because an instruct model left to
//     itself drifts into a single undifferentiated paragraph. Clear sentences,
//     logical connective tissue, and a direct answer before the elaboration
//     are all things the model can actually be steered towards.
//   * Calibre is asked for explicitly, and in the same breath as the request
//     for confidence. A model told to be authoritative with no instruction
//     about doubt will manufacture certainty, which is the one failure a user
//     cannot detect. So the prompt makes an unverified claim worse than no
//     answer, and names the cases where it should stop and say so.
//
// Note what this cannot do: it shapes how a model answers, not what it knows.
// A small local model asked to reason carefully will still reason within its
// capacity, and the prompt should not be mistaken for a way around that.
const QString kDefaultSystemPrompt = QStringLiteral(
    "You are Kestrel, a local desktop assistant running entirely on the user's "
    "own machine.\n"
    "\n"
    "Write well. Answer in complete, well-structured sentences, using clear "
    "paragraphs where the material warrants them. Lead with the direct answer, "
    "then give the reasoning that supports it. Use concrete nouns and active "
    "verbs, connect claims to the evidence for them, and avoid filler, "
    "hedging padding, and restating the question back.\n"
    "\n"
    "Be as confident as your evidence actually supports, and no more. Before "
    "asserting something specific -- a number, a date, a name, an API, a "
    "quotation -- check it against what you actually know. If you are not "
    "sure, say so plainly and say what would settle it. If a question needs "
    "information you do not have, ask for that rather than inventing an "
    "answer. A correct refusal is a better result than a confident invention, "
    "and the user is relying on this to make decisions.\n"
    "\n"
    "If you can genuinely verify something, say how you verified it. If you "
    "cannot, do not imply that you did.");

// How many prior turns are replayed to the model. Bounded because the prompt
// grows with it, and the whole point of the shared prefix is lost if the
// per-turn text swamps the part that is cached.
constexpr std::size_t kHistoryTurns = 8;

// How long the UI will wait for an in-flight generation to unwind before a
// backend swap proceeds anyway. Generous enough for a real decode, short
// enough that a wedged backend cannot freeze the window.
constexpr int kBackendSwapTimeoutMs = 15000;

// Minimum gap between metricsChanged emissions while streaming. Emitting on
// every token would flood the binding with updates faster than the UI repaints.
constexpr qint64 kMetricsIntervalMs = 100;

QString deriveTitle(const QString& text) {
    const QString line = text.section(QLatin1Char('\n'), 0, 0).simplified();
    if (line.size() <= kTitleLimit) {
        return line;
    }
    return line.left(kTitleLimit - 1).trimmed() + QChar(0x2026);
}

} // namespace

AppController::AppController(QObject* parent)
    : QObject(parent), m_backend(runtime::selectBackend(runtime::BackendKind::Mock)) {
    m_messageModel = new MessageModel(this);
    m_conversationModel = new ConversationModel(&m_entries, this);

    m_probe = runtime::probeCuda();
    rebuildDiagnostics();

    m_worker = new GenerationWorker(m_backend.get());
    m_worker->moveToThread(&m_generationThread);
    m_generationThread.start();

    // Declare the shared prefix before any turn runs. The backend decodes it
    // on the first generate() and keeps it from then on.
    m_systemPrompt = kDefaultSystemPrompt;
    m_backend->setSystemPrompt(m_systemPrompt.toStdString());

    // The worker emits from its own thread, so these connections are queued and
    // the slots run on the UI thread where the QML state lives.
    connect(m_worker, &GenerationWorker::tokenReady,
            this, &AppController::onGenerationToken, Qt::QueuedConnection);
    connect(m_worker, &GenerationWorker::finished,
            this, &AppController::onGenerationFinished, Qt::QueuedConnection);

    ConversationEntry* entry = createConversation();
    m_conversationModel->refilter();
    setActiveConversation(entry->id);
}

AppController::~AppController() {
    // Order matters: cancel so a blocked generate() returns, then stop the event
    // loop, then wait. Only once the thread is idle is it safe to destroy an
    // object whose affinity was that thread.
    m_worker->cancel();
    m_generationThread.quit();
    m_generationThread.wait();
    delete m_worker;
}

MessageModel* AppController::messages() const noexcept {
    return m_messageModel;
}

ConversationModel* AppController::conversations() const noexcept {
    return m_conversationModel;
}

int AppController::activeConversationId() const noexcept {
    return m_activeId;
}

QString AppController::conversationTitle() const {
    if (const ConversationEntry* entry = activeEntry()) {
        return QString::fromStdString(entry->conversation.title());
    }
    return {};
}

bool AppController::generating() const noexcept {
    return m_generating;
}

bool AppController::canRegenerate() const noexcept {
    return m_canRegenerate;
}

bool AppController::sidebarOpen() const noexcept {
    return m_sidebarOpen;
}

bool AppController::diagnosticsOpen() const noexcept {
    return m_diagnosticsOpen;
}

QString AppController::searchQuery() const {
    return m_searchQuery;
}

QString AppController::backendName() const {
    return QString::fromStdString(m_backend->status().backendName);
}

QString AppController::modelName() const {
    return QString::fromStdString(m_backend->status().modelName);
}

QString AppController::runtimeDetail() const {
    return QString::fromStdString(m_backend->status().detail);
}

bool AppController::runtimeAvailable() const {
    const runtime::RuntimeStatus status = m_backend->status();
    return status.available && status.modelLoaded;
}

int AppController::contextUsed() const {
    return static_cast<int>(m_backend->status().contextUsed);
}

int AppController::contextLimit() const {
    return static_cast<int>(m_backend->status().contextLimit);
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
    return QStringLiteral("%1 / %2 tokens")
        .arg(static_cast<qulonglong>(status.contextUsed))
        .arg(static_cast<qulonglong>(status.contextLimit));
}

QString AppController::kvCacheSummary() const {
    const runtime::RuntimeStatus status = m_backend->status();
    if (status.kvCacheBytes == 0) {
        // Distinct from zero bytes: this backend cannot account for its cache,
        // which is not the same as the cache being empty.
        return QStringLiteral("not reported by this backend");
    }
    return QStringLiteral("%1 of %2")
        .arg(QString::fromStdString(runtime::formatBytes(status.kvCacheBytesUsed)))
        .arg(QString::fromStdString(runtime::formatBytes(status.kvCacheBytes)));
}

QString AppController::prefixSummary() const {
    const std::size_t resident = m_backend->cachedPrefixTokens();
    if (resident == 0) {
        return m_systemPrompt.isEmpty()
                   ? QStringLiteral("none")
                   : QStringLiteral("not cached — resent every turn");
    }
    return QStringLiteral("%1 tokens, reused")
        .arg(static_cast<qulonglong>(resident));
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
    return canPause();
}

bool AppController::gpuAvailable() const {
    return m_probe.runtime.available && m_probe.selectedDevice() != nullptr;
}

QString AppController::gpuName() const {
    const runtime::CudaDeviceInfo* device = m_probe.selectedDevice();
    if (device == nullptr) {
        return QStringLiteral("No GPU detected");
    }
    return QString::fromStdString(device->name);
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
        return QString::fromStdString(m_probe.runtime.detail);
    }
    return QString::fromStdString(runtime::describeDevice(*device));
}

QString AppController::computeCapability() const {
    const runtime::CudaDeviceInfo* device = m_probe.selectedDevice();
    if (device == nullptr) {
        return QString();
    }
    return QStringLiteral("sm_%1").arg(QString::fromStdString(device->computeCapability()));
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

QString AppController::systemPrompt() const {
    return m_systemPrompt;
}

void AppController::setBackendForTesting(runtime::ModelBackend* backend) {
    if (backend == nullptr) {
        return;
    }
    waitForIdleGeneration(kBackendSwapTimeoutMs);
    m_backend.reset(backend);
    m_worker->setBackend(m_backend.get());
    m_backend->setSystemPrompt(m_systemPrompt.toStdString());
    rebuildDiagnostics();
    emit runtimeChanged();
    emit metricsChanged();
}

void AppController::setSystemPrompt(const QString& text) {
    const QString trimmed = text.trimmed();
    if (trimmed == m_systemPrompt) {
        return;
    }
    m_systemPrompt = trimmed;
    // Declaring a new prefix invalidates the cached one. The backend works out
    // the consequences on its own thread, so this is safe to call from here.
    m_backend->setSystemPrompt(m_systemPrompt.toStdString());
    emit systemPromptChanged();
    publishMetrics();
}

bool AppController::canLoadModel() const {
    // Ask a throwaway instance rather than caching a flag: whether a real model
    // can be loaded is a build-time fact, but a cached copy would go stale the
    // moment the answer is refactored into a runtime check.
    return runtime::LlamaCppBackend{}.status().available;
}

QString AppController::modelPath() const {
    return m_modelPath;
}

QString AppController::modelError() const {
    return m_modelError;
}

void AppController::loadModelFromUrl(const QString& url) {
    const QString path = QUrl(url).toLocalFile();
    if (path.isEmpty()) {
        // A dialog can hand back an empty selection, and a non-file URL (a
        // remote location) has no local path at all. Both are user error, not
        // a crash, so say so rather than passing "" to the loader.
        m_modelError = tr("That is not a local file. Choose a GGUF from disk.");
        emit modelErrorChanged();
        return;
    }

    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        m_modelError = tr("No such file: %1").arg(path);
        emit modelErrorChanged();
        return;
    }
    // Checking the extension first turns a typo into a clear message instead of
    // an opaque loader failure deep inside llama.cpp.
    if (info.suffix().compare(QStringLiteral("gguf"), Qt::CaseInsensitive) != 0) {
        m_modelError = tr("%1 is not a GGUF file.").arg(info.fileName());
        emit modelErrorChanged();
        return;
    }

    // Build and load the candidate before touching the running backend, so a
    // failed attempt leaves the app exactly as it was. Loading a model takes
    // seconds; finding out afterwards that it was the wrong file should not
    // cost the user their current one.
    auto candidate = std::make_unique<runtime::LlamaCppBackend>();
    if (!candidate->status().available) {
        m_modelError = QString::fromStdString(candidate->status().detail);
        emit modelErrorChanged();
        return;
    }

    std::string error;
    if (!candidate->loadModel(path.toStdString(), error)) {
        m_modelError = QString::fromStdString(error);
        emit modelErrorChanged();
        return;
    }

    // The new model is proven loadable. Now it is safe to tear the old one
    // down -- but only once the worker has left it.
    waitForIdleGeneration(kBackendSwapTimeoutMs);
    if (m_generating) {
        m_modelError = tr("A response is still running. Stop it and try again.");
        emit modelErrorChanged();
        return;
    }

    m_modelPath = path;
    m_modelError.clear();
    m_backend = std::move(candidate);
    m_worker->setBackend(m_backend.get());
    // The new context holds none of the old prefix's entries, so re-declare it
    // and let the backend rebuild the cache on the next turn.
    m_backend->clearSharedPrefix();
    m_backend->setSystemPrompt(m_systemPrompt.toStdString());
    rebuildDiagnostics();
    emit modelErrorChanged();
    emit runtimeChanged();
    emit metricsChanged();
}

void AppController::usePreviewBackend() {
    waitForIdleGeneration(kBackendSwapTimeoutMs);
    if (m_generating) {
        m_modelError = tr("A response is still running. Stop it and try again.");
        emit modelErrorChanged();
        return;
    }
    m_modelPath.clear();
    m_modelError.clear();
    m_backend = runtime::selectBackend(runtime::BackendKind::Mock);
    m_worker->setBackend(m_backend.get());
    m_backend->setSystemPrompt(m_systemPrompt.toStdString());
    rebuildDiagnostics();
    emit modelErrorChanged();
    emit runtimeChanged();
    emit metricsChanged();
}

void AppController::waitForIdleGeneration(int timeoutMs) {
    if (!m_generating) {
        return;
    }
    stopGeneration();
    QEventLoop loop;
    const QMetaObject::Connection done =
        connect(this, &AppController::onGenerationFinished, &loop, &QEventLoop::quit);
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
    disconnect(done);
}

void AppController::setSearchQuery(const QString& query) {
    if (m_searchQuery == query) {
        return;
    }
    m_searchQuery = query;
    m_conversationModel->setSearchQuery(query);
    emit searchQueryChanged();
}

void AppController::setDiagnosticsOpen(bool open) {
    if (m_diagnosticsOpen == open) {
        return;
    }
    m_diagnosticsOpen = open;
    emit diagnosticsOpenChanged();
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
            m_tokensPerSecond = static_cast<double>(m_tokensGenerated) * 1000.0 /
                                static_cast<double>(elapsed);
        }
    }
    emit metricsChanged();
}

void AppController::onGenerationToken(quint64 requestId, const QString& token) {
    if (requestId != m_activeRequestId || m_userPaused || m_userStopped) {
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

    m_messageModel->appendToLastMessage(token);
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
    m_generationClock.invalidate();

    if (m_userPaused) {
        // Paused deliberately: the worker was stopped so delivery could halt,
        // but the response is preserved for resume() and must not finalize.
        m_generating = false;
        emit generatingChanged();
        emit voiceChanged();
        return;
    }

    if (m_userStopped) {
        finalizeStream(MessageStatus::Stopped, {});
        return;
    }

    if (!success) {
        if (!error.isEmpty()) {
            m_voice.fail(m_activeResponse, error.toStdString());
        }
        finalizeStream(MessageStatus::Failed, error);
        return;
    }

    m_voice.finishGeneration(m_activeResponse, m_activeGeneration);

    // There is no audio engine yet, so playback is treated as delivered as soon
    // as generation finishes. That is the text-only fallback VoiceSession
    // defines for exactly this situation.
    m_voice.complete(m_activeResponse);
    finalizeStream(MessageStatus::Complete, {});
    emit voiceChanged();
}

void AppController::startGeneration(const QString& userText) {
    m_messageModel->appendMessage(core::MessageRole::Assistant, {}, MessageStatus::Streaming);
    m_generating = true;
    m_userStopped = false;
    m_userPaused = false;

    resetMetrics();
    m_generationClock.start();

    m_activeResponse = m_voice.queueResponse(userText.toStdString());
    m_activeGeneration = m_voice.beginGeneration(m_activeResponse);
    m_activeRequestId = m_nextRequestId++;

    emit generatingChanged();
    emit voiceChanged();
    refreshCanRegenerate();
    touchActiveConversation();

    if (m_activeGeneration == core::kInvalidGenerationId) {
        // Another response still owns the single active timeline.
        finalizeStream(MessageStatus::Failed,
                       QStringLiteral("An earlier response is still active."));
        return;
    }

    // The voice timeline tracks the user's words; the model gets the assembled
    // conversation. The system prompt is not in it -- the backend holds that as
    // a cached prefix, and repeating it here would undo the caching.
    m_worker->start(m_activeRequestId, buildMessages(userText), 0.7F, 512);
}

QString AppController::buildPrompt(const QString& userText) const {
    QString prompt;
    const ConversationEntry* entry = activeEntry();
    if (entry != nullptr) {
        const auto& messages = entry->conversation.messages();
        // Walk backwards so the limit keeps the most recent turns, which are
        // the ones the current question actually depends on.
        const std::size_t firstUsable =
            messages.size() > kHistoryTurns ? messages.size() - kHistoryTurns : 0;
        for (std::size_t i = firstUsable; i < messages.size(); ++i) {
            const core::Message& message = messages[i];
            // The empty assistant placeholder for this turn is already in the
            // conversation and would only add a dangling label.
            if (message.content.empty()
                || (message.role != core::MessageRole::User
                    && message.role != core::MessageRole::Assistant)) {
                continue;
            }
            if (!prompt.isEmpty()) {
                prompt += QLatin1Char('\n');
            }
            prompt += message.role == core::MessageRole::User ? QStringLiteral("User: ")
                                                               : QStringLiteral("Assistant: ");
            prompt += QString::fromStdString(message.content);
        }
    }
    if (!prompt.isEmpty()) {
        prompt += QLatin1Char('\n');
    }
    prompt += QStringLiteral("Assistant:");
    return prompt;
}

std::vector<runtime::ChatMessage> AppController::buildMessages(const QString& userText) const {
    std::vector<runtime::ChatMessage> messages;
    const ConversationEntry* entry = activeEntry();
    if (entry != nullptr) {
        const auto& history = entry->conversation.messages();
        // Walk backwards so the limit keeps the most recent turns, which are
        // the ones the current question actually depends on.
        const std::size_t firstUsable =
            history.size() > kHistoryTurns ? history.size() - kHistoryTurns : 0;
        for (std::size_t i = firstUsable; i < history.size(); ++i) {
            const core::Message& message = history[i];
            // The empty assistant placeholder for this turn is already in the
            // conversation and would only add a dangling label.
            if (message.content.empty()) {
                continue;
            }
            runtime::Role role = runtime::Role::User;
            switch (message.role) {
                case core::MessageRole::User:
                    role = runtime::Role::User;
                    break;
                case core::MessageRole::Assistant:
                    role = runtime::Role::Assistant;
                    break;
                case core::MessageRole::System:
                    role = runtime::Role::System;
                    break;
                case core::MessageRole::Tool:
                    role = runtime::Role::Tool;
                    break;
            }
            messages.push_back(runtime::ChatMessage{role, message.content});
        }
    }
    // The shared system prompt is not added here. It is the backend's cached
    // prefix, and repeating it in the conversation would both undo the caching
    // and let the renderer place it somewhere the model was not trained to
    // expect it.
    messages.push_back(runtime::ChatMessage{runtime::Role::User, userText.toStdString()});
    return messages;
}

void AppController::finalizeStream(MessageStatus status, const QString& note) {
    m_messageModel->setLastMessageStatus(status, note);
    m_generating = false;
    m_userPaused = false;
    m_userStopped = false;
    m_generationClock.invalidate();
    emit generatingChanged();
    touchActiveConversation();
    emit runtimeChanged();
    emit metricsChanged();
    emit voiceChanged();
    refreshCanRegenerate();
}

void AppController::sendMessage(const QString& text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || !runtimeAvailable()) {
        return;
    }
    ConversationEntry* entry = activeEntry();
    if (entry == nullptr) {
        return;
    }

    if (m_generating) {
        // A new prompt mid-response is a barge-in: preserve what was already
        // delivered, invalidate the in-flight generation so late tokens are
        // rejected as stale, and hand the timeline to the new prompt.
        m_voice.interrupt(m_activeResponse, trimmed.toStdString());
        // Replacement abandons the interrupted response, so there is no
        // replacement generation to adopt. The value is still consumed rather
        // than discarded so a future intent that does return one cannot slip
        // through unnoticed.
        static_cast<void>(
            m_voice.resolveInterruption(m_activeResponse, core::InterruptionIntent::Replacement));
        m_userStopped = true;
        m_worker->cancel();
        emit voiceChanged();
    }

    const bool firstMessage = entry->conversation.size() == 0;
    m_messageModel->appendMessage(core::MessageRole::User, trimmed, MessageStatus::Complete);
    if (firstMessage) {
        entry->conversation.setTitle(deriveTitle(trimmed).toStdString());
        emit activeConversationChanged();
    }

    startGeneration(trimmed);
}

void AppController::stopGeneration() {
    if (!m_generating) {
        return;
    }
    m_userStopped = true;
    m_worker->cancel();
    m_voice.cancel(m_activeResponse);
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
    m_userPaused = true;
    m_worker->cancel();
    emit voiceChanged();
}

void AppController::resumeConversation() {
    if (!canResume()) {
        return;
    }
    const auto generation = m_voice.resume(m_activeResponse);
    if (!generation.has_value()) {
        emit voiceChanged();
        return;
    }

    m_activeGeneration = *generation;
    m_userPaused = false;
    m_generationClock.invalidate();

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
        m_worker->start(m_activeRequestId,
                        {runtime::ChatMessage{runtime::Role::User, std::string(remainder)}}, 0.7F, 512);
        return;
    }

    // Everything the response needed was already generated; nothing to restart.
    m_voice.complete(m_activeResponse);
    finalizeStream(MessageStatus::Complete, {});
}

void AppController::newConversation() {
    if (m_generating) {
        stopGeneration();
    }
    if (ConversationEntry* current = activeEntry();
        current != nullptr && current->conversation.size() == 0) {
        setSearchQuery({});
        return; // an empty conversation is already waiting
    }

    ConversationEntry* entry = createConversation();
    setSearchQuery({});
    m_conversationModel->refilter();
    setActiveConversation(entry->id);
}

void AppController::selectConversation(int id) {
    if (id == m_activeId || findEntry(id) == nullptr) {
        return;
    }
    if (m_generating) {
        stopGeneration();
    }
    setActiveConversation(id);
}

void AppController::renameConversation(int id, const QString& title) {
    const QString trimmed = title.simplified();
    ConversationEntry* entry = findEntry(id);
    if (entry == nullptr || trimmed.isEmpty()
        || trimmed == QString::fromStdString(entry->conversation.title())) {
        return;
    }
    entry->conversation.setTitle(trimmed.toStdString());
    m_conversationModel->notifyEntryChanged(id);
    if (id == m_activeId) {
        emit activeConversationChanged();
    }
}

void AppController::deleteConversation(int id) {
    const auto it = std::find_if(m_entries.begin(), m_entries.end(),
                                 [id](const auto& entry) { return entry->id == id; });
    if (it == m_entries.end()) {
        return;
    }

    const bool wasActive = (id == m_activeId);
    if (wasActive && m_generating) {
        stopGeneration();
    }
    if (wasActive) {
        m_messageModel->setEntry(nullptr);
    }

    const auto index = static_cast<std::size_t>(std::distance(m_entries.begin(), it));
    m_entries.erase(it);
    m_conversationModel->refilter();

    if (!wasActive) {
        return;
    }
    if (m_entries.empty()) {
        ConversationEntry* entry = createConversation();
        m_conversationModel->refilter();
        setActiveConversation(entry->id);
    } else {
        const std::size_t next = std::min(index, m_entries.size() - 1);
        setActiveConversation(m_entries[next]->id);
    }
}

void AppController::regenerateLastResponse() {
    if (m_generating) {
        return;
    }
    ConversationEntry* entry = activeEntry();
    if (entry == nullptr) {
        return;
    }
    const auto& messages = entry->conversation.messages();
    if (messages.empty() || messages.back().role != core::MessageRole::Assistant) {
        return;
    }

    QString prompt;
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->role == core::MessageRole::User) {
            prompt = QString::fromStdString(it->content);
            break;
        }
    }
    if (prompt.isEmpty()) {
        return;
    }

    m_messageModel->removeLastMessage();
    startGeneration(prompt);
}

void AppController::copyToClipboard(const QString& text) const {
    if (QClipboard* clipboard = QGuiApplication::clipboard()) {
        clipboard->setText(text);
    }
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
        entry.insert(QStringLiteral("label"), QString::fromStdString(row.label));
        entry.insert(QStringLiteral("value"), QString::fromStdString(row.value));
        entry.insert(QStringLiteral("ok"), row.ok);
        m_runtimeDiagnostics.append(entry);
    }
}

ConversationEntry* AppController::findEntry(int id) noexcept {
    for (const auto& entry : m_entries) {
        if (entry->id == id) {
            return entry.get();
        }
    }
    return nullptr;
}

ConversationEntry* AppController::activeEntry() noexcept {
    return findEntry(m_activeId);
}

const ConversationEntry* AppController::activeEntry() const noexcept {
    for (const auto& entry : m_entries) {
        if (entry->id == m_activeId) {
            return entry.get();
        }
    }
    return nullptr;
}

ConversationEntry* AppController::createConversation() {
    auto entry = std::make_unique<ConversationEntry>();
    entry->id = m_nextId++;
    entry->updatedAt = QDateTime::currentDateTime();
    ConversationEntry* raw = entry.get();
    m_entries.insert(m_entries.begin(), std::move(entry)); // newest first
    return raw;
}

void AppController::setActiveConversation(int id) {
    m_activeId = id;
    m_messageModel->setEntry(findEntry(id));
    emit activeConversationChanged();
    refreshCanRegenerate();
}

void AppController::touchActiveConversation() {
    if (ConversationEntry* entry = activeEntry()) {
        entry->updatedAt = QDateTime::currentDateTime();
        m_conversationModel->notifyEntryChanged(entry->id);
    }
}

void AppController::refreshCanRegenerate() {
    bool can = false;
    if (!m_generating) {
        if (const ConversationEntry* entry = activeEntry()) {
            const auto& messages = entry->conversation.messages();
            can = !messages.empty() && messages.back().role == core::MessageRole::Assistant
                  && std::any_of(messages.begin(), messages.end(), [](const core::Message& m) {
                         return m.role == core::MessageRole::User;
                     });
        }
    }
    if (can != m_canRegenerate) {
        m_canRegenerate = can;
        emit canRegenerateChanged();
    }
}

void AppController::seedDemoContent(bool startLiveStream) {
    const int placeholderId = m_activeId;
    const QDateTime now = QDateTime::currentDateTime();

    const auto addMessage = [](ConversationEntry* entry, core::MessageRole role,
                               const char* text, MessageStatus status,
                               const QString& note = {}) {
        entry->conversation.addMessage(role, text);
        entry->extras.push_back({status, note});
    };

    ConversationEntry* flight = createConversation();
    flight->conversation.setTitle("Why kestrels hover so well");
    addMessage(flight, core::MessageRole::User,
               "Why can kestrels hover in place while hunting?", MessageStatus::Complete);
    addMessage(flight, core::MessageRole::Assistant,
               "Kestrels hover by flying into the wind at exactly the speed it pushes them "
               "back, so their ground speed drops to zero. Constant micro-adjustments of the "
               "wings and fanned tail cancel the gusts, and the head stays almost perfectly "
               "still, which is what lets them track voles in the grass below.",
               MessageStatus::Complete);
    flight->updatedAt = now.addDays(-2);

    ConversationEntry* cmake = createConversation();
    cmake->conversation.setTitle("Optional TensorRT backend in CMake");
    addMessage(cmake, core::MessageRole::User,
               "What's a clean way to keep the TensorRT backend optional in CMake?",
               MessageStatus::Complete);
    addMessage(cmake, core::MessageRole::Assistant,
               "Gate it behind an option such as KESTREL_ENABLE_TENSORRT, resolve the SDK "
               "with find_path and find_library inside the adapter target, and",
               MessageStatus::Stopped);
    cmake->updatedAt = now.addDays(-1);

    ConversationEntry* vram = createConversation();
    vram->conversation.setTitle("VRAM headroom for a 4096 context");
    addMessage(vram, core::MessageRole::User,
               "How much VRAM headroom should I keep for a 4096-token context window?",
               MessageStatus::Complete);
    addMessage(vram, core::MessageRole::Assistant,
               "Budget the weights first, then the KV cache, which grows roughly linearly "
               "with context length. For a 7B model at FP16 that is on the order of half a "
               "gigabyte at 4096 tokens, so keeping about one gigabyte free above the "
               "weights leaves room for the cache, activations, and fragmentation.",
               MessageStatus::Complete);
    vram->updatedAt = now.addSecs(-3600);

    // Retire the constructor's placeholder if it is still empty.
    if (ConversationEntry* placeholder = findEntry(placeholderId);
        placeholder != nullptr && placeholder->conversation.size() == 0) {
        m_messageModel->setEntry(nullptr);
        std::erase_if(m_entries, [placeholderId](const auto& entry) {
            return entry->id == placeholderId;
        });
    }

    m_conversationModel->refilter();
    setActiveConversation(m_entries.front()->id);

    if (startLiveStream) {
        newConversation();
        sendMessage("Give me a one-line status message I can show while the model warms up.");
    }
}

} // namespace kestrel::app
