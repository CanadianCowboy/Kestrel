#include "app/appcontroller.h"

#include "app/generationworker.h"
#include "app/listensession.h"
#include "app/speechsynthesizer.h"
#if KESTREL_HAS_QT_MULTIMEDIA
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#endif
#include "runtime/backendregistry.h"
#include "runtime/llamacppbackend.h"
#include "runtime/ortgenaibackend.h"

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

// How often the controller offers the idle loop a moment to think. The loop
// decides whether it is this tick's turn; the timer only keeps time passing.
constexpr int kIdleTickMs = 700;

// How long Kestrel must be left alone before the loop starts, how long before
// it is worth greeting someone back, and how long an anticipatory line holds
// the status line before the activity whisper takes over. The last one is
// generous because a line nobody had time to read is not a line.
constexpr std::uint64_t kIdleQuietPeriodMs = 6000;
constexpr std::uint64_t kReturnGreetingMs = 45000;
constexpr qint64 kWhisperHoldMs = 9000;

// Responses longer than this earn "Would you like me to continue?" rather than
// a closing prompt, because at this size the answer really is unfinished.
constexpr int kLongResponseChars = 420;

// The prewarm request. One short, unremarkable generation whose tokens are
// discarded: the point is that the weights and the KV cache are resident and
// the clocks are up, not that anything was said.
const QString kWarmupPrompt = QStringLiteral("Assistant: Ready.");
constexpr int kWarmupMaxTokens = 8;
// How much of the session a background summary is shown, and how long it is
// allowed to be. Both bounded on purpose: this runs on a timer the user did not
// start, and an unbounded summary of an unbounded transcript is a bill nobody
// agreed to.
constexpr int kSummaryMaterialChars = 4000;
constexpr int kSummaryMaxTokens = 96;

QString deriveTitle(const QString& text) {
    const QString line = text.section(QLatin1Char('\n'), 0, 0).simplified();
    if (line.size() <= kTitleLimit) {
        return line;
    }
    return line.left(kTitleLimit - 1).trimmed() + QChar(0x2026);
}

} // namespace

/// Initializes preview mode, the persona-backed system prompt, a generation worker,
/// the presence projection, the idle loop, and a conversation.
AppController::AppController(QObject* parent)
    : QObject(parent)
    , m_backend(runtime::selectBackend(runtime::BackendKind::Mock))
    , m_idle(m_persona) {
    // Declared, not enabled. The registry is populated so the interface can
    // show what exists, and a tool stays inert until the user both switches it
    // on and grants what it declared.
    m_idleTools.declare(core::indexThreadsDeclaration());
    m_idleTools.declare(core::summariseSessionDeclaration());
    m_messageModel = new MessageModel(this);
    m_conversationModel = new ConversationModel(&m_entries, this);

    m_probe = runtime::probeCuda();
    refreshCachedRuntime();
    rebuildDiagnostics();

    m_worker = new GenerationWorker(m_backend.get());
    m_worker->moveToThread(&m_generationThread);
    m_generationThread.start();

    // Declare the shared prefix before any turn runs. The backend decodes it
    // on the first generate() and keeps it from then on. The persona's presence
    // line is part of it, and is derived from the fixed tone profile rather
    // than the drifting dials, so it stays byte-identical every turn and the
    // cache behind it is never thrown away.
    m_systemPrompt = kDefaultSystemPrompt + QLatin1Char('\n')
        + QString::fromStdString(m_persona.systemPromptFragment());
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

    // The clock every core subsystem is handed, so none of them reaches for a
    // clock of its own and the whole thing stays testable.
    m_clock.start();
    m_presence.setNow(0);
    m_presence.applyPersona(m_persona.state());
    m_idle.setQuietPeriodMs(kIdleQuietPeriodMs);
    m_idle.noteActivity(0);
    noteAssistant(core::AssistantAction::Idle);

    connect(&m_idleTimer, &QTimer::timeout, this, &AppController::onIdleTick);
    m_idleTimer.start(kIdleTickMs);

    // Audio is optional and its absence changes nothing about how a turn runs.
    // The synthesizer still exists; it reports itself unavailable and the
    // text-only path below is what the app takes.
    m_speech = std::make_unique<SpeechSynthesizer>(this);

#if KESTREL_HAS_QT_MULTIMEDIA
    // A local voice, if one has been installed. Both engines are preferred over
    // the platform voice, because the platform voice on a stock Windows install
    // is a recording from the early 2000s and a better one is a single download
    // away. Found by looking rather than by asking, and absent without complaint:
    // the platform voice underneath is a perfectly good fallback.
    m_localVoices = std::make_unique<LocalVoiceEngines>(LocalVoiceEngines::discover());
    // An explicit choice wins, and an impossible one is ignored rather than
    // fatal: asking for an engine that is not installed should leave the app
    // with the voice it can actually speak, not with no voice at all.
    const QString requested = qEnvironmentVariable("KESTREL_VOICE_ENGINE");
    const QString fallback = m_localVoices->defaultEngineId();
    const QString selected = requested.isEmpty() ? fallback : requested;
    std::unique_ptr<SpeechBackend> localBackend = m_localVoices->create(selected);
    if (localBackend == nullptr && selected != fallback) {
        localBackend = m_localVoices->create(fallback);
        if (localBackend != nullptr) {
            m_speechEngine = fallback;
        }
    } else if (localBackend != nullptr) {
        m_speechEngine = selected;
    }
    if (localBackend != nullptr) {
        m_speech->adoptBackend(std::move(localBackend));
    }
#endif
    connect(m_speech.get(), &SpeechSynthesizer::segmentFinished,
            this, &AppController::onSpeechSegmentFinished);
    connect(m_speech.get(), &SpeechSynthesizer::stopCompleted,
            this, &AppController::onSpeechStopCompleted);
    connect(m_speech.get(), &SpeechSynthesizer::availabilityChanged,
            this, &AppController::onSpeechAvailabilityChanged);
    connect(m_speech.get(), &SpeechSynthesizer::availabilityChanged,
            this, [this](bool) { emit ttsChanged(); });
    connect(m_speech.get(), &SpeechSynthesizer::owedAudioReleased,
            this, &AppController::onOwedAudioReleased);
    connect(m_speech.get(), &SpeechSynthesizer::failed, this, [this](const QString& reason) {
        m_waitingForSpeechBoundary = false;
        m_deferredSpeechRestart = false;
        // A voice that dies mid-response must not take the reply with it: the
        // text is already on screen, so the failure is reported and the
        // remaining clauses are simply not spoken.
        m_speechError = reason;
        m_speaking = false;
        m_hasPendingSegment = false;
        emit ttsChanged();
    });

    // Speech input. The recognizer is chosen for the machine rather than
    // hardcoded: a box with a microphone gets the SAPI 5 adapter, and anything
    // else gets the scripted preview recognizer, so the whole voice loop --
    // partial words, barge-in, submission -- runs everywhere. Which one it is
    // says so through sttDetail(), so a user who cannot dictate is told why
    // rather than left to work it out.
    //
    // KESTREL_SPEECH_INPUT forces either branch, for the two callers that would
    // otherwise be at the mercy of the machine's audio hardware: the test suite,
    // which asserts on partial words only the scripted recognizer produces, and
    // a user who wants the real adapter on a machine the probe says is deaf.
    const QString requestedInput = qEnvironmentVariable("KESTREL_SPEECH_INPUT");
    const runtime::SpeechInput preference =
        requestedInput.compare(QLatin1String("mock"), Qt::CaseInsensitive) == 0
            ? runtime::SpeechInput::Mock
        : requestedInput.compare(QLatin1String("platform"), Qt::CaseInsensitive) == 0
            ? runtime::SpeechInput::Platform
            : runtime::SpeechInput::Auto;
    m_recognizer = runtime::makeBestSpeechRecognizer(preference);
    m_listen = std::make_unique<ListenSession>(*m_recognizer, this);
    connect(m_listen.get(), &ListenSession::utteranceFinal,
            this, &AppController::onUtteranceFinal);
    connect(m_listen.get(), &ListenSession::listeningEnded,
            this, &AppController::onListeningEnded);
    connect(m_listen.get(), &ListenSession::partialChanged, this, [this] {
        emit listeningChanged();
    });
}

