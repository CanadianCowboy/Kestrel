#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QThread>
#include <QVariantList>

#include <memory>
#include <vector>

#include "app/conversationentry.h"
#include "app/conversationmodel.h"
#include "app/messagemodel.h"
#include "core/voicesession.h"
#include "runtime/cudadevice.h"
#include "runtime/modelbackend.h"

namespace kestrel::app {

class GenerationWorker;

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

    // Voice conversation state, projected from the core VoiceSession machine.
    Q_PROPERTY(QString voiceState READ voiceState NOTIFY voiceChanged)
    Q_PROPERTY(bool canPause READ canPause NOTIFY voiceChanged)
    Q_PROPERTY(bool canResume READ canResume NOTIFY voiceChanged)
    Q_PROPERTY(bool canBargeIn READ canBargeIn NOTIFY voiceChanged)

    Q_PROPERTY(bool diagnosticsOpen READ diagnosticsOpen WRITE setDiagnosticsOpen NOTIFY diagnosticsOpenChanged)

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

    [[nodiscard]] MessageModel* messages() const noexcept;
    [[nodiscard]] ConversationModel* conversations() const noexcept;
    [[nodiscard]] int activeConversationId() const noexcept;
    [[nodiscard]] QString conversationTitle() const;
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

    [[nodiscard]] QString voiceState() const;
    [[nodiscard]] bool canPause() const noexcept;
    [[nodiscard]] bool canResume() const noexcept;
    [[nodiscard]] bool canBargeIn() const noexcept;

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

    Q_INVOKABLE void sendMessage(const QString& text);
    Q_INVOKABLE void stopGeneration();
    Q_INVOKABLE void pauseConversation();
    Q_INVOKABLE void resumeConversation();
    Q_INVOKABLE void newConversation();
    Q_INVOKABLE void selectConversation(int id);
    Q_INVOKABLE void renameConversation(int id, const QString& title);
    Q_INVOKABLE void deleteConversation(int id);
    Q_INVOKABLE void regenerateLastResponse();
    Q_INVOKABLE void copyToClipboard(const QString& text) const;
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
    void metricsChanged();
    void voiceChanged();

private:
    // Worker callbacks, delivered on the UI thread by queued connections.
    void onGenerationToken(quint64 requestId, const QString& token);
    void onGenerationFinished(quint64 requestId, bool success, const QString& error);

    [[nodiscard]] ConversationEntry* findEntry(int id) noexcept;
    [[nodiscard]] ConversationEntry* activeEntry() noexcept;
    [[nodiscard]] const ConversationEntry* activeEntry() const noexcept;

    ConversationEntry* createConversation();
    void setActiveConversation(int id);
    void startGeneration(const QString& prompt);
    void finalizeStream(MessageStatus status, const QString& note);
    void touchActiveConversation();
    void refreshCanRegenerate();
    void resetMetrics();
    void publishMetrics();
    void rebuildDiagnostics();

    std::unique_ptr<runtime::ModelBackend> m_backend;

    // The controller owns the thread so shutdown order is explicit: cancel the
    // work, stop the loop, wait for it, and only then destroy the worker.
    QThread m_generationThread;
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
    core::ResponseId m_activeResponse = core::kInvalidResponseId;
    core::GenerationId m_activeGeneration = core::kInvalidGenerationId;
    quint64 m_nextRequestId = 1;
    quint64 m_activeRequestId = 0;

    runtime::CudaProbe m_probe;
    QVariantList m_runtimeDiagnostics;
};

} // namespace kestrel::app
