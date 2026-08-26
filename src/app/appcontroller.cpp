#include "app/appcontroller.h"

#include "runtime/mockbackend.h"

#include <QVariantMap>

namespace kestrel::app {

AppController::AppController(QObject* parent)
    : QObject(parent), m_backend(std::make_unique<runtime::MockBackend>()) {
    refreshRuntime();
}

QVariantList AppController::messages() const {
    return m_messages;
}

QString AppController::conversationTitle() const {
    return m_conversationTitle;
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

bool AppController::generating() const noexcept {
    return m_generating;
}

bool AppController::sidebarOpen() const noexcept {
    return m_sidebarOpen;
}

void AppController::setSidebarOpen(bool open) {
    if (m_sidebarOpen == open) {
        return;
    }
    m_sidebarOpen = open;
    emit sidebarOpenChanged();
}

void AppController::appendMessage(runtime::BackendKind role, const QString& content) {
    QString roleName = QStringLiteral("assistant");
    if (role == runtime::BackendKind::Mock) {
        roleName = QStringLiteral("assistant");
    }

    QVariantMap message;
    message.insert(QStringLiteral("role"), roleName);
    message.insert(QStringLiteral("content"), content);
    m_messages.append(message);
    emit messagesChanged();
}

void AppController::sendMessage(const QString& text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || m_generating) {
        return;
    }

    QVariantMap userMessage;
    userMessage.insert(QStringLiteral("role"), QStringLiteral("user"));
    userMessage.insert(QStringLiteral("content"), trimmed);
    m_messages.append(userMessage);

    QVariantMap assistantMessage;
    assistantMessage.insert(QStringLiteral("role"), QStringLiteral("assistant"));
    assistantMessage.insert(QStringLiteral("content"), QString());
    m_messages.append(assistantMessage);
    emit messagesChanged();

    if (m_messages.size() == 2) {
        m_conversationTitle = trimmed.left(34);
        emit conversationTitleChanged();
    }

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
            refreshRuntime();
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
    emit runtimeChanged();
}

} // namespace kestrel::app
