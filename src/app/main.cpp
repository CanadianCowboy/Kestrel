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
#include "runtime/backendregistry.h"

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

// The model to load when nobody named one on the command line.
//
// A model used to be reachable only through --model, so an ordinary launch --
// double-clicked from Explorer, started from the desktop shortcut -- had no
// model at all. Every real backend then reported itself unavailable and the
// registry handed the conversation to the mock, which is why the app talked in
// canned lines by default and why a real model had to be passed on the command
// line to see one working. A launcher with no way to reach the point of the
// product is the defect, not the missing flag.
//
// Two sources, in order:
//
//   1. KESTREL_MODEL, for a model kept anywhere on the machine. This is what
//      a developer sets, because build trees and model stores are separate.
//   2. A `models` directory beside the executable. This is what the packaged
//      app uses, and it is why the packager ships that directory: dropping a
//      model in it is the whole installation step.
//
// Two layouts are recognised, because Kestrel has two real backends reading two
// model formats:
//
//   * a .gguf file, for llama.cpp;
//   * a *directory* holding genai_config.json, for ONNX Runtime GenAI. A GenAI
//     model is a config plus one or more ONNX graphs plus external data, so it
//     is a folder rather than a file, and requiring a .gguf here would have
//     made the ONNX backend unreachable from a plain launch -- the same class of
//     defect as the one this function was written to fix.
//
// Among candidates the largest is tried first. A quantisation of the same family
// differs in size by a wide margin, and among different families the larger
// model is the more capable one -- picking alphabetically would hand a 0.5 B
// model chosen for the name starting with 'a' over the 8 B one sitting next to
// it. Kestrel shipped a `qwen.gguf` next to a much better local model precisely
// because the name said nothing about the contents.
//
// The rest come back too, rather than only the best, because a ranking is a
// guess and the caller is what settles it.
QStringList discoverModels(const QString& explicitPath) {
    if (!explicitPath.isEmpty()) {
        return {explicitPath};
    }

    const QString fromEnvironment =
        qEnvironmentVariable("KESTREL_MODEL", QString());
    if (!fromEnvironment.isEmpty() && QFileInfo::exists(fromEnvironment)) {
        return {fromEnvironment};
    }

    const QString modelDirectory =
        QCoreApplication::applicationDirPath() + QStringLiteral("/models");
    QDir directory(modelDirectory);
    if (!directory.exists()) {
        return {};
    }

    // (size, path), largest first. Kept as a ranked list rather than a single
    // winner because "largest" is a guess about which model will load, and
    // guesses about model files are wrong: a folder can hold gigabytes of
    // weights behind a config that is empty or truncated, and a .gguf can be a
    // half-finished download. Ranking says who to try; the caller loading each
    // in turn and moving on is what actually decides.
    QList<QPair<qint64, QString>> ranked;
    const auto consider = [&ranked](const QString& path, qint64 bytes) {
        ranked.append({bytes, path});
    };

    for (const QString& entry :
         directory.entryList({QStringLiteral("*.gguf")}, QDir::Files, QDir::Name)) {
        const QFileInfo info(directory.filePath(entry));
        if (info.isFile() && info.isReadable() && info.size() > 0) {
            consider(info.absoluteFilePath(), info.size());
        }
    }
    for (const QString& entry :
         directory.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        // A GenAI model is identified by its config, not by its name, so the
        // presence of that one file is what makes a directory a candidate.
        const QDir candidate(directory.filePath(entry));
        if (!candidate.exists(QStringLiteral("genai_config.json"))) {
            continue;
        }
        const QFileInfo config(
            candidate.filePath(QStringLiteral("genai_config.json")));
        if (!config.isReadable() || config.size() == 0) {
            // An empty or unreadable config is the specific case that used to
            // win the ranking on the strength of the weights beside it and then
            // fail to load. There is no graph to run, so it is not a model
            // however large the folder is.
            continue;
        }
        if (candidate.entryList({QStringLiteral("*.onnx")}, QDir::Files).isEmpty()) {
            continue;
        }
        qint64 bytes = 0;
        const QFileInfoList files =
            candidate.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
        for (const QFileInfo& file : files) {
            bytes += file.size();
        }
        consider(candidate.absolutePath(), bytes);
    }

    // Ties broken by path, so the same directory always yields the same order.
    std::sort(ranked.begin(), ranked.end(),
              [](const QPair<qint64, QString>& left,
                 const QPair<qint64, QString>& right) {
                  if (left.first != right.first) {
                      return left.first > right.first;
                  }
                  return left.second < right.second;
              });
    QStringList ordered;
    ordered.reserve(ranked.size());
    for (const QPair<qint64, QString>& entry : std::as_const(ranked)) {
        ordered.append(entry.second);
    }
    return ordered;
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

    kestrel::app::AppController controller;

    const QStringList arguments = QGuiApplication::arguments();

    const QStringList modelCandidates =
        discoverModels(valueAfter(arguments, QStringLiteral("--model")));
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
        // Each candidate in turn, and the next one when the last fails to load.
        //
        // Ranking by size picks the model most likely to be the one wanted; it
        // cannot know whether the largest file on disk is a complete download
        // or a folder whose config names a graph that is not there. Trying the
        // rest costs one failed load and is the difference between starting on
        // a real model and starting on the preview mock with an error nobody
        // asked for. An explicit --model is a single candidate, so the loop
        // never second-guesses a choice the user made.
        //
        // The controller drops its load thread before publishing
        // modelLoadFinished, so the next attempt is not refused as a load
        // already in flight.
        auto attempt = std::make_shared<int>(0);
        QObject::connect(&controller, &kestrel::app::AppController::modelLoadFinished,
                         &app, [&controller, modelCandidates, attempt] {
            if (controller.modelError().isEmpty()) {
                return; // loaded; the loop is finished
            }
            if (*attempt >= modelCandidates.size()) {
                return; // every candidate refused; the last error stands
            }
            const QString next = modelCandidates.at(*attempt);
            ++(*attempt);
            controller.loadModelFromUrl(QUrl::fromLocalFile(next).toString());
        }, Qt::QueuedConnection);
        // The controller loads on a worker and publishes the result on the UI
        // thread. Start once the event loop can receive that completion.
        const QString first = modelCandidates.first();
        QTimer::singleShot(0, &controller, [&controller, first] {
            controller.loadModelFromUrl(QUrl::fromLocalFile(first).toString());
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

    return app.exec();
}
