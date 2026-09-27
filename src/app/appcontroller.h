#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QVariantList>

#include <cstdint>
#include <memory>
#include <vector>

#include "app/conversationentry.h"
#include "app/localvoiceengines.h"
#include "app/conversationmodel.h"
#include "app/messagemodel.h"
#include "core/idlepersona.h"
#include "core/idletool.h"
#include "core/persona.h"
#include "core/presence.h"
#include "core/voicesession.h"
#include "runtime/cudadevice.h"
#include "runtime/modelbackend.h"
#include "runtime/speechrecognizer.h"

namespace kestrel::app {

class GenerationWorker;
class SpeechSynthesizer;
class ListenSession;
// Forward declared as a class, not a struct, to match its definition in
// speechsynthesizer.h. MSVC encodes that difference in the mangled name, so a
// mismatched tag here compiles and then fails to link.
class SpeechBackend;

// QML-facing application state: owns the conversations, the model backend,
// and the streaming pipeline between them.
//
// The controller is the only place that translates portable runtime concepts
// into Qt properties. It never includes a CUDA or TensorRT header: it reports
// device facts through the portable structs in runtime/cudadevice.h, so the UI
// behaves identically whether or not those SDKs were compiled in.
//
// Everything here runs on the UI thread. Generation does not: it is handed to
// GenerationWorker, and results arrive back as queued signals.
class AppController final : public QObject {
    Q_OBJECT

    // Conversation state, published as real list models so a streamed chunk is
    // one dataChanged on one row rather than a full model rebuild.
    Q_PROPERTY(MessageModel* messages READ messages CONSTANT)
    Q_PROPERTY(ConversationModel* conversations READ conversations CONSTANT)
    Q_PROPERTY(int activeConversationId READ activeConversationId NOTIFY activeConversationChanged)
    Q_PROPERTY(QString conversationTitle READ conversationTitle NOTIFY activeConversationChanged)
    Q_PROPERTY(bool generating READ generating NOTIFY generatingChanged)
    Q_PROPERTY(bool canRegenerate READ canRegenerate NOTIFY canRegenerateChanged)
    Q_PROPERTY(bool sidebarOpen READ sidebarOpen WRITE setSidebarOpen NOTIFY sidebarOpenChanged)
    Q_PROPERTY(QString searchQuery READ searchQuery WRITE setSearchQuery NOTIFY searchQueryChanged)

    // Runtime identity and live metrics.
    Q_PROPERTY(QString backendName READ backendName NOTIFY runtimeChanged)
    Q_PROPERTY(QString modelName READ modelName NOTIFY runtimeChanged)
    Q_PROPERTY(QString runtimeDetail READ runtimeDetail NOTIFY runtimeChanged)
    Q_PROPERTY(bool runtimeAvailable READ runtimeAvailable NOTIFY runtimeChanged)
    Q_PROPERTY(int contextUsed READ contextUsed NOTIFY metricsChanged)
    Q_PROPERTY(int contextLimit READ contextLimit NOTIFY metricsChanged)
    // Measured here from tokens actually delivered, not read from the backend,
    // so the figure reflects what the user experienced.
    Q_PROPERTY(double tokensPerSecond READ tokensPerSecond NOTIFY metricsChanged)
    Q_PROPERTY(int tokensGenerated READ tokensGenerated NOTIFY metricsChanged)
    Q_PROPERTY(QString contextSummary READ contextSummary NOTIFY metricsChanged)
    // KV cache occupancy in bytes, alongside the token count above. Token
    // counts stay flat while the bytes behind them grow linearly, so this is
    // the figure that says whether a bigger context is affordable.
    Q_PROPERTY(QString kvCacheSummary READ kvCacheSummary NOTIFY metricsChanged)
    // How much of the shared system prompt is resident and being reused.
    Q_PROPERTY(QString prefixSummary READ prefixSummary NOTIFY metricsChanged)