bool AppController::startListening() {
    if (m_listen == nullptr) {
        m_listenError = tr("Speech input is not available in this build.");
        emit listeningChanged();
        return false;
    }
    const bool mockRequested = qEnvironmentVariable("KESTREL_SPEECH_INPUT")
        .compare(QLatin1String("mock"), Qt::CaseInsensitive) == 0;
    if (!sttAvailable() && !mockRequested) {
        m_listenError = sttDetail();
        emit listeningChanged();
        return false;
    }
    QString error;
    if (!m_listen->startListening(error)) {
        m_listenError = error;
        emit listeningChanged();
        return false;
    }
    m_listenError.clear();
    // Someone is talking, so the idle loop has to keep quiet even though no
    // message has been sent yet.
    noteActivity();
    m_presence.noteUserAction(core::UserAction::Spoke);
    m_presence.setNow(nowMs());
    m_presence.applyPersona(m_persona.state());
    noteAssistant(core::AssistantAction::Waiting);
    emit listeningChanged();
    return true;
}

void AppController::stopListening() {
    if (m_listen != nullptr) {
        m_listen->stopListening();
    }
    emit listeningChanged();
}

void AppController::abandonListening() {
    if (m_listen != nullptr) {
        m_listen->abandon();
    }
    emit listeningChanged();
}

bool AppController::listening() const noexcept {
    return m_listen != nullptr && m_listen->listening();
}

bool AppController::sttAvailable() const noexcept {
    if (m_recognizer == nullptr || !m_recognizer->available()) {
        return false;
    }
    // The fallback recognizer is always available, because availability is what
    // makes it a usable fallback -- so reporting that as "dictation is ready"
    // offers the user a microphone button that cannot hear them. What it
    // actually does is submit a canned phrase as if the user had said it, which
    // is worse than being unavailable: the app appears to be listening while
    // inventing what was said.
    //
    // So the property means "there is a recognizer here that can hear you",
    // and the preview is reported as what it is in sttDetail(). This is the
    // same overstatement as a backend reporting itself ready with nothing
    // loaded, which is why it is checked here rather than left to the registry.
    return dynamic_cast<const runtime::MockSpeechRecognizer*>(m_recognizer.get()) == nullptr;
}

QString AppController::sttDetail() const {
    return m_recognizer != nullptr ? QString::fromStdString(m_recognizer->detail()) : QString();
}

QString AppController::partialTranscript() const {
    return m_listen != nullptr ? m_listen->partialText() : QString();
}

QString AppController::listenError() const {
    return m_listenError;
}

void AppController::onUtteranceFinal(const QString& text, double confidence) {
    static_cast<void>(confidence);
    emit listeningChanged();
    // The single path a turn starts from. A spoken request is a typed request
    // that happened to arrive by ear, and everything downstream -- the barge-in
    // path, the acknowledgement, the presence projection, the transcript -- is
    // the same because of this one call.
    sendMessage(text);
}

void AppController::onListeningEnded(const QString& reason) {
    m_listenError = reason;
    emit listeningChanged();
}

AppController::~AppController() {
    // The listen session goes first, then the recognizer, and this is not a
    // style preference.
    //
    // m_listen borrows m_recognizer, so it must be released first. Its
    // destructor invalidates queued callbacks and calls stopAndWait() before
    // destroying the QObject receiver; the recognizer remains alive for that
    // boundary. Ordinary cancellation remains nonblocking.
    m_listen.reset();
    m_recognizer.reset();

    // The idle loop is a child of this object and its timer is stopped first:
    // a tick arriving during teardown would call into a half-destroyed
    // controller.
    m_idleTimer.stop();
    // Order matters: cancel so a blocked generate() returns, then stop the event
    // loop, then wait. Only once the thread is idle is it safe to destroy an
    // object whose affinity was that thread.
    if (m_modelLoadThread) {
        m_modelLoadThread->wait();
    }
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
    return QString::fromStdString(m_cachedStatus.backendName);
}

QString AppController::modelName() const {
    return QString::fromStdString(m_cachedStatus.modelName);
}

QString AppController::runtimeDetail() const {
    return QString::fromStdString(m_cachedStatus.detail);
}

bool AppController::runtimeAvailable() const {
    return m_cachedStatus.available && m_cachedStatus.modelLoaded;
}

int AppController::contextUsed() const {
    return static_cast<int>(m_cachedStatus.contextUsed);
}

int AppController::contextLimit() const {
    return static_cast<int>(m_cachedStatus.contextLimit);
}

double AppController::tokensPerSecond() const noexcept {
    return m_tokensPerSecond;
}

int AppController::tokensGenerated() const noexcept {
    return m_tokensGenerated;
}

QString AppController::contextSummary() const {
    const runtime::RuntimeStatus& status = m_cachedStatus;
    if (status.contextLimit == 0) {
        return QStringLiteral("not reported by this backend");
    }
    return QStringLiteral("%1 / %2 tokens")
        .arg(static_cast<qulonglong>(status.contextUsed))
        .arg(static_cast<qulonglong>(status.contextLimit));
}

/// Formats used and total KV-cache bytes, or reports unavailable accounting.
QString AppController::kvCacheSummary() const {
    const runtime::RuntimeStatus& status = m_cachedStatus;
    if (status.kvCacheBytes == 0) {
        // Distinct from zero bytes: this backend cannot account for its cache,
        // which is not the same as the cache being empty.
        return QStringLiteral("not reported by this backend");
    }
    return QStringLiteral("%1 of %2")
        .arg(QString::fromStdString(runtime::formatBytes(status.kvCacheBytesUsed)))
        .arg(QString::fromStdString(runtime::formatBytes(status.kvCacheBytes)));
}

/// Formats the backend-reported prefix token count or the uncached/empty state.
QString AppController::prefixSummary() const {
    const std::size_t resident = m_cachedPrefixTokens;
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
    return !m_generating && response != nullptr
        && response->state() == core::ResponseState::Paused;
}

bool AppController::canBargeIn() const noexcept {
    return canPause();
}

QString AppController::presenceState() const {
    return QString::fromLatin1(core::toString(m_presence.activity()));
}

double AppController::presenceIntensity() const noexcept {
    return m_presence.intensity();
}

bool AppController::presenceSpeaking() const noexcept {
    return m_presence.speaking();
}

QString AppController::personaMood() const {
    return QString::fromLatin1(core::toString(m_presence.snapshot().mood));
}

QString AppController::statusWhisper() const {
    // An anticipatory line outranks the activity whisper while it is fresh, and
    // then lets it back: the status line must return to saying what Kestrel is
    // doing, or a single "Would you like me to continue?" would sit there for
    // the rest of the session.
    if (!m_statusWhisperOverride.isEmpty() && nowMs() < static_cast<std::uint64_t>(m_whisperOverrideUntilMs)) {
        return m_statusWhisperOverride;
    }
    return QString::fromStdString(m_persona.statusWhisper(m_presence.activity()));
}

QString AppController::acknowledgement() const noexcept {
    return m_acknowledgement;
}

QString AppController::ambientThought() const {
    return m_ambientThought;
}

