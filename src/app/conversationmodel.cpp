#include "app/conversationmodel.h"

#include <QLocale>

#include <algorithm>

namespace kestrel::app {

namespace {

QString previewText(const ConversationEntry& entry) {
    if (entry.conversation.size() == 0) {
        return QStringLiteral("No messages yet");
    }
    const QString text = QString::fromStdString(entry.conversation.messages().back().content);
    return text.simplified();
}

QString stampText(const QDateTime& updatedAt) {
    if (!updatedAt.isValid()) {
        return {};
    }
    const QDate today = QDate::currentDate();
    if (updatedAt.date() == today) {
        return updatedAt.toString(QStringLiteral("HH:mm"));
    }
    if (updatedAt.date().year() == today.year()) {
        return QLocale().toString(updatedAt.date(), QStringLiteral("MMM d"));
    }
    return QLocale().toString(updatedAt.date(), QStringLiteral("MMM d, yyyy"));
}

} // namespace

ConversationModel::ConversationModel(const EntryList* entries, QObject* parent)
    : QAbstractListModel(parent), m_entries(entries) {
    refilter();
}

int ConversationModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(m_filteredIds.size());
}

QVariant ConversationModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }

    const ConversationEntry* entry = findEntry(m_filteredIds[static_cast<std::size_t>(index.row())]);
    if (entry == nullptr) {
        return {};
    }

    switch (role) {
    case IdRole:
        return entry->id;
    case TitleRole:
        return QString::fromStdString(entry->conversation.title());
    case PreviewRole:
        return previewText(*entry);
    case StampRole:
        return stampText(entry->updatedAt);
    default:
        return {};
    }
}

QHash<int, QByteArray> ConversationModel::roleNames() const {
    return {
        {IdRole, QByteArrayLiteral("convId")},
        {TitleRole, QByteArrayLiteral("title")},
        {PreviewRole, QByteArrayLiteral("preview")},
        {StampRole, QByteArrayLiteral("stamp")},
    };
}

void ConversationModel::setSearchQuery(const QString& query) {
    const QString trimmed = query.trimmed();
    if (m_query == trimmed) {
        return;
    }
    m_query = trimmed;
    refilter();
}

void ConversationModel::refilter() {
    beginResetModel();
    m_filteredIds.clear();
    if (m_entries != nullptr) {
        for (const auto& entry : *m_entries) {
            if (matches(*entry)) {
                m_filteredIds.push_back(entry->id);
            }
        }
    }
    endResetModel();
}

void ConversationModel::notifyEntryChanged(int id) {
    if (!m_query.isEmpty()) {
        refilter();
        return;
    }
    const auto it = std::find(m_filteredIds.begin(), m_filteredIds.end(), id);
    if (it == m_filteredIds.end()) {
        return;
    }
    const int row = static_cast<int>(std::distance(m_filteredIds.begin(), it));
    emit dataChanged(index(row), index(row));
}

const ConversationEntry* ConversationModel::findEntry(int id) const {
    if (m_entries == nullptr) {
        return nullptr;
    }
    for (const auto& entry : *m_entries) {
        if (entry->id == id) {
            return entry.get();
        }
    }
    return nullptr;
}

bool ConversationModel::matches(const ConversationEntry& entry) const {
    if (m_query.isEmpty()) {
        return true;
    }
    if (QString::fromStdString(entry.conversation.title()).contains(m_query, Qt::CaseInsensitive)) {
        return true;
    }
    for (const core::Message& message : entry.conversation.messages()) {
        if (QString::fromStdString(message.content).contains(m_query, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

} // namespace kestrel::app
