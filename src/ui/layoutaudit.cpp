#include "ui/layoutaudit.h"

#include <QMetaObject>
#include <QQuickWindow>
#include <QRectF>

#include <algorithm>

namespace kestrel::ui {
namespace {

/// Below this many pixels an item is collapsed rather than short.
///
/// Half a pixel, because QML computes heights in floating point and a cell
/// that should be zero comes out as something like 1e-13 rather than 0. It is
/// not a fudge on the threshold that decides a problem: the decision is
/// whether content spills past the cell, and this only says which of those is
/// "no height at all".
constexpr double kCollapsed = 0.5;

/// Whether an item is one of the layouts QML's RowLayout and friends create.
///
/// QQuickLayout itself is private to Qt, so a layout cannot be asked what it
/// is from public headers. The four concrete classes behind the QML layout
/// types are not, and those names have been stable since Qt 5. Spelled as an
/// enumeration rather than a substring test so that a future class called, say,
/// QQuickLayoutHelper is not mistaken for one.
bool isLayout(const QQuickItem* item) {
    const QByteArray name = item->metaObject()->className();
    return name == "QQuickRowLayout" || name == "QQuickColumnLayout" || name == "QQuickGridLayout"
           || name == "QQuickFlowLayout";
}

/// A path an author can paste into a file search to find the offending item.
QString pathTo(const QQuickItem* item) {
    QStringList parts;
    for (const QQuickItem* node = item; node != nullptr; node = node->parentItem()) {
        const QString name = node->objectName();
        parts.prepend(name.isEmpty() ? QString::fromLatin1(node->metaObject()->className()) : name);
    }
    return parts.join(QLatin1Char('/'));
}

QRectF inScene(const QQuickItem* item) {
    return item->mapRectToScene(QRectF(0.0, 0.0, item->width(), item->height()));
}

/// The tallest visible thing inside `cell`, and how far it reaches below it.
///
/// Returns false when the cell's content fits, which is the only case that is
/// not a problem. Only visible descendants count: a cell can legitimately hold
/// a hidden label for later, and hidden content is not drawn over anything.
bool contentOverflows(const QQuickItem* cell, double& tallestBelow) {
    const QRectF bounds = inScene(cell);
    tallestBelow = 0.0;
    bool found = false;

    // Breadth-first over the subtree, skipping the cell itself. Deep enough for
    // a switch inside a hit target inside a row inside a panel, which is four
    // levels for the shape that actually broke.
    QVector<QQuickItem*> pending = cell->childItems();
    while (!pending.isEmpty()) {
        const QQuickItem* node = pending.takeFirst();
        if (!node->isVisible()) {
            continue;
        }
        const QRectF rect = inScene(node);
        if (node->width() > 0.0 && node->height() > 0.0
            && (rect.top() < bounds.top() - kCollapsed
                || rect.bottom() > bounds.bottom() + kCollapsed)) {
            tallestBelow = std::max(tallestBelow, rect.bottom() - bounds.bottom());
            found = true;
        }
        const auto& children = node->childItems();
        pending.append(children);
    }
    return found;
}

void auditNode(QQuickItem* node, QVector<LayoutProblem>& out) {
    if (isLayout(node)) {
        const auto cells = node->childItems();
        for (QQuickItem* cell : cells) {
            if (!cell->isVisible()) {
                continue;
            }
            // The failure that shipped: a cell with no height at all, holding
            // content that therefore lands on top of the next cell's.
            if (cell->height() < kCollapsed) {
                double spill = 0.0;
                if (contentOverflows(cell, spill)) {
                    out.append({pathTo(cell),
                                QStringLiteral("a layout cell is %1px tall and holds content "
                                               "reaching %2px past it, so the next cell is drawn over it")
                                    .arg(cell->height())
                                    .arg(spill)});
                }
            }
        }
        // The backstop: whatever the cause, two cells of one layout may not
        // share space. Expressed as an intersection of the two rectangles by
        // more than half a pixel so that cells which merely touch, as they do
        // in a healthy column, are not reported.
        for (int i = 0; i < cells.size(); ++i) {
            if (!cells.at(i)->isVisible()) {
                continue;
            }
            const QRectF first = inScene(cells.at(i));
            for (int j = i + 1; j < cells.size(); ++j) {
                if (!cells.at(j)->isVisible()) {
                    continue;
                }
                const QRectF second = inScene(cells.at(j));
                const double overlapX = std::min(first.right(), second.right())
                                        - std::max(first.left(), second.left());
                const double overlapY = std::min(first.bottom(), second.bottom())
                                        - std::max(first.top(), second.top());
                if (overlapX > kCollapsed && overlapY > kCollapsed) {
                    out.append({pathTo(node),
                                QStringLiteral("two cells overlap by %1x%2px")
                                    .arg(overlapX)
                                    .arg(overlapY)});
                }
            }
        }
    }
    const auto& children = node->childItems();
    for (QQuickItem* child : children) {
        auditNode(child, out);
    }
}

} // namespace

QVector<LayoutProblem> auditLayouts(QQuickItem* root) {
    QVector<LayoutProblem> out;
    if (root == nullptr) {
        return out;
    }
    auditNode(root, out);
    // Sorted so that a report is the same on every run and two runs can be
    // diffed, which is the only reason to keep one of these.
    std::sort(out.begin(), out.end(), [](const LayoutProblem& a, const LayoutProblem& b) {
        return a.where != b.where ? a.where < b.where : a.what < b.what;
    });
    return out;
}

} // namespace kestrel::ui