    // Voice conversation state, projected from the core VoiceSession machine.
    Q_PROPERTY(QString voiceState READ voiceState NOTIFY voiceChanged)
    Q_PROPERTY(bool canPause READ canPause NOTIFY voiceChanged)
    Q_PROPERTY(bool canResume READ canResume NOTIFY voiceChanged)
    Q_PROPERTY(bool canBargeIn READ canBargeIn NOTIFY voiceChanged)

    // Assistant presence, projected from the core presence engine. The UI
    // animates on these instead of on raw events, so a pulse means the same
    // thing whether it was caused by a keystroke, a barge-in, or the idle loop.
    Q_PROPERTY(QString presenceState READ presenceState NOTIFY presenceChanged)
    Q_PROPERTY(double presenceIntensity READ presenceIntensity NOTIFY presenceChanged)
    Q_PROPERTY(bool presenceSpeaking READ presenceSpeaking NOTIFY presenceChanged)
    Q_PROPERTY(QString personaMood READ personaMood NOTIFY presenceChanged)
    // The single status line shown under the composer: an activity whisper, or
    // an anticipatory line while one is still fresh.
    Q_PROPERTY(QString statusWhisper READ statusWhisper NOTIFY presenceChanged)
    // The cue said the moment a request is accepted. Empty once the answer is
    // already underway, so a consumer can speak it once and move on.
    Q_PROPERTY(QString acknowledgement READ acknowledgement NOTIFY presenceChanged)
    // Kestrel's own thought between turns. Always populated, displayed only when
    // the user asks to see it: an internal note that leaks by default is not an
    // internal note.
    Q_PROPERTY(QString ambientThought READ ambientThought NOTIFY presenceChanged)
    Q_PROPERTY(QString idleTaskLabel READ idleTaskLabel NOTIFY presenceChanged)
    // The idle tools the loop may ask for, and what each one still needs before
    // it is allowed to run. A list rather than a single flag because the tools
    // are not interchangeable: each declares its own capabilities, and the point
    // of the panel is that the user can see which is which.
    Q_PROPERTY(QVariantList idleTools READ idleTools NOTIFY idleToolsChanged)
    // The voices the current backend can speak with, and the one it is using.
    // Empty when the platform voice is in charge: that choice belongs to the
    // operating system, and offering a list the user cannot change would be a
    // control that does nothing.
    Q_PROPERTY(QStringList speechVoices READ speechVoices NOTIFY ttsChanged)
    Q_PROPERTY(QString currentVoice READ currentVoice NOTIFY ttsChanged)
    // The local engines found on this machine, for switching between them.
    Q_PROPERTY(QVariantList speechEngines READ speechEngines NOTIFY ttsChanged)
    Q_PROPERTY(QString sessionTopic READ sessionTopic NOTIFY presenceChanged)

    // The autonomous loop between turns, plus the two switches that keep it
    // inside its box: the loop itself, the GPU prewarm that is off until asked
    // for, and the reveal of internal thoughts.
    Q_PROPERTY(bool idleLoopEnabled READ idleLoopEnabled WRITE setIdleLoopEnabled NOTIFY presenceChanged)
    Q_PROPERTY(bool idlePrewarmEnabled READ idlePrewarmEnabled WRITE setIdlePrewarmEnabled NOTIFY presenceChanged)
    Q_PROPERTY(bool showIdleThoughts READ showIdleThoughts WRITE setShowIdleThoughts NOTIFY presenceChanged)
    // True while the composer holds unsent text. The idle loop goes quiet
    // whenever this is set, so a half-typed question is never interrupted.
    Q_PROPERTY(bool inputPending READ inputPending WRITE setInputPending NOTIFY presenceChanged)

    // Audio. Available only when this build has a speech engine and the machine
    // has an installed voice; false means the response is delivered as text,
    // which is a supported outcome rather than an error.
    Q_PROPERTY(bool ttsAvailable READ ttsAvailable NOTIFY ttsChanged)
    Q_PROPERTY(QString ttsVoice READ ttsVoice NOTIFY ttsChanged)
    Q_PROPERTY(bool speaking READ speaking NOTIFY ttsChanged)
    // Why audio is unavailable, or what went wrong with it. Empty when it is
    // simply working, so the interface can say which of the two it is.
    Q_PROPERTY(QString ttsError READ ttsError NOTIFY ttsChanged)

