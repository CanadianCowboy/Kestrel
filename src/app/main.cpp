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
int runSmokeTest(kestrel::app::AppController& controller, int timeoutMs, bool modelRequested) {
    // A GGUF now loads on a worker thread so the window stays responsive while
    // a multi-hundred-megabyte file is read off disk. That makes "is a model
    // loaded?" a question whose answer arrives later, and asking it the moment
    // the scene appears is asking too early: the answer is always "not yet".
    // Wait for the load to finish first, but only when a model was actually
    // requested -- otherwise the no-model case would sit out the whole timeout
    // before reporting the thing it already knew, which is the opposite of a
    // fast negative control.
    if (modelRequested) {
        // Whether the load actually finished is the whole point of the wait.
        // QEventLoop::quit is also reachable from the timeout, so a loop that
        // returns tells you nothing on its own: with only the runtimeAvailable()
        // check below, a model that failed to load fell through to the mock
        // backend -- which reports available -- and the smoke test passed
        // against a model that was never loaded. It has to be named here.
        bool loadFinished = false;
        QEventLoop loading;
        QObject::connect(&controller, &kestrel::app::AppController::modelLoadFinished, &loading,
                         [&] {
                             loadFinished = true;
                             loading.quit();
                         });
        QTimer::singleShot(timeoutMs, &loading, &QEventLoop::quit);
        loading.exec();
        if (!loadFinished) {
            QTextStream(stdout) << "  FAIL    the model did not finish loading within " << timeoutMs
                                << " ms\n";
            return 1;
        }
        if (!controller.modelError().isEmpty()) {
            QTextStream(stdout) << "  FAIL    the model failed to load: " << controller.modelError()
                                << "\n";
            return 1;
        }
    }
    QTextStream out(stdout);
    out << "Kestrel smoke test\n";
    out << "  backend : " << controller.backendName() << "\n";
    out << "  model   : " << controller.modelName() << "\n";
    out.flush();

    // A real model, not merely an available runtime. The mock backend reports
    // itself available and answers instantly, so checking runtimeAvailable()
    // here let the whole check pass against a canned reply -- which is the one
    // outcome this function exists to rule out. modelPath() is empty until a
    // GGUF is actually loaded, so it is the question worth asking.
    if (controller.modelPath().isEmpty()) {
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

    const QString modelArgument = valueAfter(arguments, QStringLiteral("--model"));
    const bool reportRuntime = arguments.contains(QStringLiteral("--print-runtime"));
    if (reportRuntime && modelArgument.isEmpty()) {
        return printRuntime(controller);
    }
    if (!modelArgument.isEmpty()) {
        if (reportRuntime) {
            QObject::connect(&controller, &kestrel::app::AppController::modelLoadFinished,
                             &app, [&] { app.exit(printRuntime(controller)); },
                             Qt::QueuedConnection);
        }
        // The controller loads on a worker and publishes the result on the UI
        // thread. Start once the event loop can receive that completion.
        QTimer::singleShot(0, &controller, [&controller, modelArgument] {
            controller.loadModelFromUrl(QUrl::fromLocalFile(modelArgument).toString());
        });
    }
    if (reportRuntime) {
        return app.exec();
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
    // QQmlApplicationEngine has no errors() accessor; it reports them through
    // this signal, so the connection has to exist before the scene is loaded.
    // Without it a failure prints one bare line and the reason -- an
    // unresolvable type, a misspelled property, a missing import -- is lost,
    // which turns a one-line diagnosis into an afternoon.
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, &app,
                     [](const QList<QQmlError>& errors) {
                         for (const QQmlError& error : errors) {
                             QTextStream(stderr) << "Kestrel QML warning: " << error.toString() << "\n";
                         }
                     });

    engine.loadFromModule(QStringLiteral("Kestrel"), QStringLiteral("Main"));

    if (engine.rootObjects().isEmpty()) {
        QTextStream(stderr) << "Kestrel failed to load its QML scene.\n";
        return 1;
    }

    // Deliberately after the scene loads: the point is to exercise the window,
    // not just the controller. The timeout is generous because a real model's
    // first token can take a while on a cold context.
    if (arguments.contains(QStringLiteral("--smoke-test"))) {
        return runSmokeTest(controller, 120000, !modelArgument.isEmpty());
    }

    // Development aid: KESTREL_SCREENSHOT=<path.png> captures the composed
    // window and exits. Works together with QT_QPA_PLATFORM=offscreen for
    // display-less UI review.
    //
    // KESTREL_SCREENSHOT_DELAY_MS sets how long to wait first. The default is
    // long enough for the window to appear and too short for anything to have
    // happened in it, so capturing the app doing its actual job -- a
    // conversation on screen, with bubbles sized and wrapped -- needs to wait
    // for the model rather than guess. Default unchanged.
    const QString screenshotPath = qEnvironmentVariable("KESTREL_SCREENSHOT");
    if (!screenshotPath.isEmpty()) {
        // qEnvironmentVariableIntValue's second parameter is a bool* for
        // "was it set", not a default value, so the fallback is spelled out.
        bool delayGiven = false;
        const int requested = qEnvironmentVariableIntValue("KESTREL_SCREENSHOT_DELAY_MS", &delayGiven);
        const int delayMs = delayGiven ? requested : 1600;
        QTimer::singleShot(delayMs, &app, [&engine, &app, screenshotPath] {
            if (auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().value(0))) {
                if (!window->grabWindow().save(screenshotPath)) {
                    QTextStream(stderr) << "Kestrel could not write " << screenshotPath << "\n";
                }
            }
            app.quit();
        });
    }

    return app.exec();
}
