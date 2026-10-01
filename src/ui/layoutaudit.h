/// Why the user can see a layout that the tests all called correct.
///
/// The diagnostics panel shipped with three switches drawn on top of each
/// other and nothing failed. That is not a matter of a missing assertion; it
/// is a matter of every assertion in the tree being about the wrong thing. The
/// app tests assert a reply arrived, the runtime tests assert a backend loaded,
/// the QML tests assert one bubble's width -- and a bubble's width says nothing
/// about whether its *neighbour* has been given any height to sit in. The
/// screenshot check could not see it either: a row that loses its height does
/// not go dark, it draws its label on top of the one below, so the picture
/// ends up with more contrast in it than before.
///
/// This is the check that sees it, and it does not work by looking at pixels.
/// It asks the scene graph where every layout put its cells, which is a
/// question with an exact answer:
///
///   * a cell that is shorter than the content inside it, so the next cell is
///     drawn over it, and
///   * two cells of one layout that overlap at all, which no layout may do.
///
/// Both are properties of the tree rather than inferences from a picture, so
/// there is no threshold to tune and nothing here that can be argued with. The
/// unit test below builds the broken shape on purpose and asserts this finds
/// it, because a checker that has never been shown to fail is indistinguishable
/// from one that cannot.
#ifndef KESTREL_UI_LAYOUTAUDIT_H
#define KESTREL_UI_LAYOUTAUDIT_H

#include <QQuickItem>
#include <QString>
#include <QVector>

namespace kestrel::ui {

/// One thing wrong with the way the scene was laid out.
struct LayoutProblem {
    /// Where in the tree, as a slash-separated path an author can find again.
    QString where;
    /// What is wrong, in one sentence naming the numbers.
    QString what;
};

/// Every cell in the tree that a layout put in the wrong place.
///
/// Walks the whole subtree, including branches the user cannot currently see:
/// a collapsed row in a closed panel is still a collapsed row. Cells are only
/// judged where they are themselves visible, though, so that "the user would
/// see this" stays true of every problem reported -- the caller is expected to
/// open whatever panels it wants judged before asking.
[[nodiscard]] QVector<LayoutProblem> auditLayouts(QQuickItem* root);

} // namespace kestrel::ui

#endif // KESTREL_UI_LAYOUTAUDIT_H
