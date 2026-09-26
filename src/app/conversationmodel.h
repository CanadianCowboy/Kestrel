#pragma once

#include <QAbstractListModel>
#include <QString>

#include <memory>
#include <vector>

#include "app/conversationentry.h"

namespace kestrel::app {

// Sidebar list of conversations with an optional search filter over titles
// and message contents. The entry storage is owned by AppController; this
// model only reads it and must be told when it changes.
class ConversationModel final : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        TitleRole,
        PreviewRole,
        StampRole,
    };

    using EntryList = std::vector<std::unique_ptr<ConversationEntry>>;

    explicit ConversationModel(const EntryList* entries, QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void setSearchQuery(const QString& query);

    // Rebuilds the filtered view after entries were added or removed.
    void refilter();

    // Refreshes one conversation's row after its title, preview, or stamp
    // changed. Falls back to a refilter while a search is active because the
    // change may alter which conversations match.
    void notifyEntryChanged(int id);

private:
    [[nodiscard]] const ConversationEntry* findEntry(int id) const;
    [[nodiscard]] bool matches(const ConversationEntry& entry) const;

    const EntryList* m_entries;
    QString m_query;
    std::vector<int> m_filteredIds;
};

} // namespace kestrel::app