QVariantList AppController::idleTools() const {
    QVariantList result;
    for (const core::IdleToolDeclaration& declaration : m_idleTools.tools()) {
        QStringList required;
        for (const core::ToolPermission permission : declaration.permissions) {
            required << QString::fromLatin1(core::toString(permission));
        }
        QStringList outstanding;
        for (const core::ToolPermission permission : m_idleTools.missing(declaration.name)) {
            outstanding << QString::fromLatin1(core::toString(permission));
        }
        QVariantMap entry;
        entry.insert(QStringLiteral("name"), QString::fromStdString(declaration.name));
        entry.insert(QStringLiteral("summary"), QString::fromStdString(declaration.summary));
        entry.insert(QStringLiteral("enabled"), m_idleTools.enabled(declaration.name));
        entry.insert(QStringLiteral("required"), required);
        entry.insert(QStringLiteral("missing"), outstanding);
        // Reported so the panel can say the difference between a tool that is
        // off and a tool that is on but cannot do anything yet.
        entry.insert(QStringLiteral("permitted"), m_idleTools.permits(declaration.name));
        result.append(entry);
    }
    return result;
}

void AppController::setIdleToolEnabled(const QString& name, bool enabled) {
    m_idleTools.setEnabled(name.toStdString(), enabled);
    emit idleToolsChanged();
}

void AppController::setToolPermission(const QString& permission, bool granted) {
    // Matched by name so QML never has to know the enum, and so a name that
    // does not exist simply grants nothing.
    static constexpr core::ToolPermission kAll[] = {
        core::ToolPermission::ReadConversations,
        core::ToolPermission::RunGeneration,
        core::ToolPermission::WriteFiles,
        core::ToolPermission::Network,
    };
    const std::string wanted = permission.toStdString();
    for (const core::ToolPermission candidate : kAll) {
        if (wanted == core::toString(candidate)) {
            m_idleTools.grant(candidate, granted);
            emit idleToolsChanged();
            return;
        }
    }
}

QStringList AppController::speechVoices() const {
    return m_speech != nullptr ? m_speech->voiceChoices() : QStringList();
}

QString AppController::currentVoice() const {
    return m_speech != nullptr ? m_speech->currentVoice() : QString();
}

QVariantList AppController::speechEngines() const {
    return m_localVoices != nullptr ? m_localVoices->describe() : QVariantList();
}

bool AppController::setSpeechEngine(const QString& engineId) {
    if (m_localVoices == nullptr || engineId == m_speechEngine || m_speaking
        || m_waitingForSpeechBoundary || (m_speech != nullptr && m_speech->audioOwed())) {
        return false;
    }
    std::unique_ptr<SpeechBackend> backend = m_localVoices->create(engineId);
    if (backend == nullptr || (!backend->usable() && !backend->present())) {
        // Refused rather than swapped in and failed later: an engine that cannot
        // start would leave the app with no voice at all, which is worse than
        // staying on the one that works.
        return false;
    }
    m_speechEngine = engineId;
    // The voice list belongs to the engine, so it is asked again rather than
    // remembered; the previous engine's voices are not this engine's voices.
    m_speech->adoptBackend(std::move(backend));
    emit ttsChanged();
    return true;
}

bool AppController::setSpeechVoice(const QString& voice) {
    if (m_speech == nullptr || !m_speech->setVoice(voice)) {
        return false;
    }
    // The description carries the voice name, so the panel updates from the same
    // signal that publishes it rather than needing a notification of its own.
    emit ttsChanged();
    return true;
}

QString AppController::idleTaskLabel() const {
    if (m_idleTaskKind.isEmpty()) {
        return {};
    }
    // A detail is private to the extent the thought beside it is. When thoughts
    // are hidden, the detail goes with them, and that has to be decided here
    // rather than when the task ran or the detail would outlive the setting.
    if (m_idleTaskDetail.isEmpty()) {
        return m_idleTaskKind;
    }
    if (!m_showIdleThoughts && !m_ambientThought.isEmpty()) {
        return m_idleTaskKind;
    }
    // A tool that was refused is reported instead of the task that wanted to
    // run it. The task happened either way, but the part the user is being
    // asked about is why nothing came of it.
    if (!m_idleToolNotice.isEmpty()) {
        return QStringLiteral("%1 \u00b7 %2").arg(m_idleTaskKind, m_idleToolNotice);
    }
    return QStringLiteral("%1 \u00b7 %2").arg(m_idleTaskKind, m_idleTaskDetail);
}

QString AppController::sessionTopic() const {
    return QString::fromStdString(m_persona.sessionTopic());
}

bool AppController::idleLoopEnabled() const noexcept {
    return m_idle.enabled();
}

bool AppController::idlePrewarmEnabled() const noexcept {
    return m_idle.policy().allowModelWarmup;
}

bool AppController::showIdleThoughts() const noexcept {
    return m_showIdleThoughts;
}

bool AppController::inputPending() const noexcept {
    return m_inputPending;
}

bool AppController::ttsAvailable() const noexcept {
    return m_speech != nullptr && m_speech->available();
}

QString AppController::ttsVoice() const {
    if (m_speech == nullptr) {
        return tr("unavailable");
    }
    return m_speech->voiceDescription();
}

bool AppController::speaking() const noexcept {
    return m_speaking;
}

QString AppController::ttsError() const {
    if (!m_speechError.isEmpty()) {
        return m_speechError;
    }
    if (m_speech != nullptr && !m_speech->available()) {
        return m_speech->voiceDescription();
    }
    return {};
}

void AppController::onSpeechSegmentFinished() {
    if (!m_speaking) {
        return;
    }
    if (m_hasPendingSegment) {
        // The clause is out of the speaker. Only now does the spoken cursor move,
        // which is what keeps an interruption resuming from exactly where the
        // user stopped hearing rather than from where decoding happened to be.
        m_hasPendingSegment = false;
        m_voice.advancePlayback(m_activeResponse, m_pendingSegmentEnd);
        noteAssistant(core::AssistantAction::Speaking);
    } else {
        // That was the acknowledgement cue. The pause the persona asked for
        // belongs in front of the answer, not after the cue.
        m_openingPauseMs = m_persona.acknowledgementPauseMs();
        m_acknowledgement.clear();
    }
    pumpNextSegment();
}

void AppController::applyResponseVoice() {
    if (m_speech == nullptr) {
        return;
    }
    // Taken once per response and reused for every clause after it, so the pace
    // belongs to the reply rather than to the moment any one clause happens to
    // be asked for. Re-handed on each request, because the engine holds the
    // pace it was last given: a fresh engine starts at 1.0, and nothing else
    // would put this response's pace back before the next clause was made.
    if (!m_responseVoiceTaken) {
        m_responseVoice = m_voice.voicePersona();
        m_responseVoice.warmth = m_persona.state().warmth;
        m_responseVoiceTaken = true;
    }
    m_speech->applyVoice(m_responseVoice);
}

void AppController::pumpNextSegment() {
    if (!m_speaking) {
        return;
    }
    const auto segment = m_voice.nextSpeechSegment(m_activeResponse, m_openingPauseMs);
    m_openingPauseMs = 0;
    if (!segment.has_value()) {
        // No complete segment is available yet, which is not the same as the
        // response being over. The pump starts as soon as the request is
        // accepted so the cue can be heard while the model is still working,
        // and at that point there is usually no answer text at all. Treating
        // that empty result as completion would deliver a response made of the
        // first few tokens and skip everything after them.
        if (m_generating) {
            return;
        }
        finishPlayback();
        return;
    }
    m_hasPendingSegment = true;
    m_pendingSegmentEnd = segment->endOffset;
    // Committed here rather than only at startPlayback, because this is the
    // point the audio is asked for. The prefetch below asks for more of it in
    // the same breath and must be made at the same pace.
    applyResponseVoice();
    m_speech->speak(QString::fromStdString(segment->text), segment->leadingPauseMs);

    // Look one clause ahead. A synthesising engine needs real time to produce a
    // sentence, and asking it only once the previous one has finished puts that
    // time on the record as silence between sentences. The engine already
    // holding the text makes it sound like a person pausing to think rather than
    // a program waiting on a file.
    if (const auto ahead = m_voice.peekSpeechSegment(m_activeResponse); ahead.has_value()) {
        m_speech->prefetch(QString::fromStdString(ahead->text));
    }
}

