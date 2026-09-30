/// Geometry tests for the QML the user actually looks at.
///
/// The chat was unreadable for a long stretch and nothing failed. Every other
/// test asserts that a reply arrived, which was true throughout: the reply was
/// being written into a bubble four pixels wide, floating over the composer.
/// Asserting "a reply came back" cannot see that. Asserting that the bubble is
/// wide enough for its text, tall enough to contain it, and that the row is
/// tall enough to hold the bubble, can.
///
/// These load the real component from src/ui rather than a copy, so the test
/// tracks the file the app uses. A test measuring a hand-copied delegate would
/// keep passing after the real one broke.
#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QUrl>
#include <QtGui/QGuiApplication>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

#include "ui/layoutaudit.h"

#include <cmath>
#include <iostream>
#include <memory>
#include <string>

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what) {
    std::cout << (condition ? "  ok   " : "  FAIL ") << what << "\n";
    if (!condition) {
        ++g_failures;
    }
}

void checkClose(double actual, double expected, double tolerance, const std::string& what) {
    const bool ok = std::fabs(actual - expected) <= tolerance;
    std::cout << (ok ? "  ok   " : "  FAIL ") << what << " (expected " << expected << ", got " << actual
              << ")\n";
    if (!ok) {
        ++g_failures;
    }
}

/// One measured bubble, with the numbers the assertions need.
struct Bubble {
    QQuickItem* root = nullptr;
    QQuickItem* bubble = nullptr;
    QQuickItem* body = nullptr;
    double width = 0.0;
    double height = 0.0;
    double implicitHeight = 0.0;
    /// What a ListView would use for the row, as opposed to implicitHeight.
    double rowHeight = 0.0;
    double bodyWidth = 0.0;
    double bodyHeight = 0.0;
    double bodyNeeds = 0.0;
    double cap = 0.0;
};

/// Instantiates MessageBubble.qml at a known width and measures it.
///
/// The item is parented into a real, shown window: an item that is never in an
/// exposed scene can skip polish and layout, and the whole point is to measure
/// what the user would actually see.
Bubble measure(QQmlEngine& engine, const char* author, const QString& content, const char* status,
               double rowWidth) {
    Bubble out;
    static QQuickWindow* window = new QQuickWindow();
    window->resize(static_cast<int>(rowWidth), 900);

    QQmlComponent component(&engine);
    component.loadUrl(QUrl::fromLocalFile(QStringLiteral(KESTREL_UI_DIR "/MessageBubble.qml")));
    if (component.isError()) {
        std::cout << "  FAIL cannot load MessageBubble: "
                  << component.errorString().toStdString() << "\n";
        ++g_failures;
        return out;
    }

    // The four message properties are required, because that is how a ListView
    // delegate receives the model's roles. They are therefore supplied at
    // creation rather than set afterwards, which is also closer to what the
    // delegate does: this test used to set them by hand and so measured a
    // bubble the app itself never produced.
    QVariantMap initial;
    initial.insert(QStringLiteral("author"), QString::fromLatin1(author));
    initial.insert(QStringLiteral("content"), content);
    initial.insert(QStringLiteral("status"), QString::fromLatin1(status));
    initial.insert(QStringLiteral("note"), QString());

    std::unique_ptr<QObject> object(component.createWithInitialProperties(initial));
    if (!object) {
        std::cout << "  FAIL cannot create MessageBubble: "
                  << component.errorString().toStdString() << "\n";
        ++g_failures;
        return out;
    }

    auto* root = qobject_cast<QQuickItem*>(object.get());
    if (root == nullptr) {
        std::cout << "  FAIL MessageBubble is not an Item\n";
        ++g_failures;
        return out;
    }

    root->setWidth(rowWidth);
    root->setParentItem(window->contentItem());

    window->show();
    QCoreApplication::processEvents();
    root->polish();
    QCoreApplication::processEvents();

    out.root = root;
    out.bubble = root->findChild<QQuickItem*>(QStringLiteral("bubble"));
    out.body = root->findChild<QQuickItem*>(QStringLiteral("bodyText"));
    if (out.bubble == nullptr || out.body == nullptr) {
        std::cout << "  FAIL MessageBubble is missing its bubble or body item\n";
        ++g_failures;
        return out;
    }

    out.width = out.bubble->width();
    out.height = out.bubble->height();
    out.implicitHeight = root->implicitHeight();
    out.rowHeight = root->height();
    out.bodyWidth = out.body->width();
    out.bodyHeight = out.body->height();
    out.bodyNeeds = out.body->implicitHeight();
    out.cap = rowWidth * 0.78;
    return out;
}

