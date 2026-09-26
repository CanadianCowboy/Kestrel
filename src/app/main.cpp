#include <QGuiApplication>
#include <QEventLoop>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTextStream>
#include <QTimer>
#include <QUrl>

#include "app/appcontroller.h"
#include "runtime/backendregistry.h"

namespace {

// Dumps runtime and device state, then exits without opening a window.
//
// Contributions to the runtime layer are expected to state which optional
// dependencies were available during validation. This makes that check
// mechanical instead of a matter of memory, and it exercises the same
// controller and QML bindings the window uses, so it also catches a QML load
// failure that would otherwise only appear on launch.
// The value following `flag`, or empty when the flag is absent, is last, or is
// followed by another flag. A flag with no value must not silently swallow the
// next one and report a nonsense path.
QString valueAfter(const QStringList& arguments, const QString& flag) {
    const int index = arguments.indexOf(flag);
    if (index < 0 || index + 1 >= arguments.size()) {
        return {};
    }
    const QString value = arguments.at(index + 1);
    return value.startsWith(QStringLiteral("--")) ? QString() : value;
}

// Sends one message through the real window and reports whether a reply came
// back, then exits. This is the only check that covers the whole path at once:
// the QML scene loads, a message reaches the backend on its worker thread, and
// the streamed tokens land in the model the UI renders from. Every unit test
// below that layer can be green while the app as a whole is broken -- and one
// was, for exactly this reason.
//
// Runs only when asked, and only with a real model loaded, because a smoke test
// that passes against the mock proves nothing about the runtime.
int runSmokeTest(kestrel::app::AppController& controller, int timeoutMs) {
    QTextStream out(stdout);
    out << "Kestrel smoke test\n";
    out << "  backend : " << controller.backendName() << "\n";
    out << "  model   : " << controller.modelName() << "\n";
    out.flush();

    if (!controller.runtimeAvailable()) {
        out << "  FAIL    no model is loaded, so there is nothing to exercise.\n"
               "          Pass --model <path> to run this against a real model.\n";
        out.flush();
        return 2;
    }

    const QString prompt = QStringLiteral(
        "In one short sentence, state what you are and that you are working.");
    controller.sendMessage(prompt);

    // Wait on the controller's own signal rather than sleeping, so this ends as
    // soon as the reply is done. The timeout is a backstop: a backend that
    // never finishes must fail the check rather than hang it.
    QEventLoop loop;
    QObject::connect(&controller, &kestrel::app::AppController::metricsChanged, &loop, [&] {
        if (!controller.generating()) {
            loop.quit();
        }
    });
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec();

    if (controller.generating()) {
        out << "  FAIL    no reply within " << timeoutMs << " ms\n";
        out.flush();
        return 1;
    }

    kestrel::app::MessageModel* messages = controller.messages();
    if (messages == nullptr || messages->rowCount() < 2) {
        out << "  FAIL    the transcript does not contain a reply\n";
        out.flush();
        return 1;
    }

    const QString reply =
        messages->data(messages->index(1, 0), kestrel::app::MessageModel::ContentRole)
            .toString();
    const QString status =
        messages->data(messages->index(1, 0), kestrel::app::MessageModel::StatusRole).toString();
    const QString note =
        messages->data(messages->index(1, 0), kestrel::app::MessageModel::NoteRole).toString();

    out << "  tokens  : " << controller.tokensGenerated() << "\n";
    out << "  tok/s   : " << QString::number(controller.tokensPerSecond(), 'f', 1) << "\n";
    out << "  context : " << controller.contextSummary() << "\n";
    out << "  kv cache: " << controller.kvCacheSummary() << "\n";
    out << "  status  : " << status << "\n";
    out << "  reply   : " << reply.left(160) << "\n";
    out.flush();

    if (reply.trimmed().isEmpty()) {
        out << "  FAIL    the reply is empty\n";
        out.flush();
        return 1;
    }
    // An empty reply still labelled streaming is what a hung app looks like, so
    // the status column is checked as carefully as the text.
    if (status != kestrel::app::toStatusString(kestrel::app::MessageStatus::Complete)) {
        out << "  FAIL    the reply is not complete (status=" << status
            << (note.isEmpty() ? QString() : ", note=" + note) << ")\n";
        out.flush();
        return 1;
    }
    if (controller.tokensGenerated() <= 0) {
        out << "  FAIL    no tokens were counted, so throughput is unverified\n";
        out.flush();
        return 1;
    }

    out << "  OK      a reply reached the UI through the real backend\n";
    out.flush();
    return 0;
}

int printRuntime(const kestrel::app::AppController& controller) {
    QTextStream out(stdout);
    out << "Kestrel runtime report\n";
    out << "  backend      : " << controller.backendName() << "\n";
    out << "  model        : " << controller.modelName() << "\n";
    out << "  detail       : " << controller.runtimeDetail() << "\n";
    out << "  gpu available: " << (controller.gpuAvailable() ? "yes" : "no") << "\n";
    out << "  gpu          : " << controller.gpuName() << "\n";
    out << "  gpu summary  : " << controller.gpuSummary() << "\n";
    out << "  capability   : " << controller.computeCapability() << "\n";
    out << "  device count : " << controller.gpuDeviceCount() << "\n";
    out << "  diagnostics  :\n";
    for (const QVariant& row : controller.runtimeDiagnostics()) {
        const QVariantMap entry = row.toMap();
        out << (entry.value(QStringLiteral("ok")).toBool() ? "    [ ok ] " : "    [warn] ")
            << entry.value(QStringLiteral("label")).toString() << ": "
            << entry.value(QStringLiteral("value")).toString() << "\n";
    }
    if (!controller.modelPath().isEmpty()) {
        out << "  model path   : " << controller.modelPath() << "\n";
    }
    if (!controller.modelError().isEmpty()) {
        out << "  model error  : " << controller.modelError() << "\n";
    }
    out.flush();
    // A model that was asked for and did not load is a failure, not a report.
    // This is what lets the load path be checked without opening a window.
    return controller.modelError().isEmpty() ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Kestrel"));
    app.setOrganizationName(QStringLiteral("Kestrel"));
    app.setApplicationVersion(QStringLiteral("0.1.0"));

    // Kestrel draws its own controls: every Button in the QML overrides
    // contentItem and background. The native Windows style silently ignores
    // those overrides, which drops the intended look and makes Qt emit
    // customization warnings. Basic is the non-native style that honours
    // them, and it must be selected before the QML is loaded.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    kestrel::app::AppController controller;

    const QStringList arguments = QGuiApplication::arguments();

    // Load a model named on the command line before the window opens, so the
    // first frame already shows the real runtime instead of flashing "no model"
    // and then loading. It also makes the load path reachable without driving a
    // native file dialog, which is what a scripted check has to do.
    const QString modelArgument = valueAfter(arguments, QStringLiteral("--model"));
    if (!modelArgument.isEmpty()) {
        controller.loadModelFromUrl(QUrl::fromLocalFile(modelArgument).toString());
    }

    if (arguments.contains(QStringLiteral("--print-runtime"))) {
        return printRuntime(controller);
    }

    // Development aid: KESTREL_DEMO=static seeds reviewable conversations,
    // KESTREL_DEMO=live additionally starts a streaming response, so the UI
    // can be exercised and captured without manual interaction.
    const QString demoMode = qEnvironmentVariable("KESTREL_DEMO");
    if (!demoMode.isEmpty()) {
        controller.seedDemoContent(demoMode == QLatin1String("live"));
    }

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("appController"), &controller);
    engine.loadFromModule(QStringLiteral("Kestrel"), QStringLiteral("Main"));

    if (engine.rootObjects().isEmpty()) {
        QTextStream(stderr) << "Kestrel failed to load its QML scene.\n";
        return 1;
    }

    // Deliberately after the scene loads: the point is to exercise the window,
    // not just the controller. The timeout is generous because a real model's
    // first token can take a while on a cold context.
    if (arguments.contains(QStringLiteral("--smoke-test"))) {
        return runSmokeTest(controller, 120000);
    }

    // Development aid: KESTREL_SCREENSHOT=<path.png> captures the composed
    // window shortly after startup and exits. Works together with
    // QT_QPA_PLATFORM=offscreen for display-less UI review.
    const QString screenshotPath = qEnvironmentVariable("KESTREL_SCREENSHOT");
    if (!screenshotPath.isEmpty()) {
        QTimer::singleShot(1600, &app, [&engine, &app, screenshotPath] {
            if (auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().value(0))) {
                window->grabWindow().save(screenshotPath);
            }
            app.quit();
        });
    }

    return app.exec();
}