    // Speech input. A recognized phrase is submitted through the same path a
    // typed one takes, so a spoken request interrupts a reply exactly as
    // typing one would.
    Q_PROPERTY(bool listening READ listening NOTIFY listeningChanged)
    Q_PROPERTY(bool sttAvailable READ sttAvailable NOTIFY listeningChanged)
    Q_PROPERTY(QString sttDetail READ sttDetail NOTIFY listeningChanged)
    // Words heard so far, so the user can see and correct them before the
    // phrase is committed to a turn.
    Q_PROPERTY(QString partialTranscript READ partialTranscript NOTIFY listeningChanged)
    // Why listening stopped without a phrase, empty otherwise.
    Q_PROPERTY(QString listenError READ listenError NOTIFY listeningChanged)

    Q_PROPERTY(bool diagnosticsOpen READ diagnosticsOpen WRITE setDiagnosticsOpen NOTIFY diagnosticsOpenChanged)

    // The shared instruction prefix. It is identical on every turn, so the
    // backend decodes it once and keeps it resident rather than resending it.
    Q_PROPERTY(QString systemPrompt READ systemPrompt WRITE setSystemPrompt NOTIFY systemPromptChanged)

    // Loading a model from disk, so the app is not stuck on the mock preview.
    Q_PROPERTY(bool canLoadModel READ canLoadModel NOTIFY runtimeChanged)
    Q_PROPERTY(QString modelPath READ modelPath NOTIFY runtimeChanged)
    // Why the last load attempt failed, empty when it succeeded. Shown next to
    // the picker so a failure is visible rather than silently ignored.
    Q_PROPERTY(QString modelError READ modelError NOTIFY modelErrorChanged)

    // GPU facts, sourced from a real CUDA probe rather than assumed.
    Q_PROPERTY(bool gpuAvailable READ gpuAvailable NOTIFY runtimeChanged)
    Q_PROPERTY(QString gpuName READ gpuName NOTIFY runtimeChanged)
    Q_PROPERTY(QString gpuSummary READ gpuSummary NOTIFY runtimeChanged)
    Q_PROPERTY(QString gpuDetail READ gpuDetail NOTIFY runtimeChanged)
    Q_PROPERTY(QString computeCapability READ computeCapability NOTIFY runtimeChanged)
    Q_PROPERTY(int gpuDeviceCount READ gpuDeviceCount NOTIFY runtimeChanged)
    Q_PROPERTY(QVariantList runtimeDiagnostics READ runtimeDiagnostics NOTIFY runtimeChanged)

public:
    explicit AppController(QObject* parent = nullptr);
    ~AppController() override;

    // Test seam: adopt a backend supplied by the caller instead of selecting
    // one. Ownership remains with the parameter until the worker is idle.
    //
    // The constructor cannot take a backend because QML creates this object,
    // but the send path (controller -> worker -> backend -> message model) is
    // exactly the part with no coverage, and it cannot be covered without
    // choosing which backend drives it.
    void setBackendForTesting(std::unique_ptr<runtime::ModelBackend> backend);

    // Test seam for audio: adopt a speech backend supplied by the caller, so the
    // clause pump can be driven on a machine with no voice installed. Mirrors
    // setBackendForTesting, for the same reason.
    void setSpeechBackendForTesting(std::unique_ptr<SpeechBackend> backend);

    [[nodiscard]] MessageModel* messages() const noexcept;
    [[nodiscard]] ConversationModel* conversations() const noexcept;
    [[nodiscard]] int activeConversationId() const noexcept;
    [[nodiscard]] QString conversationTitle() const;
    /// Returns the shared instruction text declared for subsequent turns.
    [[nodiscard]] QString systemPrompt() const;
    /// Returns whether this build provides an available llama.cpp backend.
    [[nodiscard]] bool canLoadModel() const;
    /// Returns the loaded model's local path, or an empty string in preview mode.
    [[nodiscard]] QString modelPath() const;
    /// Returns the latest model-switch error, cleared after a successful switch.
    [[nodiscard]] QString modelError() const;
    [[nodiscard]] bool generating() const noexcept;
    [[nodiscard]] bool canRegenerate() const noexcept;
    [[nodiscard]] bool sidebarOpen() const noexcept;
    [[nodiscard]] QString searchQuery() const;
    [[nodiscard]] bool diagnosticsOpen() const noexcept;