void AppController::finishPlayback() {
    m_speaking = false;
    m_hasPendingSegment = false;
    m_openingPauseMs = 0;
    m_waitingForSpeechBoundary = false;
    m_deferredSpeechRestart = false;
    // The response is delivered once it has been spoken, not once it has been
    // generated. That is the whole point of the text-only fallback having been
    // a fallback: with audio, completion follows the audio.
    m_voice.complete(m_activeResponse);
    noteAssistant(core::AssistantAction::Waiting);
    anticipateForDelivery();
    emit voiceChanged();
    emit ttsChanged();
}

void AppController::anticipateForDelivery() {
    // Whether a long answer is offered to continue depends on how long it
    // actually turned out to be, so the judgement is made at delivery rather
    // than guessed at when the request was sent.
    const int lastRow = m_messageModel->rowCount() - 1;
    const QString delivered = lastRow >= 0
        ? m_messageModel->data(m_messageModel->index(lastRow, 0),
                               MessageModel::ContentRole).toString()
        : QString();
    anticipate(delivered.size() > kLongResponseChars ? core::PersonaTrigger::LongResponse
                                                     : core::PersonaTrigger::TurnCompleted);
}

void AppController::onSpeechAvailabilityChanged(bool available) {
    if (!available) {
        // The engine will never answer. The synthesizer has already ended the
        // hold it was holding, and onOwedAudioReleased delivered that response
        // as text; there is nothing left to decide here.
        return;
    }
    m_speechError.clear();
    if (m_speech->audioOwed() && !m_waitingForSpeechBoundary) {
        // The engine turned up, so the response it was holding is spoken
        // now, from the same untouched text: the pump reads that response's
        // own unspoken remainder, so nothing is re-sent and nothing stale is
        // spoken.
        startPlayback();
    }
}

void AppController::setVoiceLoadTimeoutForTesting(int ms) {
    if (m_speech != nullptr) {
        m_speech->setVoiceLoadTimeout(ms);
    }
}

void AppController::onOwedAudioReleased(bool voiceGaveUp) {
    if (voiceGaveUp) {
        m_speechError = tr("The local voice was still loading its model and has given up, "
                           "so replies are being delivered as text.");
    }
    // The response is delivered as text -- the delivery it would have had
    // anyway, at the moment it became true rather than at a moment chosen for
    // convenience. The words are already in the transcript either way; what
    // stops here is the promise of audio for them.
    m_voice.complete(m_activeResponse);
    noteAssistant(core::AssistantAction::Waiting);
    anticipateForDelivery();
    emit voiceChanged();
}

void AppController::onSpeechStopCompleted() {
    if (m_waitingForSpeechBoundary) {
        m_waitingForSpeechBoundary = false;
        m_speaking = false;
        m_hasPendingSegment = false;
        m_openingPauseMs = 0;
        emit ttsChanged();
        emit presenceChanged();
        if (m_deferredSpeechRestart) {
            m_deferredSpeechRestart = false;
            startPlayback();
        }
        return;
    }
    if (!m_speaking) {
        return;
    }
    m_speaking = false;
    m_hasPendingSegment = false;
    m_openingPauseMs = 0;
    emit ttsChanged();
    emit presenceChanged();
}

void AppController::startPlayback() {
    if (m_waitingForSpeechBoundary) {
        // The previous response still owns the speaker until its active clause
        // ends. startGeneration() may already be collecting the replacement's
        // text, so remember to start the new response when stopCompleted arrives.
        m_deferredSpeechRestart = true;
        return;
    }
    // Whether the response is owed audio or handed over to text is decided
    // here, once, and it is the decision that used to be made too early: a
    // local model is installed and running before it can answer, so checking
    // availability at the moment a request is made answered a cold-start reply
    // in text and never revisited it. What the engine is doing is the
    // synthesizer's business; this only asks, once, and moves on either way.
    if (m_speech != nullptr) {
        m_speech->oweAudio();
    }
    if (!ttsAvailable()) {
        return;
    }
    // Every start is the start of a response: a new one, a held one being
    // spoken for the first time, or a resume. The dials are read again here
    // rather than carried over, because carrying them over was a real defect --
    // a message sent while a reply is still speaking is a new response, and it
    // was inheriting the persona the interrupted one had been given, so a
    // back-and-forth at speed spoke every reply at the first reply's pace.
    m_responseVoiceTaken = false;
    // Warmth is the persona's to set, so the voice the synthesizer is handed
    // carries the dial rather than the constant VoicePersona was born with.
    // Filled in per response rather than per clause on purpose: a speed that
    // changed mid-sentence would sound like the voice faltering, and the mood
    // does not move that fast. It is committed again per clause, to the same
    // persona, so the pace a clause is made at is the pace it is played at.
    applyResponseVoice();
    m_speaking = true;
    m_openingPauseMs = 0;
    if (m_acknowledgement.isEmpty()) {
        pumpNextSegment();
        return;
    }
    // The cue goes first and unpaused. It is the sound of the request being
    // accepted, and it must not be delayed by a pause meant for the answer.
    m_hasPendingSegment = false;
    m_speech->speak(m_acknowledgement, 0);
    emit ttsChanged();
}

void AppController::setIdleLoopEnabled(bool enabled) {
    if (m_idle.enabled() == enabled) {
        return;
    }
    m_idle.setEnabled(enabled);
    if (!enabled) {
        m_ambientThought.clear();
        m_idleTaskKind.clear();
        m_idleTaskDetail.clear();
    }
    emit presenceChanged();
}

void AppController::setIdlePrewarmEnabled(bool enabled) {
    core::IdlePolicy policy = m_idle.policy();
    if (policy.allowModelWarmup == enabled) {
        return;
    }
    policy.allowModelWarmup = enabled;
    m_idle.setPolicy(policy);
    emit presenceChanged();
}

void AppController::setShowIdleThoughts(bool show) {
    if (m_showIdleThoughts == show) {
        return;
    }
    m_showIdleThoughts = show;
    emit presenceChanged();
}

