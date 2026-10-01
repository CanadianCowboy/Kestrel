#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QEventLoop>
#include <QImage>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTextStream>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <utility>

#include "app/appcontroller.h"
#include "core/pathtext.h"
#include "runtime/backendregistry.h"
#include "runtime/modeldiscovery.h"
#include "ui/layoutaudit.h"

#ifdef _WIN32
#  include <windows.h>
#endif

namespace {

#ifdef _WIN32
// Points the C streams at the console that launched us, if they have nowhere
// to go already.
//
// The executable is built for the GUI subsystem, which is the whole reason a
// user double-clicking it does not get a black terminal window behind the app.
// The cost of that is that a process with no console of its own can have an
// invalid stdout, and --print-runtime is only worth having if its output can be
// read.
//
// The existing handle is checked first and left alone when it is good, which is
// the case that matters most: a launch that redirects stdout to a pipe or a
// file already works, and re-opening it onto the console tears that redirect
// loose and prints the report somewhere nobody is reading. Attaching rather
// than allocating a console keeps the same property -- a redirected launch has
// no parent console to attach to, and adding one would only give the report
// somewhere to go that it was not going before.
void attachToLaunchConsole() {
    // Each stream is checked on its own. Treating them as one is wrong in a way
    // that is easy to hit: a launcher that captures stderr to a log file and
    // leaves stdout alone gives a valid stderr and an invalid stdout, and a
    // single early return on stdout would leave the report going nowhere while
    // a single freopen pair would tear the log redirect loose and send every
    // later error to the console instead. Both streams are therefore reopened
    // only when that stream is the broken one.
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    const bool stdoutBroken = out == nullptr || out == INVALID_HANDLE_VALUE;
    const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    const bool stderrBroken = err == nullptr || err == INVALID_HANDLE_VALUE;

    if (!stdoutBroken && !stderrBroken) {
        return;
    }
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        return;
    }
    // A failure here means the console is attached but that stream was already
    // usable after all, which is the normal case for a redirected launch, so
    // there is nothing to report and nothing to do about it.
    if (stdoutBroken) {
        static_cast<void>(freopen("CONOUT$", "w", stdout));
    }
    if (stderrBroken) {
        static_cast<void>(freopen("CONOUT$", "w", stderr));
    }
}
#else
void attachToLaunchConsole() {}
#endif

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