/// The invariants that matter, checked for every shape of message. Each of
/// these was false at some point while the chat looked broken.
void checkSane(const Bubble& b, const std::string& label) {
    check(b.width > 0, label + ": the bubble has a positive width");
    check(b.height > 0, label + ": the bubble has a positive height");
    check(b.bodyWidth > 0, label + ": the text has a positive width");
    check(b.bodyHeight > 0, label + ": the text has a positive height");
    // The text must fit inside the bubble's padding.
    check(b.bodyHeight <= b.height - 30 + 1, label + ": the text fits inside the bubble");
    check(b.bodyWidth <= b.width - 30 + 1, label + ": the text is not wider than the bubble");
    // The row has to be able to hold the bubble, or the bubble paints outside
    // its own row and lands on top of the composer. The height a ListView uses
    // is `height`, not implicitHeight, and checking only the latter is what let
    // a zero-height conversation pass every assertion here.
    check(b.rowHeight >= b.height, label + ": the row is tall enough to hold the bubble");
    check(b.rowHeight > 0, label + ": the row is not zero tall");
}

/// A short reply still occupies a real bubble.
void testShortMessageIsVisible(QQmlEngine& engine) {
    std::cout << "a short message is still visible\n";
    const Bubble b = measure(engine, "assistant", QStringLiteral("Hi."), "complete", 1200.0);
    checkClose(b.cap, b.width, 0.5, "the bubble takes the full cap");
    checkSane(b, "short");
}

/// A long message wraps, and the bubble grows to hold the wrapped lines.
void testLongMessageWrapsAndGrows(QQmlEngine& engine) {
    std::cout << "a long message wraps and the bubble grows\n";
    const QString longText = QStringLiteral(
        "Give me a one-line status message I can show while the model warms up. "
        "Something short and calm, and do not mention that it is an assistant "
        "or explain how it works, because nobody asked for any of that.");
    const Bubble b = measure(engine, "user", longText, "complete", 1200.0);
    checkClose(b.cap, b.width, 0.5, "the bubble stops at the cap");
    // One line at 14px with 1.35 line height is about 19px. A wrapped message
    // must be taller than that, or the bubble clipped the text.
    check(b.bodyNeeds > 25.0, "the text needed more than one line");
    check(b.height > 30.0 + 25.0, "the bubble grew to hold the wrapped lines");
    checkSane(b, "long");
}

/// A narrow window: the cap shrinks with it and nothing goes negative.
void testNarrowWindowStillWorks(QQmlEngine& engine) {
    std::cout << "a narrow window still works\n";
    const Bubble b =
        measure(engine, "user", QStringLiteral("A reasonably long sentence that has to wrap."),
                "complete", 300.0);
    checkClose(b.cap, b.width, 0.5, "the bubble follows the narrower cap");
    checkSane(b, "narrow");
}

/// An empty message: the in-flight state, before any token arrives.
void testEmptyMessageReservesSpace(QQmlEngine& engine) {
    std::cout << "an empty message reserves space\n";
    const Bubble b = measure(engine, "assistant", QString(), "streaming", 1200.0);
    check(b.width > 0, "the bubble is visible before the first token");
    check(b.height > 0, "the bubble has height before the first token");
    check(b.rowHeight > 0, "the row reserves space before the first token");
}

/// A failed response carries a second line, which must also fit.
void testStatusNoteFitsInsideTheBubble(QQmlEngine& engine) {
    std::cout << "a status note fits inside the bubble\n";
    const Bubble b =
        measure(engine, "assistant", QStringLiteral("Partial answer"), "failed", 1200.0);
    const Bubble plain =
        measure(engine, "assistant", QStringLiteral("Partial answer"), "complete", 1200.0);
    // The Column stacks body and note, so the bubble must be taller when the
    // note is showing than when it is not.
    check(b.height > plain.height, "the note makes the bubble taller");
    checkSane(b, "noted");
}