void AppController::setInputPending(bool pending) {
    if (m_inputPending == pending) {
        return;
    }
    m_inputPending = pending;
    // Something half-typed is the strongest possible signal that someone is
    // here, so the loop is told before it can decide anything.
    if (pending) {
        noteActivity();
    }
    emit presenceChanged();
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

/// Returns the shared instruction text declared for subsequent turns.
QString AppController::systemPrompt() const {
    return m_systemPrompt;
}

/// Retains ownership while waiting for the in-flight generation to finish.
void AppController::setBackendForTesting(std::unique_ptr<runtime::ModelBackend> backend) {
    if (backend == nullptr) {
        return;
    }
    waitForIdleGeneration(kBackendSwapTimeoutMs);
    if (workerBusy()) {
        return;
    }
    m_backend = std::move(backend);
    m_worker->setBackend(m_backend.get());
    m_backend->setSystemPrompt(m_systemPrompt.toStdString());
    // The snapshot must be refreshed here for the same reason every other
    // backend swap does it: the getters read the cache, so a test that
    // installs a backend and then asks what is loaded would otherwise be told
    // about the backend that was replaced.
    refreshCachedRuntime();
    rebuildDiagnostics();
    emit runtimeChanged();
    emit metricsChanged();
}

/// Trims and stores changed instruction text, updates the backend, and publishes metrics.
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

/// Adopts a speech backend so the playback pump can be driven without a voice.
void AppController::setSpeechBackendForTesting(std::unique_ptr<SpeechBackend> backend) {
    if (backend == nullptr) {
        return;
    }
    m_speech->setBackendForTesting(std::move(backend));
    emit ttsChanged();
}

void AppController::setPersonaStateForTesting(const core::PersonaState& state) {
    m_persona.setState(state);
    m_presence.applyPersona(m_persona.state());
    emit presenceChanged();
}

/// Returns whether this build provides an available llama.cpp backend.
bool AppController::canLoadModel() const {
    // Ask a throwaway instance rather than caching a flag: whether a real model
    // can be loaded is a build-time fact, but a cached copy would go stale the
    // moment the answer is refactored into a runtime check.
    return runtime::LlamaCppBackend{}.status().available;
}

/// Returns the loaded model's local path, or an empty string in preview mode.
QString AppController::modelPath() const {
    return m_modelPath;
}

/// Returns the latest model-switch error, cleared after a successful switch.
QString AppController::modelError() const {
    return m_modelError;
}

/// Loads a candidate off the UI thread, then installs it through a queued signal.
/// Keeps the current backend on validation/load failure or if generation remains active.
void AppController::loadModelFromUrls(const QStringList& urls) {
    if (m_modelLoadThread) {
        return;
    }
    m_modelQueue = urls;
    m_modelAttempts.clear();
    if (m_modelQueue.isEmpty()) {
        return;
    }
    loadModelFromUrl(m_modelQueue.takeFirst());
}

void AppController::loadModelFromUrl(const QString& url) {
    if (m_modelLoadThread) {
        return;
    }
    m_modelAttempts.append(QUrl(url).toLocalFile());
    struct LoadResult {
        QString path;
        QString error;
        std::unique_ptr<runtime::ModelBackend> candidate;
    };
    auto result = std::make_shared<LoadResult>();
    m_discardModelLoad = false;
    m_modelLoadThread.reset(QThread::create([url, result] {
        result->path = QUrl(url).toLocalFile();
        if (result->path.isEmpty()) {
            result->error = tr("That is not a local file. Choose a model from disk.");
            return;
        }
        const QFileInfo info(result->path);
        if (!info.exists()) {
            result->error = tr("No such file: %1").arg(result->path);
            return;
        }

        // The backend is chosen from the shape of what was handed over, not
        // from a preference order. There are two real formats and they are not
        // interchangeable:
        //
        //   * a .gguf file            -> llama.cpp
        //   * a folder with
        //     genai_config.json      -> ONNX Runtime GenAI
        //
        // A GenAI model is a folder -- a config, one or more ONNX graphs and
        // external data -- so a check for a .gguf *file* rejects every one of
        // them. That is not a hypothetical gap: it is how the ONNX Runtime
        // backend would have been built, linked, listed in the diagnostics and
        // still impossible to hand a model, which is the same defect the
        // TensorRT stub was removed for. The backend is picked here, from the
        // thing itself, so a new format is one branch rather than a new
        // preference entry.
        const bool isDirectory = info.isDir();
        const bool isGenAi = isDirectory && QFileInfo::exists(
            result->path + QStringLiteral("/genai_config.json"));
        const bool isGguf = !isDirectory
            && info.suffix().compare(QStringLiteral("gguf"), Qt::CaseInsensitive) == 0;

        if (isGenAi) {
            result->candidate = std::make_unique<runtime::OrtGenAiBackend>();
        } else if (isGguf) {
            result->candidate = std::make_unique<runtime::LlamaCppBackend>();
        } else if (isDirectory) {
            result->error = tr("%1 is a folder but not an ONNX Runtime GenAI model: "
                               "it has no genai_config.json.").arg(info.fileName());
            return;
        } else {
            result->error = tr("%1 is not a model Kestrel can read. Expected a .gguf file "
                               "or a folder containing genai_config.json.")
                               .arg(info.fileName());
            return;
        }

        if (!result->candidate->status().available) {
            result->error = QString::fromStdString(result->candidate->status().detail);
            return;
        }
        std::string error;
        if (!result->candidate->loadModel(result->path.toStdString(), error)) {
            result->error = QString::fromStdString(error);
        }
    }));
    connect(m_modelLoadThread.get(), &QThread::finished, this, [this, result] {
        m_modelLoadThread->wait();
        if (!m_discardModelLoad && result->error.isEmpty()) {
            waitForIdleGeneration(kBackendSwapTimeoutMs);
            if (workerBusy()) {
                result->error = tr("A response is still running. Stop it and try again.");
            }
        }
        if (!m_discardModelLoad) {
            m_modelError = result->error;
            if (m_modelError.isEmpty()) {
                m_modelPath = result->path;
                m_backend = std::move(result->candidate);
                m_worker->setBackend(m_backend.get());
                m_backend->setSystemPrompt(m_systemPrompt.toStdString());
                refreshCachedRuntime();
                rebuildDiagnostics();
                // Loading a model is the one file operation this app performs,
                // so it is also the one place a finished task is worth
                // reporting.
                anticipate(core::PersonaTrigger::TaskCompleted);
                emit runtimeChanged();
                emit metricsChanged();
            }
            emit modelErrorChanged();
        }
        m_modelLoadThread.reset();
        // A failure here is only the end of the sequence when there is nothing
        // left to try. Otherwise the next candidate starts here, before the
        // signal, so that everything listening for modelLoadFinished hears the
        // outcome of the whole attempt rather than of the first one to end.
        //
        // Emitting first and retrying from a connection is the shape this
        // replaced, and it was wrong for a reason worth writing down: queued
        // connections are delivered in the order they were connected, so a
        // --print-runtime or a smoke test already waiting on that signal ran,
        // concluded, and exited before the retry was ever delivered. The
        // fallback was in the code and had never run.
        if (!m_discardModelLoad && !m_modelError.isEmpty() && !m_modelQueue.isEmpty()) {
            m_modelError.clear();
            loadModelFromUrl(m_modelQueue.takeFirst());
            return;
        }
        if (!m_modelError.isEmpty() && m_modelAttempts.size() > 1) {
            m_modelError = tr("No model could be loaded. Tried %n candidate(s), "
                              "and the last one said: %1",
                              nullptr, static_cast<int>(m_modelAttempts.size()))
                               .arg(m_modelError);
        }
        m_modelQueue.clear();
        emit modelErrorChanged();
        emit modelLoadFinished();
    }, Qt::QueuedConnection);
    m_modelLoadThread->start();
}

/// Cancels generation and switches to the mock backend only once idle.
/// Preserves the declared system prompt and reports a timeout through modelError().
void AppController::usePreviewBackend() {
    m_discardModelLoad = true;
    m_modelQueue.clear();
    waitForIdleGeneration(kBackendSwapTimeoutMs);
    if (workerBusy()) {
        m_modelError = tr("A response is still running. Stop it and try again.");
        emit modelErrorChanged();
        return;
    }
    m_modelPath.clear();
    m_modelError.clear();
    m_backend = runtime::selectBackend(runtime::BackendKind::Mock);
    m_worker->setBackend(m_backend.get());
    m_backend->setSystemPrompt(m_systemPrompt.toStdString());
    refreshCachedRuntime();
    rebuildDiagnostics();
    emit modelErrorChanged();
    emit runtimeChanged();
    emit metricsChanged();
}

/// Requests cancellation and pumps a nested event loop for at most timeoutMs.
/// Callers must check workerBusy() afterwards before replacing the backend.
void AppController::drainBackgroundRequests() {
    // Only the requests that are not a conversation. A reply in flight is a
    // different thing entirely and is handled by the barge-in path above.
    if (m_warmupRequestId == 0 && m_toolRequestId == 0) {
        return;
    }
    m_worker->cancel();
    QEventLoop loop;
    const QMetaObject::Connection done =
        connect(this, &AppController::generatingChanged, &loop, [this, &loop] {
            if (m_warmupRequestId == 0 && m_toolRequestId == 0) {
                loop.quit();
            }
        });
    // Bounded, for the same reason waitForIdleGeneration is: a backend that
    // ignores cancellation must not turn a typed message into a frozen window.
    // If it does overrun, the reply is queued anyway and starts behind it,
    // which is the old behaviour rather than a worse one.
    QTimer::singleShot(kBackendSwapTimeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
    disconnect(done);
}

void AppController::waitForIdleGeneration(int timeoutMs, bool stopAudio) {
    if (!workerBusy()) {
        return;
    }
    if (stopAudio) {
        stopGeneration();
    } else if (m_generating) {
        // Replacing a prompt cancels text but lets the speaking clause land.
        m_userStopped = true;
        m_worker->cancel();
    }
    // The prewarm and the tool run are not a conversation, so stopGeneration()
    // does not speak for them and they keep going unless they are cancelled
    // here. Without this the wait below is a wait for a request that will not
    // finish until its own generation does, which may be much longer than the
    // timeout, so the swap proceeds with the worker still inside the old
    // backend.
    if (m_warmupRequestId != 0 || m_toolRequestId != 0) {
        m_worker->cancel();
    }
    QEventLoop loop;
    const QMetaObject::Connection done =
        connect(this, &AppController::generatingChanged, &loop, [this, &loop] {
        if (!workerBusy()) {
            loop.quit();
        }
    });
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
    disconnect(done);
}

bool AppController::workerBusy() const noexcept {
    // All three are inside one generate() call on the worker thread, and all
    // three hold the backend's generation lock for its duration. So "is the
    // worker free" is one question with three answers, and the checks that ask
    // it have to ask all three -- a check that forgets the tool id believes the
    // worker is idle while a summary is running, and then either replaces the
    // backend underneath that generate() or calls a status() accessor that
    // blocks the UI thread until the summary finishes.
    //
    // It is here rather than spelled out at each call site because that is how
    // the tool request came to be missed: the list grew a third member and two
    // of the five places that needed it were left reading the old pair.
    return m_generating || m_warmupRequestId != 0 || m_toolRequestId != 0;
}

std::uint64_t AppController::nowMs() const noexcept {    // m_clock is only invalid between construction and start(), which no caller
    // can observe: the clock is started in the constructor and the tick timer
    // is not connected until after that.
    return static_cast<std::uint64_t>(m_clock.isValid() ? m_clock.elapsed() : 0);
}

void AppController::noteAssistant(core::AssistantAction action) {
    m_presence.noteAssistantAction(action);
    if (const core::VoiceResponse* response = m_voice.find(m_activeResponse)) {
        m_presence.setVoiceState(response->state());
    }
    m_presence.setGenerating(m_generating);
    m_presence.applyPersona(m_persona.state());
    emit presenceChanged();
}

void AppController::setWhisperOverride(const QString& text, int holdMs) {
    if (text.isEmpty()) {
        return;
    }
    m_statusWhisperOverride = text;
    m_whisperOverrideUntilMs = static_cast<qint64>(nowMs())
        + kWhisperHoldMs + static_cast<qint64>(holdMs);
    emit presenceChanged();
}

void AppController::anticipate(core::PersonaTrigger trigger) {
    if (const auto line = m_persona.react(trigger)) {
        setWhisperOverride(QString::fromStdString(line->text), line->microPauseMs);
    }
}

void AppController::noteActivity() {
    const std::uint64_t now = nowMs();
    // A long absence earns one greeting when activity resumes, and only once
    // the loop has had the chance to prepare one.
    if (m_idle.userReturned(now, kReturnGreetingMs)) {
        if (const std::string greeting = m_idle.takeGreeting(); !greeting.empty()) {
            m_presence.noteUserAction(core::UserAction::Returned);
            setWhisperOverride(QString::fromStdString(greeting), 0);
        }
    }
    m_idle.noteActivity(now);
}

void AppController::runIdleToolIfPermitted() {
    if (workerBusy()) {
        m_idleToolNotice = tr("The model worker is busy; the idle tool was skipped.");
        return;
    }
    const ConversationEntry* entry = activeEntry();
    if (entry == nullptr) {
        return;
    }

    // A refusal is reported rather than swallowed. A tool that was skipped
    // silently is indistinguishable from one that ran and found nothing, and
    // the user who granted a capability deserves to know it went unused.

    if (m_idleTools.granted(core::ToolPermission::ReadConversations)) {
        const core::ToolRunResult outcome = core::runIdleTool(
            m_idleTools, core::kIndexThreadsTool, entry->conversation.messages());
        if (outcome.ran) {
            m_idleTaskDetail = QString::fromStdString(outcome.summary);
        } else {
            m_idleToolNotice = QString::fromStdString(outcome.summary);
        }
    } else {
        m_idleToolNotice = tr("No idle tool can run until you grant a capability.");
    }

    // The summariser costs tokens, so it is gated on more than permission: the
    // model has to exist, and the worker has to be free of both a reply and the
    // prewarm. A tool that competed with a real question for the worker would
    // lose, and the user would watch their own answer stall behind a background
    // summary they may not even know is running.
    if (m_idleTools.permits(core::kSummariseSessionTool) && runtimeAvailable()
        && m_warmupRequestId == 0 && m_toolRequestId == 0 && !m_generating && !m_speaking) {
        startSessionSummary();
        return;
    }
    emit presenceChanged();
}

void AppController::startSessionSummary() {
    const ConversationEntry* entry = activeEntry();
    if (entry == nullptr) {
        return;
    }
    // The material goes in the prompt rather than into the system prompt, so a
    // background summary cannot quietly change how every later reply is
    // answered.
    QString material;
    for (const core::Message& message : entry->conversation.messages()) {
        if (message.role == core::MessageRole::Tool) {
            continue;
        }
        const QString prefix = message.role == core::MessageRole::User
            ? QStringLiteral("User: ")
            : QStringLiteral("Kestrel: ");
        material += prefix + QString::fromStdString(message.content) + QLatin1Char('\n');
        if (material.size() > kSummaryMaterialChars) {
            material.truncate(kSummaryMaterialChars);
            break;
        }
    }
    m_toolRequestId = m_nextRequestId++;
    m_toolOutput.clear();
    m_worker->start(m_toolRequestId,
                    QStringLiteral("Assistant: ") + material
                        + QStringLiteral("\nIn two sentences, what is this session about?"),
                    0.3F, kSummaryMaxTokens);
}

void AppController::onIdleTick() {
    const std::uint64_t now = nowMs();

    core::IdleGate gate;
    gate.generating = m_generating;
    // A live response owns the timeline, and an in-flight prewarm owns the
    // worker. Either one means there is no thinking space to be had.
    gate.voiceActive = m_voice.activeResponseId() != core::kInvalidResponseId
                       || m_warmupRequestId != 0;
    gate.userInputPending = m_inputPending;
    m_idle.setGate(gate);
    m_idle.setTopic(m_persona.sessionTopic());

    const core::IdleTick tick = m_idle.tick(now);
    if (tick.produced) {
        m_ambientThought = QString::fromStdString(tick.thought);
        m_idleTaskKind = QString::fromLatin1(core::toString(tick.task.kind));
        m_idleTaskDetail = QString::fromStdString(tick.task.detail);
        m_presence.applyPersona(m_persona.state());

        if (!tick.whisper.empty()) {
            setWhisperOverride(QString::fromStdString(tick.whisper), 0);
        }
        if (tick.task.kind == core::IdleTaskKind::ModelWarmup) {
            noteAssistant(core::AssistantAction::Preparing);
        } else if (tick.task.kind == core::IdleTaskKind::CreativeThought
                   || tick.task.kind == core::IdleTaskKind::SelfReflection) {
            noteAssistant(core::AssistantAction::Reflecting);
        }

        // The one idle task that does real work rather than thinking. It goes
        // through the registry, so it runs only when the user has switched it
        // on and granted what it declared. Its latest outcome is shown in the
        // idle detail without growing the conversation on each run.
        if (tick.task.kind == core::IdleTaskKind::ContextReindex) {
            runIdleToolIfPermitted();
        }
    }

    // The prewarm is the one idle task that leaves pure computation, so it only
    // runs when the policy allows it, a model is actually loaded, and the
    // worker is free. Its tokens are discarded: the point is warm caches, not
    // something said.
    if (tick.produced && tick.task.kind == core::IdleTaskKind::ModelWarmup
        && m_warmupRequestId == 0 && runtimeAvailable() && !m_generating) {
        m_warmupRequestId = m_nextRequestId++;
        m_worker->start(m_warmupRequestId, kWarmupPrompt, 0.1F, kWarmupMaxTokens);
    }

    m_presence.setNow(now);
    m_presence.advance();
    emit presenceChanged();
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
    if (requestId == m_toolRequestId) {
        // A permissioned idle run. Its tokens belong to it, and they used to be
        // dropped on the floor: the guard below keeps only the active request,
        // and a tool's request is never the active one, so a summary was
        // generated in full and then discarded without ever being read. The
        // completion handler made up the text by reading the last row of the
        // transcript, which is the previous assistant reply.
        m_toolOutput += token;
        return;
    }
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
    if (requestId == m_warmupRequestId) {
        // An idle prewarm, not a reply. Its output was never shown; the only
        // consequence is that the backend is warm and its counters moved.
        m_warmupRequestId = 0;
        refreshCachedRuntime();
        emit generatingChanged();
        emit runtimeChanged();
        emit presenceChanged();
        return;
    }
    if (requestId == m_toolRequestId) {
        // A permissioned idle run, not a reply. It is recorded in the
        // transcript as what it is -- a note Kestrel wrote to itself while the
        // user was away -- and then released, so the worker is free for the
        // next real question.
        m_toolRequestId = 0;
        // The text is what the tool actually produced. Reading it back out of
        // the last transcript row was how the summary used to be "recovered":
        // that row is the previous assistant reply or the indexing note, so the
        // notice said "Summarised the session" every single time, including
        // when the summary had been empty or had failed outright.
        const QString said = success ? m_toolOutput.trimmed() : QString();
        m_toolOutput.clear();
        if (!said.isEmpty()) {
            m_idleTaskDetail = said;
            m_idleToolNotice = tr("Summarised the session.");
        } else {
            m_idleToolNotice = error.isEmpty()
                ? tr("The session summary came back empty.")
                : tr("Session summary failed: %1").arg(error);
        }
        refreshCachedRuntime();
        emit generatingChanged();
        emit runtimeChanged();
        emit presenceChanged();
        return;
    }
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

    // Delivery. With a voice installed the response is not finished until the
    // audio is, so completion waits for the playback pump. Without one this is
    // the text-only fallback VoiceSession defines for exactly that situation.
    //
    // An owed response belongs in the first branch, not the second. It means the
    // voice is not able to answer yet but an engine is installed that still
    // might, so completing the response as text here would answer a cold-start
    // reply in writing and strand the audio it is owed. The response stays open
    // instead; onSpeechAvailabilityChanged speaks it when the engine turns up,
    // or onOwedAudioReleased hands it to text when the engine gives up.
    if (ttsAvailable() || (m_speech != nullptr && m_speech->audioOwed())) {
        m_acknowledgement.clear();
        finalizeStream(MessageStatus::Complete, {});
        // The pump can have gone idle earlier, waiting for answer text that had
        // not been generated yet. Now that the response is whole, let it speak
        // the remainder and deliver the turn properly.
        if (m_speaking && !m_waitingForSpeechBoundary) {
            pumpNextSegment();
        }
        return;
    }

    m_voice.complete(m_activeResponse);
    m_acknowledgement.clear();
    finalizeStream(MessageStatus::Complete, {});
    noteAssistant(core::AssistantAction::Waiting);
    anticipateForDelivery();
    emit voiceChanged();
}

/// Creates a streaming reply and queues the assembled conversation on the worker.
/// Uses userText for the voice response timeline and resets per-response metrics.
void AppController::startGeneration(const QString& userText) {
    // A real turn outranks whatever the idle loop had queued for the worker.
    // Both of those requests are inside the backend's generate() right now, and
    // the backend is one object with one generator, so the reply's own start
    // would sit behind the warmup's remaining tokens and the user would watch
    // their question wait on a prewarm they never asked for.
    //
    // Cancelling and then starting in the same breath would not help:
    // GenerationWorker::start() clears the shared cancellation flag as its
    // first act, so the cancel would be forgotten before the warmup noticed it.
    // The warmup has to be given the chance to finish, and only then may the
    // reply be queued.
    drainBackgroundRequests();
    if (workerBusy()) {
        m_modelError = tr("Background work is still stopping. Try again in a moment.");
        emit modelErrorChanged();
        return;
    }

    const ConversationEntry* current = activeEntry();
    if (current == nullptr || current->conversation.messages().empty()
        || current->conversation.messages().back().role != core::MessageRole::User
        || current->conversation.messages().back().content != userText.toStdString()) {
        // The bounded nested wait can process another UI turn or a conversation
        // switch. Do not launch a reply built from a different active transcript.
        return;
    }

    m_messageModel->appendMessage(core::MessageRole::Assistant, {}, MessageStatus::Streaming);
    m_generating = true;
    m_userStopped = false;
    m_userPaused = false;

    resetMetrics();
    m_generationClock.start();

    m_activeResponse = m_voice.queueResponse(userText.toStdString());
    m_activeGeneration = m_voice.beginGeneration(m_activeResponse);
    m_activeRequestId = m_nextRequestId++;

    noteAssistant(core::AssistantAction::Thinking);
    emit generatingChanged();
    emit voiceChanged();
    startPlayback();
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
    m_worker->start(m_activeRequestId, buildMessages(), 0.7F, 512);
}

std::vector<runtime::ChatMessage> AppController::buildMessages() const {
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
                    continue;
            }
            messages.push_back(runtime::ChatMessage{role, message.content});
        }
    }
    // The shared system prompt is not added here. It is the backend's cached
    // prefix, and repeating it in the conversation would both undo the caching
    // and let the renderer place it somewhere the model was not trained to
    // expect it.
    return messages;
}

