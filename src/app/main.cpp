#include <QGuiApplication>
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
    engine.loadFromModule(QStringLiteral("Kestrel"), QStringLiteral("Main"));

    if (engine.rootObjects().isEmpty()) {
        QTextStream(stderr) << "Kestrel failed to load its QML scene.\n";
        return 1;
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