    [[nodiscard]] QString backendName() const;
    [[nodiscard]] QString modelName() const;
    [[nodiscard]] QString runtimeDetail() const;
    [[nodiscard]] bool runtimeAvailable() const;
    [[nodiscard]] int contextUsed() const;
    [[nodiscard]] int contextLimit() const;
    [[nodiscard]] double tokensPerSecond() const noexcept;
    [[nodiscard]] int tokensGenerated() const noexcept;
    [[nodiscard]] QString contextSummary() const;
    /// Formats used and total KV-cache bytes, or reports unavailable accounting.
    [[nodiscard]] QString kvCacheSummary() const;
    /// Formats the backend-reported prefix token count or the uncached/empty state.
    [[nodiscard]] QString prefixSummary() const;

    [[nodiscard]] QString voiceState() const;
    [[nodiscard]] bool canPause() const noexcept;
    [[nodiscard]] bool canResume() const noexcept;
    [[nodiscard]] bool canBargeIn() const noexcept;

    [[nodiscard]] QString presenceState() const;
    [[nodiscard]] double presenceIntensity() const noexcept;
    [[nodiscard]] bool presenceSpeaking() const noexcept;
    [[nodiscard]] QString personaMood() const;
    [[nodiscard]] QString statusWhisper() const;
    [[nodiscard]] QString acknowledgement() const noexcept;
    [[nodiscard]] QString ambientThought() const;
    [[nodiscard]] QString idleTaskLabel() const;
    [[nodiscard]] QStringList speechVoices() const;
    [[nodiscard]] QString currentVoice() const;
    [[nodiscard]] QVariantList speechEngines() const;
    // One map per declared tool: name, summary, whether it is switched on, the
    // capabilities it declared, and which of those are still outstanding.
    [[nodiscard]] QVariantList idleTools() const;
    [[nodiscard]] QString sessionTopic() const;
    [[nodiscard]] bool idleLoopEnabled() const noexcept;
    [[nodiscard]] bool idlePrewarmEnabled() const noexcept;
    [[nodiscard]] bool showIdleThoughts() const noexcept;
    [[nodiscard]] bool inputPending() const noexcept;
    [[nodiscard]] bool ttsAvailable() const noexcept;
    [[nodiscard]] QString ttsVoice() const;
    /// True while a clause is being spoken. False for the whole lifetime of a
    /// build with no voice, and false between clauses of one that has it.
    [[nodiscard]] bool speaking() const noexcept;
    [[nodiscard]] QString ttsError() const;

    [[nodiscard]] bool listening() const noexcept;
    [[nodiscard]] bool sttAvailable() const noexcept;
    [[nodiscard]] QString sttDetail() const;
    [[nodiscard]] QString partialTranscript() const;
    [[nodiscard]] QString listenError() const;

    void setIdleLoopEnabled(bool enabled);
    void setIdlePrewarmEnabled(bool enabled);
    void setShowIdleThoughts(bool show);
    void setInputPending(bool pending);

    [[nodiscard]] bool gpuAvailable() const;
    [[nodiscard]] QString gpuName() const;
    [[nodiscard]] QString gpuSummary() const;
    [[nodiscard]] QString gpuDetail() const;
    [[nodiscard]] QString computeCapability() const;
    [[nodiscard]] int gpuDeviceCount() const;
    [[nodiscard]] QVariantList runtimeDiagnostics() const;

    void setSidebarOpen(bool open);
    void setSearchQuery(const QString& query);
    void setDiagnosticsOpen(bool open);
    /// Trims and stores changed instruction text, updates the backend, and publishes metrics.
    void setSystemPrompt(const QString& text);

