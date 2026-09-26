#pragma once

#include <QAbstractListModel>

#include "app/conversationentry.h"

namespace kestrel::app {

// List model over the active conversation's messages. All mutations of the
// active conversation go through this model so the view receives granular
// change notifications (one dataChanged per streamed chunk instead of a full
// model rebuild).
class MessageModel final : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        AuthorRole = Qt::UserRole + 1,
        ContentRole,
        StatusRole,
        NoteRole,
    };

    explicit MessageModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    // Switches the model to another conversation (or nullptr). The entry must
    // outlive the model or be replaced before destruction.
    void setEntry(ConversationEntry* entry);

    void appendMessage(core::MessageRole role, const QString& content, MessageStatus status);
    void appendToLastMessage(const QString& text);
    void setLastMessageStatus(MessageStatus status, const QString& note = {});
    void removeLastMessage();

private:
    ConversationEntry* m_entry = nullptr;
};

[[nodiscard]] QString toStatusString(MessageStatus status);
[[nodiscard]] QString toAuthorString(core::MessageRole role);

} // namespace kestrel::app