void AppController::finalizeStream(MessageStatus status, const QString& note) {
    m_messageModel->setLastMessageStatus(status, note);
    m_generating = false;
    m_activeRequestId = 0;
    m_userPaused = false;
    m_userStopped = false;
    m_generationClock.invalidate();
    // The turn is over, so the backend is idle and the counters it moved during
    // generation can now be read without blocking anything.
    refreshCachedRuntime();
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
    if (workerBusy()) {
        waitForIdleGeneration(kBackendSwapTimeoutMs, false);
        if (workerBusy()) {
            return;
        }
    }
    // The nested wait may process conversation changes; borrow only afterwards.
    ConversationEntry* entry = activeEntry();
    if (entry == nullptr) {
        return;
    }
    if (canResume()) {
        m_voice.cancel(m_activeResponse);
        m_messageModel->setLastMessageStatus(MessageStatus::Stopped, {});
    }
    if (!m_speaking && m_speech != nullptr && m_speech->audioOwed()) {
        m_speech->releaseOwedAudio();
    }
    if (m_speaking) {
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
        m_userStopped = m_generating;
        if (m_generating) {
            m_worker->cancel();
        }
        if (m_speaking) {
            // Clause boundary, not mid-word: the clause in flight is allowed to
            // land, and no further segment is pulled. Defer the replacing cue
            // until that boundary arrives; otherwise its completion could be
            // mistaken for a segment of the new response. An explicit stop still
            // cuts immediately, which is what stopGeneration() is for.
            m_waitingForSpeechBoundary = true;
            m_speech->requestStop();
        }
        m_presence.noteUserAction(core::UserAction::Interrupted);
        emit voiceChanged();
    }

    // A request that arrived by ear is still the user being present.
    if (m_listen != nullptr && m_listen->listening()) {
        m_listen->stopListening();
        emit listeningChanged();
    }

    const bool firstMessage = entry->conversation.size() == 0;
    m_messageModel->appendMessage(core::MessageRole::User, trimmed, MessageStatus::Complete);

    // Everything the persona needs to be Kestrel for this turn: the request is
    // accepted out loud, the message joins the session's continuity, and the
    // idle loop is told a person is here.
    m_acknowledgement = QString::fromStdString(m_persona.acknowledgement());
    m_persona.noteUserMessage(trimmed.toStdString());
    m_idle.setTopic(m_persona.sessionTopic());
    noteActivity();
    m_presence.noteUserAction(core::UserAction::Typed);

    if (firstMessage) {
        entry->conversation.setTitle(deriveTitle(trimmed).toStdString());
        emit activeConversationChanged();
    }

    startGeneration(trimmed);
}