    Q_INVOKABLE void sendMessage(const QString& text);
    // Permissioned idle work. Both of these are deliberately string-keyed from
    // QML: the panel lists what the registry actually holds, and a name that is
    // not there is ignored rather than inventing a tool or a capability.
    Q_INVOKABLE void setIdleToolEnabled(const QString& name, bool enabled);
    Q_INVOKABLE void setToolPermission(const QString& permission, bool granted);
    // Changes the speaking voice. Refuses a name the backend does not have, so a
    // stale setting cannot leave the app quietly speaking with something else.
    Q_INVOKABLE bool setSpeechVoice(const QString& voice);
    // Switches between the local engines. Rebuilds the backend, which is the
    // only way to change which model is in charge of the voice.
    Q_INVOKABLE bool setSpeechEngine(const QString& engineId);
    [[nodiscard]] QString speechEngine() const { return m_speechEngine; }
    // Speech input. A completed phrase is submitted exactly as if it had been
    // typed, which is what makes a spoken request interrupt a reply the same way
    // a typed one does. Returns false and says why when the recognizer refuses,
    // rather than failing silently at a button the user can see.
    Q_INVOKABLE bool startListening();
    Q_INVOKABLE void stopListening();
    // Starts a phrase over: the partial text is discarded, not submitted.
    Q_INVOKABLE void abandonListening();
    Q_INVOKABLE void stopGeneration();
    Q_INVOKABLE void pauseConversation();
    Q_INVOKABLE void resumeConversation();
    Q_INVOKABLE void newConversation();
    Q_INVOKABLE void selectConversation(int id);
    Q_INVOKABLE void renameConversation(int id, const QString& title);
    Q_INVOKABLE void deleteConversation(int id);
    Q_INVOKABLE void regenerateLastResponse();
    Q_INVOKABLE void copyToClipboard(const QString& text) const;
    /// Loads a GGUF on a worker thread and switches the app onto it when ready. Takes the URL a
    /// FileDialog hands back rather than a raw path, because QML file dialogs
    /// speak in URLs and converting here is far more reliable than string
    /// surgery on the percent-encoded form.
    Q_INVOKABLE void loadModelFromUrl(const QString& url);
    /// Goes back to the built-in preview backend, so a user who loaded the
    /// wrong file is not stuck with it.
    Q_INVOKABLE void usePreviewBackend();
    // Re-runs device discovery. Probing is cheap, but it is a driver call, so
    // it is explicit rather than happening on every property read.
    Q_INVOKABLE void refreshRuntime();

    // Development helper behind the KESTREL_DEMO environment variable: seeds
    // reviewable conversations and optionally starts a live streaming
    // response. Never called in normal operation.
    void seedDemoContent(bool startLiveStream);

signals:
    void activeConversationChanged();
    void generatingChanged();
    void canRegenerateChanged();
    void sidebarOpenChanged();
    void searchQueryChanged();
    void runtimeChanged();
    void diagnosticsOpenChanged();
    /// Notifies observers that the shared instruction text changed.
    void systemPromptChanged();
    /// Notifies observers that a model-switch attempt updated or cleared the error.
    void modelErrorChanged();
    void modelLoadFinished();
    void metricsChanged();
    void voiceChanged();
    /// Notifies observers that presence, mood, or the status line changed.
    void presenceChanged();
    void idleToolsChanged();
    /// Notifies observers that audio availability or playback state changed.
    void ttsChanged();
    /// Notifies observers that listening state or the partial transcript changed.
    void listeningChanged();

private:
    // Worker callbacks, delivered on the UI thread by queued connections.
    void onGenerationToken(quint64 requestId, const QString& token);
    void onGenerationFinished(quint64 requestId, bool success, const QString& error);

    // The idle thought cycle, driven by m_idleTimer. Silent whenever the user
    // is present: a turn is running, the voice is live, or something is typed
    // and unsent.
    void onIdleTick();

