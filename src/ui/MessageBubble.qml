import QtQuick

/// One chat bubble: the message body, plus a note when the response ended in a
/// state worth reporting.
///
/// This lives in its own file rather than inline in the ListView delegate for
/// one reason: it can be loaded and measured on its own. The sizing here is
/// entirely self-referential -- the bubble is as wide as the text, the text is
/// as wide as the bubble -- which is exactly the shape of code that looks
/// correct and renders as a four-pixel sliver when a binding resolves in an
/// unexpected order. A test that instantiates this file and asserts real
/// numbers catches that; a test that only checks a reply arrived does not.
///
/// Sizing is strictly one-way, with no measurement anywhere in it:
///
///     root.width      ->  bubble.width
///     bubble.width    ->  bodyText.width
///     bodyText.width  ->  bodyText.implicitHeight
///     bodyText height ->  bodyColumn.height  ->  bubble.height
///     bubble.height   ->  implicitHeight
///
/// Every step reads only from the step before it, so each has exactly one
/// value rather than whichever order the bindings happened to be evaluated in.
///
/// The bubble takes the full width available to it and lets the text wrap,
/// rather than hugging short messages. Hugging needs the unwrapped width of the
/// text, and the only way to get that is to ask a wrapping Text for its own
/// implicitWidth -- which depends on the width it was given, and that width
/// comes from the bubble being sized by that same implicitWidth. That is the
/// loop this component used to have, and it is what collapsed the chat. A
/// consistent bubble width is a fair trade for a chat that is legible.
Item {
    id: root

    /// "user" or "assistant", as spelled by MessageModel::AuthorRole.
    ///
    /// These are `required`, and that is the only reason a message ever reaches
    /// the screen. A ListView delegate is given the model's roles through its
    /// *required* properties and nothing else: a plain `property string author`
    /// is initialised to its default and never sees the role at all. The bubble
    /// was extracted out of the delegate with plain properties on the belief
    /// that same-named properties are filled "either way", and every message
    /// rendered as an empty string -- the chat went invisible again, for the
    /// second time, from a cause that reads like a layout problem.
    ///
    /// The geometry test did not catch it because it sets these properties
    /// itself, so it measured a bubble that the app never produces.
    required property string author
    /// The message body, still growing while the response streams in.
    required property string content
    /// "streaming", "complete", "stopped", or "failed".
    required property string status
    /// Detail for a failed or stopped response, usually the reason.
    required property string note

    readonly property bool fromAssistant: author === "assistant"
    /// Bubbles cap at 78% of the row so the side they sit on stays readable.
    readonly property real maxBubbleWidth: root.width * 0.78

    /// The height a Layout will give this item.
    implicitHeight: bubble.height + 4

    /// The height a ListView will give this item, which is a different number.
    ///
    /// A ListView lays its rows out at the delegate's `height` and never looks
    /// at implicitHeight -- that is a Layout's mechanism, not a view's. A plain
    /// Item's height is 0 until something sets it, so a component that only
    /// published an implicitHeight produced zero-tall rows and an invisible
    /// conversation. Binding height here means every caller gets a correct row
    /// without needing to know that, and the two can never disagree.
    height: implicitHeight

    QtObject {
        id: palette
        readonly property color ink: "#e6e9ef"
        readonly property color line: "#2a2f38"
    }

    Rectangle {
        id: bubble
        objectName: "bubble"
        width: root.maxBubbleWidth
        // 15px of inset on each side is 30, so the bubble adds 30 for the text
        // to fit inside it rather than overrun the bottom edge.
        height: bodyColumn.height + 30
        radius: 15
        // Placed with x rather than anchors. Choosing a side with a ternary
        // that assigns `undefined` to the unused anchor -- the obvious way to
        // write this -- costs the item its width binding, and the bubble
        // renders zero-wide for half the conversation. The width is always the
        // cap, so the position is just arithmetic and needs no anchoring.
        x: root.fromAssistant ? 0 : root.width - root.maxBubbleWidth
        color: root.fromAssistant ? "#15181d" : "#243b34"
        border.color: root.fromAssistant ? palette.line : "#385b4e"

        Column {
            id: bodyColumn
            // Three anchors, not fill. A fill would make this Column's height
            // depend on the bubble, while the bubble's height is derived from
            // this Column's height.
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 15
            height: implicitHeight
            spacing: 6

            Text {
                id: bodyText
                objectName: "bodyText"
                // The Column's width, which the bubble set from the row width.
                // Unambiguous, so the implicitHeight below has a single value.
                width: parent.width
                text: root.content
                color: palette.ink
                font.pixelSize: 14
                lineHeight: 1.35
                wrapMode: Text.Wrap
            }

            // Terminal states stay visible rather than being discarded, so a
            // stopped or failed response is still readable and recoverable.
            Text {
                objectName: "statusText"
                width: parent.width
                visible: root.status === "stopped" || root.status === "failed"
                text: root.status === "failed"
                      ? "Generation failed" + (root.note ? ": " + root.note : "")
                      : "Stopped"
                color: "#8a6a74"
                font.pixelSize: 11
                wrapMode: Text.Wrap
            }
        }
    }
}