void AppController::stopGeneration() {
    m_waitingForSpeechBoundary = false;
    m_deferredSpeechRestart = false;
    if (m_speech != nullptr) {
        // Stop means stop. A reply being held for a voice that has not arrived
        // yet is still audio the user is waiting on, and Escape did nothing at
        // all without this.
        m_speech->releaseOwedAudio();
    }
    if (m_listen != nullptr && m_listen->listening()) {
        m_listen->stopListening();
        emit listeningChanged();
    }
    if (!m_generating && !m_speaking) {
        if (canResume()) {
            m_voice.cancel(m_activeResponse);
            m_messageModel->setLastMessageStatus(MessageStatus::Stopped, {});
            emit voiceChanged();
        }
        return;
    }
    if (m_generating) {
        m_userStopped = true;
        m_worker->cancel();
        m_voice.cancel(m_activeResponse);
    }
    if (m_listen != nullptr && m_listen->listening()) {
        // Escape stops the microphone as well as the reply: both are the user
        // saying stop, and leaving one running would be a surprise.
        m_listen->stopListening();
        emit listeningChanged();
    }
    if (m_speaking) {
        // An explicit cancel cuts immediately. Waiting out the current clause
        // would make the stop button feel ignored.
        m_speech->stopNow();
        m_speaking = false;
        m_hasPendingSegment = false;
        emit ttsChanged();
    }
    m_acknowledgement.clear();
    noteActivity();
    m_presence.noteUserAction(core::UserAction::Interrupted);
    noteAssistant(core::AssistantAction::Waiting);
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
    if (m_speaking) {
        // Pausing is not cancelling: the clause in flight is still allowed to
        // finish, and nothing new is pulled afterwards.
        m_speech->requestStop();
    }
    m_acknowledgement.clear();
    noteActivity();
    m_presence.noteUserAction(core::UserAction::Paused);
    // Pausing says nothing on the status line: the state is already on screen,
    // and a whisper on top of it would be noise.
    noteAssistant(core::AssistantAction::Waiting);
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
    noteActivity();
    m_presence.noteUserAction(core::UserAction::Resumed);
    noteAssistant(core::AssistantAction::Thinking);

    if (*generation != core::kInvalidGenerationId) {
        resetMetrics();
        m_generationClock.start();
        m_generating = true;
        m_activeRequestId = m_nextRequestId++;
        emit generatingChanged();
        emit voiceChanged();

        // Continue from the text the response still owes rather than
        // regenerating from scratch.
        // Keep the full conversation and leave its final assistant turn open.
        // Relabelling the suffix as a user message would answer it instead of
        // continuing the response, and dropping history loses the question.
        m_worker->start(m_activeRequestId, buildMessages(), 0.7F, 512, false);
        return;
    }

    // Everything the response needed was already generated; nothing to restart.
    if (ttsAvailable() && !m_speaking) {
        // The text is all there and the user paused the audio. Resuming means
        // picking the playback back up from the spoken cursor, not completing a
        // response the user has not heard yet.
        startPlayback();
        return;
    }
    m_voice.complete(m_activeResponse);
    finalizeStream(MessageStatus::Complete, {});
}

