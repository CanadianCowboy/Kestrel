#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QTextStream>

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
    out.flush();
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Kestrel"));
    app.setOrganizationName(QStringLiteral("Kestrel"));

    // Kestrel draws its own controls: every Button in the QML overrides
    // contentItem and background. The native Windows style silently ignores
    // those overrides, which drops the intended soft-glass look and makes Qt
    // emit customization warnings. Basic is the non-native style that honours
    // them, and it has to be selected before the QML is loaded.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    kestrel::app::AppController controller;
    const QStringList arguments = QGuiApplication::arguments();
    if (arguments.contains(QStringLiteral("--print-runtime"))) {
        return printRuntime(controller);
    }

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("appController"), &controller);
    engine.loadFromModule(QStringLiteral("Kestrel"), QStringLiteral("Main"));

    if (engine.rootObjects().isEmpty()) {
        QTextStream(stderr) << "Kestrel failed to load its QML scene.\n";
        return 1;
    }
    return app.exec();
}
