#include "app/messagemodel.h"

namespace kestrel::app {

QString toStatusString(MessageStatus status) {
    switch (status) {
    case MessageStatus::Streaming:
        return QStringLiteral("streaming");
    case MessageStatus::Complete:
        return QStringLiteral("complete");
    case MessageStatus::Stopped:
        return QStringLiteral("stopped");
    case MessageStatus::Failed:
        return QStringLiteral("failed");
    }
    return QStringLiteral("complete");
}

QString toAuthorString(core::MessageRole role) {
    switch (role) {
    case core::MessageRole::User:
        return QStringLiteral("user");
    case core::MessageRole::Assistant:
        return QStringLiteral("assistant");
    case core::MessageRole::System:
        return QStringLiteral("system");
    case core::MessageRole::Tool:
        return QStringLiteral("tool");
    }
    return QStringLiteral("assistant");
}

MessageModel::MessageModel(QObject* parent)
    : QAbstractListModel(parent) {}

int MessageModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid() || m_entry == nullptr) {
        return 0;
    }
    return static_cast<int>(m_entry->conversation.size());
}

QVariant MessageModel::data(const QModelIndex& index, int role) const {
    if (m_entry == nullptr || !index.isValid() || index.row() < 0
        || index.row() >= rowCount()) {
        return {};
    }

    const auto row = static_cast<std::size_t>(index.row());
    const core::Message& message = m_entry->conversation.messages()[row];
    const MessageExtra& extra = m_entry->extras[row];

    switch (role) {
    case AuthorRole:
        return toAuthorString(message.role);
    case ContentRole:
        return QString::fromStdString(message.content);
    case StatusRole:
        return toStatusString(extra.status);
    case NoteRole:
        return extra.note;
    default:
        return {};
    }
}

QHash<int, QByteArray> MessageModel::roleNames() const {
    return {
        {AuthorRole, QByteArrayLiteral("author")},
        {ContentRole, QByteArrayLiteral("content")},
        {StatusRole, QByteArrayLiteral("status")},
        {NoteRole, QByteArrayLiteral("note")},
    };
}

void MessageModel::setEntry(ConversationEntry* entry) {
    if (m_entry == entry) {
        return;
    }
    beginResetModel();
    m_entry = entry;
    endResetModel();
}

void MessageModel::appendMessage(core::MessageRole role, const QString& content,
                                 MessageStatus status) {
    if (m_entry == nullptr) {
        return;
    }
    const int row = rowCount();
    beginInsertRows({}, row, row);
    m_entry->conversation.addMessage(role, content.toStdString());
    m_entry->extras.push_back({status, {}});
    endInsertRows();
}

void MessageModel::appendToLastMessage(const QString& text) {
    if (m_entry == nullptr || m_entry->conversation.size() == 0 || text.isEmpty()) {
        return;
    }
    m_entry->conversation.appendToLastMessage(text.toStdString());
    const QModelIndex last = index(rowCount() - 1);
    emit dataChanged(last, last, {ContentRole});
}

void MessageModel::setLastMessageStatus(MessageStatus status, const QString& note) {
    if (m_entry == nullptr || m_entry->extras.empty()) {
        return;
    }
    m_entry->extras.back() = {status, note};
    const QModelIndex last = index(rowCount() - 1);
    emit dataChanged(last, last, {StatusRole, NoteRole});
}

void MessageModel::removeLastMessage() {
    if (m_entry == nullptr || m_entry->conversation.size() == 0) {
        return;
    }
    const int row = rowCount() - 1;
    beginRemoveRows({}, row, row);
    m_entry->conversation.removeLastMessage();
    m_entry->extras.pop_back();
    endRemoveRows();
}

} // namespace kestrel::app