    // Listen session callbacks. The final phrase is handed to sendMessage, so
    // there is exactly one way a turn starts no matter how the user said it.
    void onUtteranceFinal(const QString& text, double confidence);
    void onListeningEnded(const QString& reason);

    // Playback pump. The synthesizer reports a clause finished; the controller
    // asks VoiceSession for the next one, or lets the response complete when
    // there is nothing left to say.
    void onSpeechSegmentFinished();
    void onSpeechStopCompleted();
    void onSpeechFailed(const QString& reason);
    void startPlayback();
    void pumpNextSegment();
    void finishPlayback();
    /// Offers a closing line if the delivered answer was long enough to warrant
    /// one. Called wherever delivery actually completes, text-only or spoken.
    void anticipateForDelivery();

    [[nodiscard]] ConversationEntry* findEntry(int id) noexcept;
    [[nodiscard]] ConversationEntry* activeEntry() noexcept;
    [[nodiscard]] const ConversationEntry* activeEntry() const noexcept;

    ConversationEntry* createConversation();
    void setActiveConversation(int id);
    /// Creates a streaming reply and queues the assembled conversation on the worker.
    /// Uses userText for the voice response timeline and resets per-response metrics.
    void startGeneration(const QString& userText);

    /// Assembles the text actually sent to the model: the recent conversation
    /// followed by an assistant cue. The shared system prompt is excluded on
    /// purpose, because the backend keeps it as a cached prefix.
    [[nodiscard]] QString buildPrompt() const;

    /// Spins the UI event loop until the in-flight generation reports back, or
    /// the timeout expires. Needed before swapping or destroying a backend,
    /// because the worker is inside the old backend's generate() right now and
    /// that backend is about to go away. Bounded, so a wedged backend cannot
    /// freeze the window.
    void waitForIdleGeneration(int timeoutMs);

    void finalizeStream(MessageStatus status, const QString& note);
    void touchActiveConversation();
    void refreshCanRegenerate();

    // Re-reads the backend's status into the snapshot the UI getters use.
    // Must only be called while the worker is idle; see the definition.
    void refreshCachedRuntime();

    void resetMetrics();
    void publishMetrics();
    void rebuildDiagnostics();

    /// Monotonic milliseconds since the controller was created. Passed to the
    /// presence engine and the idle loop rather than letting either read a
    /// clock, so their behaviour is reproducible in tests.
    [[nodiscard]] std::uint64_t nowMs() const noexcept;

    /// Records what the assistant is doing and republishes presence.
    void noteAssistant(core::AssistantAction action);
    // Runs the declared idle tool if the registry permits it, and records what
    // it did in the transcript. Private because the permission check lives
    // inside; a caller cannot ask for the run to be forced.
    void runIdleToolIfPermitted();
    // Submits a background summary of the session. Only reached when the tool is
    // permitted, a model is loaded, and the worker is free.
    void startSessionSummary();

    /// Offers an anticipatory line for this long, then lets the activity
    /// whisper take the status line back.
    void setWhisperOverride(const QString& text, int holdMs);

    /// Asks the persona whether this moment deserves a line, and shows it if so.
    void anticipate(core::PersonaTrigger trigger);

    /// Any user or assistant action. Keeps the idle loop out of the way.
    void noteActivity();

    std::unique_ptr<runtime::ModelBackend> m_backend;

    // The controller owns the thread so shutdown order is explicit: cancel the
    // work, stop the loop, wait for it, and only then destroy the worker.
    QThread m_generationThread;
    std::unique_ptr<QThread> m_modelLoadThread;
    bool m_discardModelLoad = false;
    GenerationWorker* m_worker = nullptr;

    std::vector<std::unique_ptr<ConversationEntry>> m_entries;
    MessageModel* m_messageModel = nullptr;
    ConversationModel* m_conversationModel = nullptr;

    int m_activeId = 0;
    int m_nextId = 1;
    bool m_generating = false;
    bool m_canRegenerate = false;
    bool m_sidebarOpen = true;
    bool m_diagnosticsOpen = false;
    QString m_searchQuery;
    QString m_systemPrompt;
    QString m_modelPath;
    QString m_modelError;