// The models this launch will try, in order, as URLs the controller can load.
//
// The ranking and the rules about what counts as a candidate live in
// runtime::discoverModels, beside backendregistry and for the same reason:
// they are decisions about the filesystem rather than about how this program
// starts, and a rule that can only be reached from a GUI entry point cannot be
// tested. What is left here is the part that is genuinely this file's -- two
// sources, in order.
//
//   1. KESTREL_MODEL, for a model kept anywhere on the machine. This is what a
//      developer sets, because build trees and model stores are separate.
//   2. A "models" directory beside the executable. This is what the packaged
//      app uses, and it is why the packager ships that directory: dropping a
//      model in it is the whole installation step.
//
// An explicit --model or KESTREL_MODEL is a single candidate and is never
// second-guessed. A choice the user made is not a guess to be improved on.
QStringList modelCandidatesFor(const QString& explicitPath) {
    if (!explicitPath.isEmpty()) {
        return {QUrl::fromLocalFile(QFileInfo(explicitPath).absoluteFilePath()).toString()};
    }

    const QString fromEnvironment = qEnvironmentVariable("KESTREL_MODEL", QString());
    if (!fromEnvironment.isEmpty()) {
        return {QUrl::fromLocalFile(QFileInfo(fromEnvironment).absoluteFilePath()).toString()};
    }

    const QString modelDirectory =
        QCoreApplication::applicationDirPath() + QStringLiteral("/models");
    QStringList urls;
    for (const kestrel::runtime::ModelCandidate& candidate :
         kestrel::runtime::discoverModels(kestrel::core::pathFromUtf8(modelDirectory.toStdString()))) {
        urls.append(QUrl::fromLocalFile(QString::fromStdString(candidate.path)).toString());
    }
    return urls;
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
    // Voice and dictation are optional the same way the compute backends are,
    // so they are reported for the same reason: the answer must be readable
    // without launching the window and watching the panel.
    out << "  voice        : " << (controller.ttsAvailable() ? "available" : "unavailable")
        << " (" << controller.ttsVoice() << ")\n";
    out << "  dictation    : " << (controller.sttAvailable() ? "available" : "unavailable")
        << " (" << controller.sttDetail() << ")\n";
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

#ifdef KESTREL_DESKTOP_FILE_ID
    // A freedesktop desktop environment has to be told which .desktop file
    // describes this window. Without it the taskbar entry falls back to the
    // executable name, the window carries a generic icon, and the window menu
    // has no application name to offer "Quit" and "About" under.
    //
    // fromLatin1, not QStringLiteral: QStringLiteral token-pastes its argument
    // onto u"", so it cannot be handed a macro that expands to a string literal.
    QGuiApplication::setDesktopFileName(QString::fromLatin1(KESTREL_DESKTOP_FILE_ID));
#endif

    kestrel::app::AppController controller;

    const QStringList arguments = QGuiApplication::arguments();

    const QStringList modelCandidates =
        modelCandidatesFor(valueAfter(arguments, QStringLiteral("--model")));
    const bool reportRuntime = arguments.contains(QStringLiteral("--print-runtime"));
    if (reportRuntime && modelCandidates.isEmpty()) {
        attachToLaunchConsole();
        return printRuntime(controller);
    }
    if (!modelCandidates.isEmpty()) {
        if (reportRuntime) {
            QObject::connect(&controller, &kestrel::app::AppController::modelLoadFinished,
                             &app, [&] { app.exit(printRuntime(controller)); },
                             Qt::QueuedConnection);
        }
        // One call, and the controller walks the list itself. That used to be
        // a retry loop wired to modelLoadFinished from here, and it never ran:
        // queued connections are delivered in the order they were made, so the
        // --print-runtime handler above -- already connected, and already
        // calling app.exit -- was still ahead of the retry by the time the first
        // candidate failed. The fallback shipped without ever having been
        // observed to work.
        //
        // The controller publishes modelLoadFinished once, when the sequence is
        // over, so this file has nothing to get wrong about ordering.
        QTimer::singleShot(0, &controller, [&controller, modelCandidates] {
            controller.loadModelFromUrls(modelCandidates);
        });
    }
    if (reportRuntime) {
        attachToLaunchConsole();
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
        // No window exists, so this is the only place a reason can be given.
        // It goes to the launching console if there is one -- a shell launch
        // gets the full QML error -- and is otherwise lost, which is the same
        // as every other silent start-up failure the platform hides. Putting a
        // dialog here instead would mean linking QtWidgets, a whole module and
        // its runtime, for a path that only runs on a broken build.
        attachToLaunchConsole();
        QTextStream(stderr) << "Kestrel failed to load its QML scene.\n"
                            << "Run with QT_LOGGING_RULES='qt.qml.*=true' for the parse errors.\n";
        return 1;
    }

    // Deliberately after the scene loads: the point is to exercise the window,
    // not just the controller. The timeout is generous because a real model's
    // first token can take a while on a cold context.
    if (arguments.contains(QStringLiteral("--smoke-test"))) {
        return runSmokeTest(controller, 120000, !modelCandidates.isEmpty());
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
    //
    // The capture goes through the root item rather than QQuickWindow's own
    // grabWindow(). grabWindow() renders through the window's surface, and the
    // offscreen platform has no swap chain to render into, so it does not
    // return there. An item grab goes through the scene graph's own image path,
    // which works on every platform, display-less included.
    //
    // The quit is armed rather than immediate because an item grab is
    // asynchronous, and the deadline is the second failure this mode had: a
    // capture that never arrives must end in a process that exits rather than
    // one that sits there holding a model load open. Qt's own way of failing
    // here is not an exit at all -- with no platform plugin it puts up a modal
    // dialog -- so the packaging step copies the offscreen plugin and says so
    // if it cannot.
    //
    // Every one of those exits is nonzero, because the exit status is the only
    // thing the packaging check reads. A capture that quietly produced no file
    // and reported success is worse than one that failed: the check goes on to
    // measure the missing file, or, if the file was left from a previous run,
    // to pass on yesterday's picture.
    const QString screenshotPath = qEnvironmentVariable("KESTREL_SCREENSHOT");
    if (!screenshotPath.isEmpty()) {
        const auto fail = [&app](const char* what) {
            QTextStream(stderr) << "Kestrel could not " << what << "\n";
            app.exit(1);
        };
        // qEnvironmentVariableIntValue's second parameter is a bool* for
        // "was it set", not a default value, so the fallback is spelled out.
        bool delayGiven = false;
        const int requested = qEnvironmentVariableIntValue("KESTREL_SCREENSHOT_DELAY_MS", &delayGiven);
        const int delayMs = delayGiven ? requested : 1600;
        auto* watchdog = new QTimer(&app);
        watchdog->setSingleShot(true);
        QObject::connect(watchdog, &QTimer::timeout, &app,
                         [&app] { app.exit(1); }); // the grab never arrived
        watchdog->start(delayMs + 15000);
        QTimer::singleShot(delayMs, &app, [&engine, &app, screenshotPath, fail] {
            auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().value(0));
            if (window == nullptr || window->contentItem() == nullptr) {
                fail("find a window to capture");
                return;
            }
            const QSharedPointer<QQuickItemGrabResult> grab = window->contentItem()->grabToImage();
            // grabToImage returns null when the grab cannot even be started,
            // and then there is no ready signal to connect to and nothing else
            // that would ever end this process.
            if (grab.isNull()) {
                fail("start a capture of the window");
                return;
            }
            QObject::connect(grab.data(), &QQuickItemGrabResult::ready, &app,
                             [grab, screenshotPath, &app] {
                                 const QImage image = grab->image();
                                 if (image.isNull() || !image.save(screenshotPath)) {
                                     QTextStream(stderr) << "Kestrel could not write "
                                                         << screenshotPath << "\n";
                                     app.exit(1);
                                     return;
                                 }
                                 app.quit();
                             });
        });
    }

    // Development aid: KESTREL_LAYOUT_CHECK=1 asks the scene graph where every
    // layout put its cells, prints anything wrong, and exits. KESTREL_LAYOUT_CHECK_DELAY_MS
    // sets how long to wait first, and it has to be a delay rather than a
    // connection: the panels that are the point of this are built lazily, and a
    // layout that has not run yet has no geometry to be wrong about.
    //
    // The diagnostics panel is opened first, because the audit only judges what
    // the user can see and that panel is closed by default. Judging it while
    // closed would pass on the exact defect this exists to catch, so the panel
    // is opened the way a user opens it.
    //
    // The exit status is the answer, because a check whose result is only in
    // its output is a check the packaging step has to be trusted to read. This
    // is the counterpart to KESTREL_SCREENSHOT above, and it exists because
    // that one could not see the defect this found: a panel can be a correct
    // size, correctly laid out at the top level, and have its rows drawn on top
    // of each other, and a picture of that is a picture with more contrast in
    // it than before.
    if (qEnvironmentVariableIsSet("KESTREL_LAYOUT_CHECK")) {
        controller.setDiagnosticsOpen(true);
        bool waitGiven = false;
        const int requested = qEnvironmentVariableIntValue("KESTREL_LAYOUT_CHECK_DELAY_MS", &waitGiven);
        const int waitMs = waitGiven ? requested : 2000;
        // A deadline, because the alternative to ending is a process that sits
        // there holding a model load open with nothing watching it.
        auto* expired = new QTimer(&app);
        expired->setSingleShot(true);
        QObject::connect(expired, &QTimer::timeout, &app, [&app] { app.exit(2); });
        expired->start(waitMs + 15000);
        QTimer::singleShot(waitMs, &app, [&engine, &app] {
            auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().value(0));
            if (window == nullptr || window->contentItem() == nullptr) {
                QTextStream(stderr) << "Kestrel could not find a window to audit\n";
                app.exit(2);
                return;
            }
            const QVector<kestrel::ui::LayoutProblem> problems =
                kestrel::ui::auditLayouts(window->contentItem());
            QTextStream out(stdout);
            if (problems.isEmpty()) {
                out << "layout check: ok, no cell is collapsed or overlapping\n";
                out.flush();
                app.exit(0);
                return;
            }
            out << "layout check: " << problems.size() << " problem(s)\n";
            for (const auto& problem : problems) {
                out << "  " << problem.where << ": " << problem.what << "\n";
            }
            out.flush();
            app.exit(1);
        });
    }

    return app.exec();
}
