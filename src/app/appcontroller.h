#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

#include <memory>

#include "runtime/cudadevice.h"
#include "runtime/modelbackend.h"

namespace kestrel::app {

// QML-facing application state.
//
// The controller is the only place that translates portable runtime concepts
// into Qt properties. It never includes a CUDA or TensorRT header: it reports
// device facts through the portable structs in runtime/cudadevice.h, so the UI
// behaves identically whether or not those SDKs were compiled in.
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

    [[nodiscard]] QVariantList messages() const;
    [[nodiscard]] QString conversationTitle() const;
    [[nodiscard]] QString backendName() const;
    [[nodiscard]] QString modelName() const;
    [[nodiscard]] QString runtimeDetail() const;
    [[nodiscard]] bool generating() const noexcept;
    [[nodiscard]] bool sidebarOpen() const noexcept;
    [[nodiscard]] bool diagnosticsOpen() const noexcept;

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

private:
    void appendMessage(const QString& role, const QString& content);
    void rebuildDiagnostics();

    std::unique_ptr<runtime::ModelBackend> m_backend;
    runtime::CudaProbe m_probe;
    QVariantList m_runtimeDiagnostics;
    QString m_conversationTitle = QStringLiteral("New conversation");
    QVariantList m_messages;
    bool m_generating = false;
    bool m_sidebarOpen = true;
    bool m_diagnosticsOpen = false;
};

} // namespace kestrel::app