    // Live metrics. Token count and the clock are the basis for throughput;
    // the clock only runs while a response is generating.
    QElapsedTimer m_generationClock;
    int m_tokensGenerated = 0;
    double m_tokensPerSecond = 0.0;
    qint64 m_lastMetricsPublish = 0;

    // Why generation was interrupted, so the right terminal message state is
    // chosen when the worker's completion signal arrives.
    bool m_userStopped = false;
    bool m_userPaused = false;

    // Voice state machine. Owns the response timeline and is the authority on
    // what the UI is allowed to offer next.
    core::VoiceSession m_voice;

    // The personality, the presence projection of it, and the loop that keeps
    // the assistant company between turns. All three are portable core; this
    // layer only polls them and translates the results into Qt properties.
    core::Persona m_persona;
    core::IdlePersona m_idle;
    // The permission boundary for anything the loop does on its own. Held by
    // value: it is small, and sharing one would let a tool grant itself a
    // capability.
    core::IdleToolRegistry m_idleTools;
    // The local engines discovered at start-up, and which one is speaking.
    std::unique_ptr<LocalVoiceEngines> m_localVoices;
    QString m_speechEngine;
    // Non-zero while a permissioned tool is using the worker. Tracked apart from
    // the prewarm id so a backend swap can tell "busy answering someone" from
    // "busy thinking on its own".
    quint64 m_toolRequestId = 0;
    // Why the last idle tool did not run, shown in place of the task detail. A
    // tool that is refused without saying so is a tool the user cannot debug.
    QString m_idleToolNotice;
    core::Presence m_presence;
    // Optional. Null only if the allocation fails; a build with no speech engine
    // still constructs one, and it simply reports itself unavailable.
    std::unique_ptr<SpeechSynthesizer> m_speech;
    // The recognizer and the session that drives it. The recognizer stays a
    // plain runtime type -- no QObject, no Qt types in the portable layer -- so
    // the app owns it and the session borrows it.
    std::unique_ptr<runtime::MockSpeechRecognizer> m_recognizer;
    std::unique_ptr<ListenSession> m_listen;
    // True from the first clause of a response until the last one is spoken.
    // Distinct from m_generating, because generation finishes long before the
    // audio does, and the two states gate different things: the worker, and the
    // spoken cursor.
    bool m_speaking = false;
    QString m_speechError;
    QString m_listenError;
    bool m_hasPendingSegment = false;
    std::size_t m_pendingSegmentEnd = 0;
    int m_openingPauseMs = 0;
    QElapsedTimer m_clock;
    QTimer m_idleTimer;
    // In-flight GPU prewarm from the idle loop, or 0 when none is running. The
    // loop is silent while this is set, so a second warmup cannot stack behind
    // the first.
    quint64 m_warmupRequestId = 0;

    QString m_statusWhisperOverride;
    qint64 m_whisperOverrideUntilMs = 0;
    QString m_acknowledgement;
    QString m_ambientThought;
    // Kept apart rather than pre-joined into one label, because whether the
    // detail may be shown is a property of the current settings, not of the
    // task. Projecting it when the label is read means a change to those
    // settings takes effect at once instead of leaving the previous
    // projection on screen until the next task happens to run.
    QString m_idleTaskKind;
    QString m_idleTaskDetail;
    bool m_inputPending = false;
    bool m_showIdleThoughts = false;
    core::ResponseId m_activeResponse = core::kInvalidResponseId;
    core::GenerationId m_activeGeneration = core::kInvalidGenerationId;
    quint64 m_nextRequestId = 1;
    quint64 m_activeRequestId = 0;

    // A snapshot of the backend's status, taken only when the worker is idle.
    // Reading the live backend from a property getter would block the UI
    // thread on the generation mutex for the whole reply.
    runtime::RuntimeStatus m_cachedStatus;
    std::size_t m_cachedPrefixTokens = 0;

    runtime::CudaProbe m_probe;
    QVariantList m_runtimeDiagnostics;
};

} // namespace kestrel::app
