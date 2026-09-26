#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QThread>
#include <QVariantList>

#include <memory>

#include "core/voicesession.h"
#include "runtime/cudadevice.h"
#include "runtime/modelbackend.h"

namespace kestrel::app {

class GenerationWorker;

// QML-facing application state.
//
// The controller is the only place that translates portable runtime concepts
// into Qt properties. It never includes a CUDA or TensorRT header: it reports
// device facts through the portable structs in runtime/cudadevice.h, so the UI
// behaves identically whether or not those SDKs were compiled in.
//
// Everything here runs on the UI thread. Generation does not: it is handed to
// GenerationWorker, and the results arrive back as queued signals.
class AppController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList messages READ messages NOTIFY messagesChanged)
    Q_PROPERTY(QString conversationTitle READ conversationTitle NOTIFY conversationTitleChanged)
    Q_PROPERTY(QString backendName READ backendName NOTIFY runtimeChanged)
    Q_PROPERTY(QString modelName READ modelName NOTIFY runtimeChanged)
    Q_PROPERTY(QString runtimeDetail READ runtimeDetail NOTIFY runtimeChanged)
    Q_PROPERTY(bool generating READ generating NOTIFY generatingChanged)
    Q_PROPERTY(bool sidebarOpen READ sidebarOpen WRITE setSidebarOpen NOTIFY sidebarOpenChanged)
    Q_PROPERTY(bool diagnosticsOpen READ diagnosticsOpen WRITE setDiagnosticsOpen NOTIFY diagnosticsOpenChanged)

    // Live generation metrics. Throughput is measured here rather than read
    // from the backend, so it reflects what the user actually experienced.
    Q_PROPERTY(double tokensPerSecond READ tokensPerSecond NOTIFY metricsChanged)
    Q_PROPERTY(int tokensGenerated READ tokensGenerated NOTIFY metricsChanged)
    Q_PROPERTY(QString contextSummary READ contextSummary NOTIFY metricsChanged)

    // Voice conversation state, projected from the core VoiceSession machine.
    Q_PROPERTY(QString voiceState READ voiceState NOTIFY voiceChanged)
    Q_PROPERTY(bool canPause READ canPause NOTIFY voiceChanged)
    Q_PROPERTY(bool canResume READ canResume NOTIFY voiceChanged)
    Q_PROPERTY(bool canBargeIn READ canBargeIn NOTIFY voiceChanged)

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

    [[nodiscard]] QVariantList messages() const;
    [[nodiscard]] QString conversationTitle() const;
    [[nodiscard]] QString backendName() const;
    [[nodiscard]] QString modelName() const;
    [[nodiscard]] QString runtimeDetail() const;
    [[nodiscard]] bool generating() const noexcept;
    [[nodiscard]] bool sidebarOpen() const noexcept;
    [[nodiscard]] bool diagnosticsOpen() const noexcept;

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
    void setDiagnosticsOpen(bool open);

    Q_INVOKABLE void sendMessage(const QString& text);
    Q_INVOKABLE void stopGeneration();
    Q_INVOKABLE void pauseConversation();
    Q_INVOKABLE void resumeConversation();
    Q_INVOKABLE void newConversation();
    // Re-runs device discovery. Probing is cheap, but it is a driver call, so
    // it is explicit rather than happening on every property read.
    Q_INVOKABLE void refreshRuntime();

signals:
    void messagesChanged();
    void conversationTitleChanged();
    void runtimeChanged();
    void generatingChanged();
    void sidebarOpenChanged();
    void diagnosticsOpenChanged();
    void metricsChanged();
    void voiceChanged();

private:
    // Worker callbacks. These arrive from the generation thread and are
    // delivered on the UI thread by the queued connections in the constructor.
    void onGenerationToken(quint64 requestId, const QString& token);
    void onGenerationFinished(quint64 requestId, bool success, const QString& error);

    void appendMessage(const QString& role, const QString& content);
    void appendToLastAssistantMessage(const QString& text);
    void rebuildDiagnostics();
    void resetMetrics();
    void publishMetrics();

    std::unique_ptr<runtime::ModelBackend> m_backend;

    // The controller owns the thread so shutdown order is explicit: cancel
    // the work, stop the loop, wait for it, and only then destroy the worker.
    QThread m_generationThread;
    GenerationWorker* m_worker = nullptr;
    runtime::CudaProbe m_probe;
    QVariantList m_runtimeDiagnostics;
    QString m_conversationTitle = QStringLiteral("New conversation");
    QVariantList m_messages;
    bool m_generating = false;
    bool m_sidebarOpen = true;
    bool m_diagnosticsOpen = false;

    // Generation bookkeeping. Token counts and the clock are the basis for the
    // throughput figure; the clock only runs while a response is generating.
    QElapsedTimer m_generationClock;
    int m_tokensGenerated = 0;
    double m_tokensPerSecond = 0.0;
    qint64 m_lastMetricsPublish = 0;

    // Voice state machine. Owns the response timeline and is the authority on
    // what the UI is allowed to offer next.
    core::VoiceSession m_voice;
    core::ResponseId m_activeResponse = core::kInvalidResponseId;
    core::GenerationId m_activeGeneration = core::kInvalidGenerationId;
    quint64 m_nextRequestId = 1;
    quint64 m_activeRequestId = 0;
};

} // namespace kestrel::app