/// The text must actually wrap at the cap rather than being clipped. A long
/// single word with no spaces is the case that catches a width that looks
/// right but silently truncates.
void testUnbreakableTextIsNotClipped(QQmlEngine& engine) {
    std::cout << "unbreakable text is not clipped\n";
    const QString blob(400, QLatin1Char('x'));
    const Bubble b = measure(engine, "user", blob, "complete", 600.0);
    check(b.bodyHeight > 19.0, "a very long word still occupies the text it needs");
    check(b.height >= b.bodyHeight + 30 - 1, "the bubble contains the whole text");
}

/// The whole list, not just one bubble.
///
/// A component can be perfectly sized and still never appear, if the view it
/// sits in gives it no room. This asks the ListView itself how tall each row
/// turned out, which is the number that decides whether anything is visible.
void testListViewGivesRowsRealHeight(QQmlEngine& engine) {
    std::cout << "the list view gives rows real height\n";

    static QQuickWindow* window = new QQuickWindow();
    window->resize(1200, 900);

    QQmlComponent component(&engine);
    // The base URL is the ui directory, so the delegate resolves as a sibling
    // of the real component rather than a copy of it.
    // One root, because a QML document allows exactly one. Declaring the model
    // and the list as siblings was the bug: the parser rejected the second
    // root and pointed at its line, some way past the real mistake.
    component.setData(
        "import QtQuick\n"
        "Item {\n"
        "    ListModel {\n"
        "        id: seeded\n"
        "        ListElement { author: \"user\"; content: \"Hello\"; status: \"complete\"; note: \"\" }\n"
        "        ListElement { author: \"assistant\"; status: \"complete\"; note: \"\";"
        " content: \"Kestrels hover by flying into the wind at exactly the speed it pushes"
        " them back, so their ground speed drops to zero.\" }\n"
        "    }\n"
        "    ListView {\n"
        "        objectName: \"list\"\n"
        "        anchors.fill: parent\n"
        "        model: seeded\n"
        "        spacing: 20\n"
        "        delegate: MessageBubble { width: ListView.view.width }\n"
        "    }\n"
        "}\n",
        QUrl::fromLocalFile(QStringLiteral(KESTREL_UI_DIR "/listprobe.qml")));

    // Created once, and owned from the first create(): calling it in the
    // condition below and then again for `scene` would leave the first scene
    // unowned and running its own ListView and delegates, outside any window,
    // for the rest of the process -- and a failure would not say which call it
    // came from.
    std::unique_ptr<QObject> scene(component.isError() ? nullptr : component.create());
    if (!scene) {
        std::cout << "  FAIL cannot build the list: " << component.errorString().toStdString() << "\n";
        ++g_failures;
        return;
    }
    // The document's root is an Item; create() hands back a QObject* and
    // setParentItem lives on QQuickItem.
    auto* sceneItem = qobject_cast<QQuickItem*>(scene.get());
    if (sceneItem == nullptr) {
        std::cout << "  FAIL the probe root is not an Item\n";
        ++g_failures;
        return;
    }
    // The list fills its parent, and an Item with no size gives it nothing:
    // a zero-height view instantiates almost no delegates, so the test would
    // be measuring a list that had not been given room to exist.
    sceneItem->setWidth(1200);
    sceneItem->setHeight(900);
    sceneItem->setParentItem(window->contentItem());
    window->show();
    QCoreApplication::processEvents();
    sceneItem->polish();
    QCoreApplication::processEvents();

    auto* list = scene->findChild<QQuickItem*>(QStringLiteral("list"));
    if (list == nullptr) {
        std::cout << "  FAIL the probe built no ListView\n";
        ++g_failures;
        return;
    }

    std::cout << "       window " << window->width() << "x" << window->height()
              << "  root " << sceneItem->width() << "x" << sceneItem->height()
              << "  list " << list->width() << "x" << list->height() << "\n";
    check(list->property("count").toInt() == 2, "the list has both messages");
    check(list->width() > 0, "the list has a width to hand its delegates");
    check(list->height() > 0, "the list itself has height");

    // The delegates a ListView has actually instantiated are the Item children
    // of its contentItem. Asking for them any other way means boxing arguments
    // and return values through QMetaObject, where a type mismatch shows up as
    // a silent null and the test quietly measures nothing.
    auto* content = list->property("contentItem").value<QObject*>();
    if (content == nullptr) {
        std::cout << "  FAIL the list has no contentItem\n";
        ++g_failures;
        return;
    }

    int rows = 0;
    for (QObject* child : content->children()) {
        auto* item = qobject_cast<QQuickItem*>(child);
        if (item == nullptr) {
            continue;
        }
        ++rows;
        const std::string label = "row " + std::to_string(rows);
        // A row sized from a bare id instead of ListView.view came out zero
        // wide, and a zero-wide row is an invisible conversation. This is the
        // assertion that caught it.
        checkClose(item->width(), list->width(), 0.5, label + " spans the view");
        check(item->height() > 0, label + " is not zero tall");
    }
    // How many delegates a view has built is its own business and depends on
    // layout timing; that at least one exists is not.
    check(rows >= 1, "the list built at least one row (built " + std::to_string(rows) + ")");
}

