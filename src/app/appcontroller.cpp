#include "app/appcontroller.h"

#include "runtime/backendregistry.h"

#include <QVariantMap>

namespace kestrel::app {

namespace {

QString fromStd(const std::string& text) {
    return QString::fromStdString(text);
}

} // namespace

AppController::AppController(QObject* parent)
    : QObject(parent), m_backend(runtime::selectBackend(runtime::BackendKind::Mock)) {
    m_probe = runtime::probeCuda();
    rebuildDiagnostics();
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

void AppController::sendMessage(const QString& text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || m_generating) {
        return;
    }

    appendMessage(QStringLiteral("user"), trimmed);
    appendMessage(QStringLiteral("assistant"), QString());

    if (m_messages.size() == 2) {
        m_conversationTitle = trimmed.left(34);
        emit conversationTitleChanged();
    }
    emit messagesChanged();

    m_generating = true;
    emit generatingChanged();

    runtime::GenerationRequest request{trimmed.toStdString(), 0.7F, 512};
    m_backend->generate(
        request,
        [this](std::string_view token) {
            if (m_messages.isEmpty()) {
                return;
            }
            auto assistant = m_messages.last().toMap();
            assistant.insert(QStringLiteral("content"),
                             assistant.value(QStringLiteral("content")).toString()
                                 + QString::fromUtf8(token.data(), static_cast<int>(token.size())));
            m_messages.replace(m_messages.size() - 1, assistant);
            emit messagesChanged();
        },
        [this](bool, std::string_view) {
            m_generating = false;
            emit generatingChanged();
            emit runtimeChanged();
        });
}

void AppController::stopGeneration() {
    if (!m_generating) {
        return;
    }
    m_backend->cancel();
}

void AppController::newConversation() {
    m_messages.clear();
    m_conversationTitle = QStringLiteral("New conversation");
    emit messagesChanged();
    emit conversationTitleChanged();
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