void AppController::newConversation() {
    if (m_generating) {
        waitForIdleGeneration(kBackendSwapTimeoutMs);
        if (m_generating) {
            return;
        }
    }
    stopGeneration();
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
        waitForIdleGeneration(kBackendSwapTimeoutMs);
        if (m_generating) {
            return;
        }
    }
    stopGeneration();
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
    if (findEntry(id) == nullptr) {
        return;
    }

    const bool wasActive = (id == m_activeId);
    if (wasActive && m_generating) {
        waitForIdleGeneration(kBackendSwapTimeoutMs);
        if (m_generating) {
            return;
        }
    }
    const auto it = std::find_if(m_entries.begin(), m_entries.end(),
                                [id](const auto& entry) { return entry->id == id; });
    if (it == m_entries.end() || wasActive != (id == m_activeId)) {
        return; // A nested UI event already changed the selection or removed it.
    }
    if (wasActive) {
        stopGeneration();
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

    stopGeneration();
    m_voice.cancel(m_activeResponse);
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
    refreshCachedRuntime();
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
    if (id != m_activeId) {
        m_waitingForSpeechBoundary = false;
        m_deferredSpeechRestart = false;
        // Leaving the conversation a held reply belongs to abandons its audio,
        // the same way leaving abandons its generation. Without this the reply
        // went on being promised, and the engine arriving a moment later spoke
        // it into whatever conversation the user had moved to.
        if (m_speech != nullptr) {
            m_speech->releaseOwedAudio();
        }
        // A reply that has finished generating but is still being spoken is a
        // different case, and it is the one that was missed. releaseOwedAudio
        // only drops a reply that has not started playing; once the pump is
        // running, leaving the conversation has to stop the pump too. Otherwise
        // the old conversation's reply keeps being spoken over the new one, and
        // finishPlayback then judges how long the reply was by looking at the
        // last row of a transcript it does not belong to.
        if (m_speaking) {
            m_speech->stopNow();
            m_speaking = false;
            m_hasPendingSegment = false;
            m_acknowledgement.clear();
            emit ttsChanged();
        }
    }
    if (id != m_activeId) {
        m_voice.cancel(m_activeResponse);
        m_activeResponse = core::kInvalidResponseId;
        m_activeGeneration = core::kInvalidGenerationId;
    }
    m_activeId = id;
    m_messageModel->setEntry(findEntry(id));
    emit activeConversationChanged();
    refreshCanRegenerate();
}

// Takes the snapshot the getters read.
//
// ONLY call this when the worker is known to be idle. Calling it mid-turn
// reintroduces exactly the stall this exists to remove, since the backend's
// accessors take the lock generate() is holding.
void AppController::refreshCachedRuntime() {
    if (workerBusy()) {
        return;
    }
    m_cachedStatus = m_backend->status();
    m_cachedPrefixTokens = m_backend->cachedPrefixTokens();
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