/// The four message properties are how a delegate gets its content at all.
///
/// A ListView hands the model's roles to a delegate's *required* properties
/// and to nothing else. Declared as ordinary properties they keep their
/// defaults, every message renders as an empty string, and the transcript goes
/// invisible while every geometry check in this file still passes: a
/// correctly measured, completely empty conversation.
///
/// Checked against the declaration rather than against a rendered row, and
/// that is deliberate. The rendered row is not proof here. The rest of this
/// file sets these four properties by hand, so it measures a bubble the app
/// itself never produces, which is how the whole file stayed green through a
/// blank transcript. The invariant is a declaration in the source, so the
/// source is what gets read.
void testMessagePropertiesAreRequired() {
    std::cout << "the message properties are required, so a delegate receives them\n";

    QFile source(QStringLiteral(KESTREL_UI_DIR "/MessageBubble.qml"));
    if (!source.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::cout << "  FAIL cannot read MessageBubble.qml: "
                  << source.errorString().toStdString() << "\n";
        ++g_failures;
        return;
    }
    const QString text = QString::fromUtf8(source.readAll());

    for (const char* name : {"author", "content", "status", "note"}) {
        const QString declaration =
            QStringLiteral("required property string ") + QString::fromLatin1(name);
        check(text.contains(declaration),
              std::string("MessageBubble.") + name + " is declared `" + declaration.toStdString()
                  + "`, or a delegate is handed nothing");
    }
}

