#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

#include <memory>

#include "runtime/modelbackend.h"

namespace kestrel::app {

class AppController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList messages READ messages NOTIFY messagesChanged)
    Q_PROPERTY(QString conversationTitle READ conversationTitle NOTIFY conversationTitleChanged)
    Q_PROPERTY(QString backendName READ backendName NOTIFY runtimeChanged)
    Q_PROPERTY(QString modelName READ modelName NOTIFY runtimeChanged)
    Q_PROPERTY(QString runtimeDetail READ runtimeDetail NOTIFY runtimeChanged)
    Q_PROPERTY(bool generating READ generating NOTIFY generatingChanged)
    Q_PROPERTY(bool sidebarOpen READ sidebarOpen WRITE setSidebarOpen NOTIFY sidebarOpenChanged)

public:
    explicit AppController(QObject* parent = nullptr);

    [[nodiscard]] QVariantList messages() const;
    [[nodiscard]] QString conversationTitle() const;
    [[nodiscard]] QString backendName() const;
    [[nodiscard]] QString modelName() const;
    [[nodiscard]] QString runtimeDetail() const;
    [[nodiscard]] bool generating() const noexcept;
    [[nodiscard]] bool sidebarOpen() const noexcept;

    void setSidebarOpen(bool open);

    Q_INVOKABLE void sendMessage(const QString& text);
    Q_INVOKABLE void stopGeneration();
    Q_INVOKABLE void newConversation();

signals:
    void messagesChanged();
    void conversationTitleChanged();
    void runtimeChanged();
    void generatingChanged();
    void sidebarOpenChanged();

private:
    void appendMessage(runtime::BackendKind role, const QString& content);
    void refreshRuntime();

    std::unique_ptr<runtime::ModelBackend> m_backend;
    QString m_conversationTitle = QStringLiteral("New conversation");
    QVariantList m_messages;
    bool m_generating = false;
    bool m_sidebarOpen = true;
};

} // namespace kestrel::app