/// A layout cell that loses its height is the failure this file could not see.
///
/// Every check above measures one item: this bubble's width, that row's
/// height. A cell that has been given *no* height at all still measures fine
/// on its own -- zero is a number, and zero is what the layout handed it. What
/// is wrong is the relationship between it and its neighbour, and no
/// single-item assertion looks at that. The diagnostics panel shipped three
/// switches stacked on each other for exactly this reason, and this file was
/// green throughout.
///
/// So the probe below is the shipped switch, reproduced exactly: a RowLayout
/// whose only sized child is a plain Item wrapping the hit target, with the
/// label inside that. Stated `implicitHeight` it lays out; without it, the
/// layout has nothing to take a height from and the row collapses to nothing
/// while the label still draws. The two panels differ by that one line, which
/// is the whole claim: the audit can tell them apart.
void testCollapsedLayoutCellIsFound() {
    std::cout << "a layout cell with no height is found, and a healthy one is not reported\n";

    const QString probe = QStringLiteral(R"QML(
import QtQuick
import QtQuick.Layouts

Item {
    id: root
    width: 420
    height: 420

    // The one line that separates the two panels. Absent, a RowLayout with a
    // single plain-Item child has nothing to take an implicit height from,
    // which is precisely what shipped.
    property bool rowsHaveHeight: true

    component Switch: RowLayout {
        required property string label
        Layout.fillWidth: true
        implicitHeight: root.rowsHaveHeight ? 18 : 0

        // A hit target around the whole row, so the label is what you click.
        Item {
            Layout.fillWidth: true
            Rectangle {
                objectName: "dot"
                width: 14; height: 14
                anchors.verticalCenter: parent.verticalCenter
                color: "#4d5a68"
            }
            Text {
                objectName: "label"
                text: parent.label
                anchors.verticalCenter: parent.verticalCenter
                font.pixelSize: 11
            }
            MouseArea { anchors.fill: parent }
        }
    }

    ColumnLayout {
        objectName: "column"
        anchors.fill: parent
        spacing: 6

        Switch { objectName: "idleLoop";  label: "Idle loop" }
        Switch { objectName: "gpuPrewarm"; label: "GPU prewarm" }
        Switch { objectName: "thoughts";   label: "Show thoughts" }
    }
}
)QML");

    const QString path = QStringLiteral(KESTREL_UI_DIR "/layoutprobe.qml");
    {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            std::cout << "  FAIL cannot write the layout probe: " << file.errorString().toStdString()
                      << "\n";
            ++g_failures;
            return;
        }
        file.write(probe.toUtf8());
    }

    static QQuickWindow* window = new QQuickWindow();
    window->resize(420, 420);

    // Instantiated twice from the same file, so the two panels are the same
    // document differing only in the property under test.
    const auto problemsFor = [&](bool rowsHaveHeight) {
        QQmlEngine probe;
        probe.addImportPath(QStringLiteral(KESTREL_UI_DIR));
        QQmlComponent component(&probe);
        component.loadUrl(QUrl::fromLocalFile(path));
        if (component.isError()) {
            std::cout << "  FAIL cannot load the layout probe: "
                      << component.errorString().toStdString() << "\n";
            ++g_failures;
            return QVector<kestrel::ui::LayoutProblem>{};
        }
        std::unique_ptr<QObject> scene(component.create());
        auto* sceneItem = qobject_cast<QQuickItem*>(scene.get());
        if (sceneItem == nullptr) {
            std::cout << "  FAIL the layout probe is not an Item\n";
            ++g_failures;
            return QVector<kestrel::ui::LayoutProblem>{};
        }
        sceneItem->setProperty("rowsHaveHeight", rowsHaveHeight);
        sceneItem->setParentItem(window->contentItem());
        window->show();
        QCoreApplication::processEvents();
        sceneItem->polish();
        QCoreApplication::processEvents();
        const auto found = kestrel::ui::auditLayouts(sceneItem);
        sceneItem->setParentItem(nullptr);
        return found;
    };

    const QVector<kestrel::ui::LayoutProblem> healthy = problemsFor(true);
    const QVector<kestrel::ui::LayoutProblem> collapsed = problemsFor(false);

    if (healthy.isEmpty()) {
        std::cout << "  ok   the panel with a stated height reports nothing\n";
    } else {
        std::cout << "  FAIL the healthy panel reported " << healthy.size() << " problem(s); first is "
                  << healthy.first().where.toStdString() << ": "
                  << healthy.first().what.toStdString() << "\n";
        ++g_failures;
    }

    // Three switches, three cells, three problems. The count is the point: a
    // checker that finds one and misses the other two has not been shown to
    // work, it has been shown to work once.
    check(collapsed.size() == 3, "the collapsed panel reports all three switches (reported "
                                     + std::to_string(collapsed.size()) + ")");
    for (const auto& problem : collapsed) {
        std::cout << "       " << problem.where.toStdString() << ": " << problem.what.toStdString()
                  << "\n";
    }
    check(!collapsed.isEmpty() && collapsed.first().what.contains(QStringLiteral("cell is")),
          "the report names a cell, not something else in the tree");
}

}  // namespace

/// Runs the QML geometry tests; returns nonzero if any check fails.
int main(int argc, char** argv) {
    // Unbuffered, so a crash still shows how far the run got.
    std::cout << std::unitbuf;

    QGuiApplication app(argc, argv);
    QQmlEngine engine;
    // The component under test is a loose file in src/ui rather than a
    // compiled module, so the engine gets the directory for any relative
    // import it might grow later.
    engine.addImportPath(QStringLiteral(KESTREL_UI_DIR));

    std::cout << "message bubble geometry\n";
    testShortMessageIsVisible(engine);
    testLongMessageWrapsAndGrows(engine);
    testNarrowWindowStillWorks(engine);
    testEmptyMessageReservesSpace(engine);
    testStatusNoteFitsInsideTheBubble(engine);
    testUnbreakableTextIsNotClipped(engine);
    testListViewGivesRowsRealHeight(engine);
    testMessagePropertiesAreRequired();
    testCollapsedLayoutCellIsFound();

    if (g_failures == 0) {
        std::cout << "qml geometry tests passed\n";
        return 0;
    }
    std::cout << "qml geometry tests FAILED: " << g_failures << "\n";
    return 1;
}
