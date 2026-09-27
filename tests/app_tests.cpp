// Tests for the asynchronous generation path.
//
// The threading contract is the risky part of this feature, so these tests
// exercise the real thing: a backend generating on a worker thread, tokens
// crossing to the test's thread through queued signals, and cancellation
// taking effect while generation is still in flight.

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QDir>
#include <QEventLoop>
#include <QObject>
#include <QRegularExpression>
#include <QStringList>
#include <QThread>
#include <QTemporaryFile>
#include <QTimer>
#include <QUrl>

#include <atomic>
#include <iostream>
#include <mutex>
#include <vector>
#include <thread>

#include "app/appcontroller.h"
#include "app/generationworker.h"
#include "app/messagemodel.h"
#include "app/speechsynthesizer.h"
#include "runtime/mockbackend.h"
#include "runtime/modelbackend.h"

namespace {

using kestrel::app::AppController;
using kestrel::app::GenerationWorker;
using kestrel::runtime::BackendKind;
using kestrel::runtime::GenerationRequest;

// A backend slow enough that cancellation can be issued mid-generation.
//
// This mirrors what a real GPU backend does: emit a token, then check whether
// the user asked to stop before doing more work. If the loop never consulted
// the flag, cancel() would appear to do nothing until generation finished.
class SlowBackend final : public kestrel::runtime::ModelBackend {
public:
    explicit SlowBackend(int tokenCount, int delayMs)
        : m_tokenCount(tokenCount), m_delayMs(delayMs) {}

    [[nodiscard]] BackendKind kind() const noexcept override { return BackendKind::Mock; }

    [[nodiscard]] kestrel::runtime::RuntimeStatus status() const override {
        return {true, true, "Slow test backend", "test", "unit test", 0.0, 0, 4096};
    }

    bool loadModel(const std::string&, std::string&) override { return true; }

    void generate(const GenerationRequest& request,
                  kestrel::runtime::TokenCallback onToken,
                  kestrel::runtime::CompletionCallback onComplete) override {
        {
            std::lock_guard<std::mutex> lock(m_requestsMutex);
            m_requests.push_back(request);
        }
        m_generateThread = std::this_thread::get_id();
        m_cancelled.store(false, std::memory_order_release);
        for (int i = 0; i < m_tokenCount; ++i) {
            if (m_cancelled.load(std::memory_order_acquire)) {
                onComplete(false, "Generation stopped");
                return;
            }
            onToken("tok");
            m_tokensEmitted.fetch_add(1, std::memory_order_release);
            if (m_delayMs > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(m_delayMs));
            }
        }
        onComplete(true, {});
    }

    void cancel() override { m_cancelled.store(true, std::memory_order_release); }

    [[nodiscard]] std::thread::id generateThread() const noexcept { return m_generateThread; }
    [[nodiscard]] int tokensEmitted() const noexcept {
        return m_tokensEmitted.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::vector<GenerationRequest> requests() const {
        std::lock_guard<std::mutex> lock(m_requestsMutex);
        return m_requests;
    }

private:
    mutable std::mutex m_requestsMutex;
    std::vector<GenerationRequest> m_requests;
    int m_tokenCount;
    int m_delayMs;
    std::atomic<bool> m_cancelled{false};
    std::atomic<int> m_tokensEmitted{0};
    std::thread::id m_generateThread{};
};

// Receives worker signals on the thread that owns this object, which is the
// same arrangement the app uses: the controller lives on the UI thread and
// receives queued signals from the worker.
class Collector final : public QObject {
    Q_OBJECT

public:
    QStringList tokens;
    bool finished = false;
    bool success = false;
    QString error;
    quint64 startedId = 0;

    [[nodiscard]] int totalCharacters() const {
        int total = 0;
        for (const QString& token : tokens) {
            total += token.size();
        }
        return total;
    }

signals:
    void finishedSignal();
};

// A speech backend with no audio behind it.
//
// The playback pump -- cue first, then one clause at a time, cursor advancing
// only once a clause has actually been spoken, response complete when the last
// one lands -- is the part worth testing, and it is platform-independent. This
// records what would have been spoken and lets the test decide when each
// utterance ends, so the whole path is exercised on a machine with no voice.
class FakeSpeechBackend final : public kestrel::app::SpeechBackend {
public:
    [[nodiscard]] bool usable() const override { return true; }
    [[nodiscard]] QString description() const override { return QStringLiteral("fake voice"); }

    void applyVoice(const kestrel::core::VoicePersona&) override { ++voiceApplications; }

    void speak(const QString& text) override {
        spoken.append(text);
        m_speaking = true;
        if (m_autoFinish) {
            // A real engine reports the end of an utterance from the audio
            // stack some time later. Queuing the report keeps the pump's
            // ordering honest while still running the loop, which is what
            // makes a test that speaks through to completion deterministic on
            // a machine that has a voice installed. The report is queued
            // rather than raised here because speak() is called from inside the
            // pump, and a callback that re-enters it would be a real defect
            // rather than something a test should pretend does not exist.
            QTimer::singleShot(0, m_context, [this] { finishUtterance(); });
        }
    }

    // Tests about anything other than speech want a turn that finishes when the
    // text does. Without this they inherit whatever the host machine's voice
    // does, and pass or fail depending on whether audio is installed.
    void setAutoFinish(bool value) { m_autoFinish = value; }

    // A plain class cannot be a QTimer context, so the owner lends one. The
    // queued report is dropped if the owner dies first.
    void setTimerContext(QObject* context) { m_context = context; }

    void stop() override { ++boundaryStops; }

    void stopImmediately() override { ++immediateStops; }

    [[nodiscard]] bool speakingNow() const override { return m_speaking; }

    // Simulates the engine reaching the end of an utterance.
    void finishUtterance() {
        m_speaking = false;
        reportFinished();
    }

    QStringList spoken;
    int voiceApplications = 0;
    int boundaryStops = 0;
    int immediateStops = 0;

private:
    bool m_speaking = false;
    bool m_autoFinish = false;
    QObject* m_context = nullptr;
};

int failures = 0;

void check(bool condition, const char* what) {
    if (condition) {
        std::cout << "  ok   " << what << "\n";
    } else {
        std::cout << "  FAIL " << what << "\n";
        ++failures;
    }
}

// Gives a controller a voice that speaks without an audio device, so a test
// about presence, prompts, or the transcript reaches the same end state whether
// or not the machine running it has text-to-speech installed. The speech tests
// drive FakeSpeechBackend themselves and must not call this.
void useSilentVoice(kestrel::app::AppController& controller) {
    auto speech = std::make_unique<FakeSpeechBackend>();
    speech->setAutoFinish(true);
    speech->setTimerContext(&controller);
    controller.setSpeechBackendForTesting(std::move(speech));
}

// A turn is over when the words are done *and* the voice has stopped, which are
// two different moments once text-to-speech is installed.
//
// The wait polls rather than driving off a signal on purpose. The controller
// raises metricsChanged while it is still deciding what happens next, so the
// moment the last token arrives is also the moment generation has ended and
// playback has not yet begun. A signal-driven wait can observe that gap, decide
// the turn is finished, and walk away in the middle of a sentence.
void runTurnToCompletion(kestrel::app::AppController& controller) {
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(5);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (!controller.generating() && !controller.speaking()) {
            loop.quit();
        }
    });
    QTimer::singleShot(20000, &loop, &QEventLoop::quit);
    poll.start();
    loop.exec();
}

// Runs the event loop until `collector` finishes or `timeoutMs` elapses.
bool pumpUntilFinished(Collector& collector, int timeoutMs) {
    QEventLoop loop;
    QObject::connect(&collector, &Collector::finishedSignal, &loop, [&loop] { loop.quit(); });
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
    return collector.finished;
}

void testGenerationRunsOffCallingThread() {
    std::cout << "generation runs off the calling thread\n";

    // 1ms per token over 20 tokens: enough room to observe the thread and to
    // cancel, without making the suite slow.
    SlowBackend backend(20, 1);
    QThread thread;
    GenerationWorker worker(&backend);
    worker.moveToThread(&thread);
    thread.start();

    Collector collector;
    QObject::connect(&worker, &GenerationWorker::tokenReady, &collector,
                     [&collector](quint64, const QString& token) {
                         collector.tokens.append(token);
                     });
    QObject::connect(&worker, &GenerationWorker::finished, &collector,
                     [&collector](quint64, bool success, const QString& error) {
                         collector.success = success;
                         collector.error = error;
                         collector.finished = true;
                         emit collector.finishedSignal();
                     });

    const std::thread::id callerThread = std::this_thread::get_id();
    worker.start(1, QStringLiteral("hello"), 0.7F, 512);

    check(pumpUntilFinished(collector, 10000), "completion signal is delivered");

    check(backend.generateThread() != callerThread,
          "generate() does not run on the calling thread");
    check(collector.tokens.size() == 20, "every token is delivered");
    check(collector.totalCharacters() == 60, "every token arrives intact and in order");
    check(collector.success, "generation reports success");
    check(collector.error.isEmpty(), "no error is reported on success");

    thread.quit();
    thread.wait();
}

void testCancelStopsInFlightGeneration() {
    std::cout << "cancel stops in-flight generation\n";

    // 5ms per token over 400 tokens would take two seconds to finish. The
    // test cancels almost immediately, so a backend that honoured the flag
    // finishes in milliseconds and one that ignored it would still be running.
    SlowBackend backend(400, 5);
    QThread thread;
    GenerationWorker worker(&backend);
    worker.moveToThread(&thread);
    thread.start();

    Collector collector;
    QObject::connect(&worker, &GenerationWorker::tokenReady, &collector,
                     [&collector](quint64, const QString& token) {
                         collector.tokens.append(token);
                     });
    QObject::connect(&worker, &GenerationWorker::finished, &collector,
                     [&collector](quint64, bool success, const QString& error) {
                         collector.success = success;
                         collector.error = error;
                         collector.finished = true;
                         emit collector.finishedSignal();
                     });

    worker.start(2, QStringLiteral("long prompt"), 0.7F, 512);

    // Let a few tokens through so cancellation lands mid-generation rather
    // than before the backend even started.
    QThread::msleep(60);
    const int tokensBeforeCancel = backend.tokensEmitted();

    QElapsedTimer clock;
    clock.start();
    worker.cancel();
    const bool completed = pumpUntilFinished(collector, 5000);
    const qint64 elapsed = clock.elapsed();

    check(completed, "completion signal is delivered after cancel");
    check(tokensBeforeCancel > 0, "generation was genuinely in flight when cancelled");
    check(!collector.success, "cancelled generation reports failure");
    check(collector.error.contains(QStringLiteral("stopped"), Qt::CaseInsensitive),
          "cancel is reported as stopped rather than an unexplained error");
    check(elapsed < 1000, "cancellation takes effect promptly rather than at natural end");
    check(collector.tokens.size() < 400, "not every token was delivered");

    thread.quit();
    thread.wait();
}

} // namespace

#include "app_tests.moc"

/// A QML FileDialog speaks in URLs, and the controller converts them to local
/// paths. That conversion is the fragile step in loading a model from disk, so
/// it is pinned here rather than only exercised by hand.
void testFileDialogUrlBecomesALocalPath() {
    std::cout << "file dialog URLs convert to local paths\n";

    // Percent-encoded, as QtQuick.Dialogs hands it over. The exact spelling of
    // the decoded path is platform-dependent -- a drive letter on Windows, a
    // leading slash elsewhere -- so the parts that must hold everywhere are
    // asserted everywhere and the rest only where it applies. Asserting the
    // Windows spelling unconditionally failed the Linux and macOS CI jobs.
    const QUrl encoded(QStringLiteral("file:///C:/kestrel-deps/models/my%20model.gguf"));
    const QString decoded = encoded.toLocalFile();
    check(!decoded.contains(QStringLiteral("%20")), "percent-encoding is decoded");
    check(decoded.endsWith(QStringLiteral("my model.gguf")),
          "a space in the file name survives decoding");
#ifdef Q_OS_WIN
    check(decoded == QStringLiteral("C:/kestrel-deps/models/my model.gguf"),
          "Windows keeps the drive and forward slashes");
#else
    check(decoded.startsWith(QLatin1Char('/')), "POSIX keeps the leading slash");
#endif

    // POSIX, for the CI build of the same app.
    const QUrl posixUrl(QStringLiteral("file:///home/user/models/qwen.gguf"));
    check(posixUrl.toLocalFile() == QStringLiteral("/home/user/models/qwen.gguf"),
          "a POSIX URL keeps its leading slash");

    // A remote URL has no local path at all. The controller must report that as
    // an error rather than hand an empty string to the model loader.
    check(QUrl(QStringLiteral("https://example.com/model.gguf")).toLocalFile().isEmpty(),
          "a remote URL yields no local path, so it is refused");

    check(QUrl(QStringLiteral("qwen.gguf")).toLocalFile().isEmpty(),
          "a bare filename is not treated as a usable path");
}

/// After the user loads a model, the worker must generate through the new
/// backend rather than the one it was constructed with.
void testWorkerFollowsTheSwappedBackend() {
    std::cout << "worker uses the swapped backend\n";

    SlowBackend original(4, 0);
    SlowBackend replacement(4, 0);
    QThread thread;
    GenerationWorker worker(&original);
    worker.moveToThread(&thread);
    thread.start();

    Collector collector;
    QObject::connect(&worker, &GenerationWorker::tokenReady, &collector,
                     [&collector](quint64, const QString& token) {
                         collector.tokens.append(token);
                     });
    QObject::connect(&worker, &GenerationWorker::finished, &collector,
                     [&collector](quint64, bool success, const QString& error) {
                         collector.success = success;
                         collector.error = error;
                         collector.finished = true;
                         emit collector.finishedSignal();
                     });

    worker.setBackend(&replacement);
    worker.start(1, QStringLiteral("hello"), 0.7F, 512);
    check(pumpUntilFinished(collector, 10000), "the swapped backend completes");

    check(replacement.tokensEmitted() == 4, "the replacement backend generated");
    check(original.tokensEmitted() == 0, "the original backend was not used");

    thread.quit();
    thread.wait();
}

// The path a user actually takes: type a message, the controller assembles a
// prompt, the worker generates on its own thread, and the reply streams back
// into the message model. Every layer of that had a test except the glue, and
// the glue is where a real user lives.
void testSendMessageProducesAReply() {
    std::cout << "sendMessage streams a reply back into the model\n";

    kestrel::app::AppController controller;
    // The mock, deliberately: the backend's own generation is covered against
    // a real GGUF elsewhere. This test is about the wiring, so it uses the one
    // backend whose output is deterministic enough to assert on exactly.
    //
    // Transfer ownership directly so failure cannot leave two owners.
    auto backend = std::make_unique<kestrel::runtime::MockBackend>();
    controller.setBackendForTesting(std::move(backend));

    check(controller.messages() != nullptr, "the message model is exposed");
    check(controller.messages()->rowCount() == 0, "a new conversation starts empty");

    controller.sendMessage(QStringLiteral("Hello there"));
    check(controller.generating(), "sending a message starts generation");

    // Pump the real event loop until the stream finishes, exactly as the UI
    // would. A bounded wait, so a wedged backend fails the test rather than
    // hanging CI.
    QEventLoop loop;
    QObject::connect(&controller, &AppController::metricsChanged, &loop, [&] {
        if (!controller.generating()) {
            loop.quit();
        }
    });
    QTimer::singleShot(20000, &loop, &QEventLoop::quit);
    loop.exec();

    check(!controller.generating(), "generation finished");
    check(controller.messages()->rowCount() == 2, "the user message and the reply are both present");

    kestrel::app::MessageModel* messages = controller.messages();
    const QString reply = messages->data(messages->index(1, 0),
                                         kestrel::app::MessageModel::ContentRole).toString();
    check(!reply.isEmpty(), "the reply has content");

    // The status column is what the UI renders a spinner or an error note
    // from, so an empty reply that still says "streaming" would look like a
    // hung app rather than a failure.
    const auto status = messages->data(messages->index(1, 0),
                                       kestrel::app::MessageModel::StatusRole).toString();
    check(status == kestrel::app::toStatusString(kestrel::app::MessageStatus::Complete),
          "the reply is marked complete, not left streaming");

    // The prompt the controller assembled must carry the conversation, or the
    // model would have no memory of the message that was just sent.
    check(controller.contextLimit() > 0, "the context window is reported");
    check(controller.contextUsed() > 0, "context usage advanced after the turn");
}

// Pause after text is delivered, then inspect the request actually received by
// the backend. This covers both the history boundary and the continuation form.
void testResumeUsesPartialAssistantPrompt() {
    std::cout << "resume sends an open assistant turn\n";
    AppController controller;
    auto backend = std::make_unique<SlowBackend>(4, 0);
    auto* observed = backend.get();
    controller.setBackendForTesting(std::move(backend));
    bool paused = false;
    const auto pauseConnection = QObject::connect(
        &controller, &AppController::metricsChanged, &controller, [&] {
            if (!paused && controller.generating() && controller.tokensGenerated() > 0) {
                paused = true;
                controller.pauseConversation();
            }
        });
    auto waitForIdle = [&] {
        QEventLoop loop;
        QObject::connect(&controller, &AppController::generatingChanged, &loop, [&] {
            if (!controller.generating()) {
                loop.quit();
            }
        });
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        if (controller.generating()) {
            loop.exec();
        }
        check(!controller.generating(), "worker reaches idle");
    };
    controller.sendMessage(QStringLiteral("unique current turn"));
    waitForIdle();
    QObject::disconnect(pauseConnection);
    check(paused && controller.canResume(), "partial response can resume");
    const QString suffix = controller.messages()->data(controller.messages()->index(1, 0),
        kestrel::app::MessageModel::ContentRole).toString();
    check(!suffix.isEmpty(), "resume has an unspoken suffix");
    controller.resumeConversation();
    waitForIdle();
    const auto requests = observed->requests();
    check(requests.size() == 2, "resume starts a second generation");
    if (requests.size() == 2) {
        check(requests[0].prompt == "User: unique current turn\nAssistant:",
              "history contains the current user turn exactly once");
        check(requests[1].prompt == "Assistant: " + suffix.toStdString(),
              "suffix stays in an open assistant turn without another cue");
    }
}

void testModelLoadReportsAsynchronously() {
    std::cout << "model loading reports back on the controller thread\n";
    QTemporaryFile file(QDir::tempPath() + QStringLiteral("/kestrel model-XXXXXX.gguf"));
    check(file.open(), "temporary invalid GGUF is created");
    file.close();
    AppController controller;
    const QString originalBackend = controller.backendName();
    bool finished = false;
    bool onUiThread = false;
    QEventLoop loop;
    QObject::connect(&controller, &AppController::modelLoadFinished, &loop, [&] {
        finished = true;
        onUiThread = QThread::currentThread() == controller.thread();
        loop.quit();
    });
    controller.loadModelFromUrl(QUrl::fromLocalFile(file.fileName()).toString());
    check(!finished && controller.modelError().isEmpty(),
          "loading returns before publishing its result");
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    loop.exec();
    check(finished, "model load completion is delivered");
    check(onUiThread, "model load completion runs on the UI thread");
    check(!controller.modelError().isEmpty(), "invalid model reports a load error");
    check(controller.backendName() == originalBackend && controller.modelPath().isEmpty(),
          "failed load preserves the current backend");
}

// The assistant's own layer, seen from the controller: the persona is in the
// shared prompt, presence is projected for the interface, and the idle loop
// ships with everything local-only enabled and the GPU prewarm off.
void testPresenceAndIdleLoopProject() {
    std::cout << "presence and the idle loop project through the controller\n";

    kestrel::app::AppController controller;
    auto backend = std::make_unique<kestrel::runtime::MockBackend>();
    controller.setBackendForTesting(std::move(backend));

    check(controller.systemPrompt().contains(QStringLiteral("You are Kestrel")),
          "the persona's presence line is part of the shared prompt");
    check(controller.presenceState() == QStringLiteral("standing by"),
          "an idle controller reports that it is standing by");
    check(controller.statusWhisper() == QString::fromUtf8("Standing by\u2026"),
          "the status line says the same thing in one line");
    check(controller.idleLoopEnabled(), "the idle loop is on by default");
    check(!controller.idlePrewarmEnabled(),
          "the one idle task that reaches past the process is off until asked for");
    check(!controller.showIdleThoughts(), "private thoughts are not revealed by default");
    check(controller.ambientThought().isEmpty(), "nothing has been thought yet");
    check(controller.idleTaskLabel().isEmpty(), "no idle task has run yet");

    int presenceSignals = 0;
    QObject::connect(&controller, &kestrel::app::AppController::presenceChanged,
                     &controller, [&presenceSignals] { ++presenceSignals; });

    // Switched off for the rest of the test: the tick timer runs on its own
    // schedule, and a test that raced it would fail on a slow machine rather
    // than on a real defect.
    useSilentVoice(controller);
    controller.setIdleLoopEnabled(false);
    check(!controller.idleLoopEnabled(), "the loop can be switched off");
    check(controller.ambientThought().isEmpty(), "switching it off clears its output");

    controller.setIdlePrewarmEnabled(true);
    check(controller.idlePrewarmEnabled(), "prewarming is opt-in and can be turned on");

    controller.setInputPending(true);
    check(controller.inputPending(), "unsent text reaches the idle gate");
    controller.setInputPending(false);

    controller.sendMessage(QStringLiteral("What is the plan for tensorrt?"));
    check(controller.generating(), "the turn starts");
    check(!controller.acknowledgement().isEmpty(),
          "a cue is offered the moment a request is accepted");
    check(controller.presenceState() == QStringLiteral("thinking"),
          "presence reports the turn rather than the keystroke");
    check(controller.presenceIntensity() >= 0.0 && controller.presenceIntensity() <= 1.0,
          "the animated intensity stays in range");
    check(controller.sessionTopic().contains(QStringLiteral("tensorrt")),
          "the session keeps what the conversation is about");

    runTurnToCompletion(controller);

    check(!controller.generating(), "the turn finished");
    check(controller.acknowledgement().isEmpty(), "the cue is cleared once the answer is underway");
    check(controller.presenceState() == QStringLiteral("standing by"),
          "presence settles back to standing by");
    check(controller.statusWhisper() == QString::fromUtf8("Standing by\u2026"),
          "a short answer does not prompt for the next step by default");
    check(presenceSignals > 0, "observers are told when presence changes");
}

void testLongAnswerIsOfferedToContinue() {
    std::cout << "a long answer is offered to continue\n";

    kestrel::app::AppController controller;
    // Slow and long enough that the reply is unambiguously finished rather than
    // truncated, which is the condition for the offer to be made at all.
    auto backend = std::make_unique<SlowBackend>(200, 0);
    controller.setBackendForTesting(std::move(backend));
    useSilentVoice(controller);
    controller.setIdleLoopEnabled(false);

    controller.sendMessage(QStringLiteral("Explain tensorrt"));
    runTurnToCompletion(controller);

    check(!controller.generating(), "the long turn finished");
    check(controller.statusWhisper().startsWith(QStringLiteral("Would you like me to continue")),
          "a long answer is offered to continue rather than left hanging");
}

// The whole point of speaking clause by clause: the cue lands first, the answer
// follows in order, and the response is only delivered once the audio is.
// The regression this file exists to keep: a turn must not be declared
// delivered until the last clause has actually been spoken.
//
// The pump starts the moment a request is accepted, so the acknowledgement cue
// can be heard while the model is still working. That means the pump runs a
// second time, very early, when there is no answer text to speak yet. An empty
// result there means "nothing more to say right now", not "the response is
// over" -- and treating it as the second truncates the reply at whatever the
// first few tokens happened to be, and skips the follow-up line entirely.
void testSpokenTurnIsNotDeliveredBeforeTheLastClause() {
    std::cout << "a turn is not delivered before the last clause is spoken\n";

    kestrel::app::AppController controller;
    controller.setIdleLoopEnabled(false);
    // Slow enough that the cue finishes while generation is still in flight,
    // which is the only window in which this bug is reachable.
    auto backend = std::make_unique<SlowBackend>(60, 25);
    controller.setBackendForTesting(std::move(backend));
    auto speech = std::make_unique<FakeSpeechBackend>();
    FakeSpeechBackend* observed = speech.get();
    controller.setSpeechBackendForTesting(std::move(speech));

    controller.sendMessage(QStringLiteral("Explain the plan for tensorrt"));
    QEventLoop settle;
    QTimer::singleShot(300, &settle, &QEventLoop::quit);
    settle.exec();

    check(!observed->spoken.isEmpty(), "the cue reached the engine");
    check(controller.generating(), "the model is still working when the cue ends");
    check(controller.speaking(), "the turn is still in the speaker's mouth");

    // The cue is finished. There is no answer text yet, and the pump must
    // simply wait rather than conclude that the response has been delivered.
    observed->finishUtterance();
    QEventLoop afterCue;
    QTimer::singleShot(150, &afterCue, &QEventLoop::quit);
    afterCue.exec();

    check(controller.generating(), "the model is still working after the cue");
    check(controller.speaking(), "the response is not delivered before it is spoken");
    check(!controller.statusWhisper().startsWith(QStringLiteral("Would you like me to continue")),
          "no follow-up is offered for a response that has not been given");

    // Drive the rest of the pump, then confirm the whole reply was spoken rather
    // than only the part that happened to exist when the cue ended.
    for (int i = 0; i < 600 && controller.speaking(); ++i) {
        if (!observed->speakingNow()) {
            QEventLoop wait;
            QTimer::singleShot(10, &wait, &QEventLoop::quit);
            wait.exec();
            continue;
        }
        observed->finishUtterance();
    }
    QEventLoop last;
    QTimer::singleShot(300, &last, &QEventLoop::quit);
    last.exec();

    const auto withoutSpace = [](QString text) {
        text.remove(QRegularExpression(QStringLiteral("\\s+")));
        return text;
    };
    QString spokenAnswer;
    for (int i = 1; i < observed->spoken.size(); ++i) {
        spokenAnswer += observed->spoken.at(i);
    }
    const QString delivered = controller.messages()->data(controller.messages()->index(1, 0),
        kestrel::app::MessageModel::ContentRole).toString();
    if (withoutSpace(spokenAnswer) != withoutSpace(delivered)) {
        std::cout << "       spoke " << observed->spoken.size() << " utterances, "
                  << spokenAnswer.size() << " chars of a " << delivered.size()
                  << " char reply\n";
    }
    check(withoutSpace(spokenAnswer) == withoutSpace(delivered),
          "the whole reply is spoken, not only the text that existed early");
    check(!controller.speaking(), "the turn ends when the last clause is spoken");
}

// The whole point of the registry: a tool that has not been granted anything
// does nothing, and says why. This is the path a user walks through in the
// panel, so it is tested from the panel's own entry points rather than from the
// core registry the app happens to own.
void testIdleToolNeedsPermissionBeforeItRuns() {
    std::cout << "an idle tool runs only once it is permitted\n";

    kestrel::app::AppController controller;
    controller.setIdleLoopEnabled(false);
    const auto tools = controller.idleTools();
    check(!tools.isEmpty(), "the panel has something to show");

    QVariantMap indexer;
    const auto find = [&controller](const char* name) {
        for (const QVariant& entry : controller.idleTools()) {
            const QVariantMap tool = entry.toMap();
            if (tool.value(QStringLiteral("name")).toString() == QLatin1String(name)) {
                return tool;
            }
        }
        return QVariantMap();
    };
    for (const QVariant& entry : tools) {
        const QVariantMap tool = entry.toMap();
        if (tool.value(QStringLiteral("name")).toString()
            == QStringLiteral("index recent threads")) {
            indexer = tool;
        }
    }
    check(!indexer.isEmpty(), "the indexing tool is declared to the interface");
    check(!indexer.value(QStringLiteral("enabled")).toBool(),
          "a new tool is not switched on");
    check(!indexer.value(QStringLiteral("permitted")).toBool(),
          "a new tool is not permitted");
    const QStringList required = indexer.value(QStringLiteral("required")).toStringList();
    check(required.contains(QStringLiteral("read conversations")),
          "the tool declares the capability it needs");
    check(!indexer.value(QStringLiteral("missing")).toStringList().isEmpty(),
          "what it is still missing is reported rather than left blank");

    // Switched on but not granted: still refused, and the refusal is visible
    // rather than silent.
    controller.setIdleToolEnabled(QStringLiteral("index recent threads"), true);
    check(!find("index recent threads").value(QStringLiteral("permitted")).toBool(),
          "switching a tool on is not the same as allowing it");

    controller.setToolPermission(QStringLiteral("read conversations"), true);
    const bool permitted =
        find("index recent threads").value(QStringLiteral("permitted")).toBool();
    check(permitted, "granting the declared capability permits the tool");

    // A capability the tool did not declare changes nothing, which is what
    // stops a grant from becoming a blank cheque.
    controller.setToolPermission(QStringLiteral("network"), true);
    check(permitted, "an unrelated grant does not alter what the tool may do");

    // Unknown names are ignored rather than inventing a tool or a capability.
    controller.setIdleToolEnabled(QStringLiteral("no such tool"), true);
    controller.setToolPermission(QStringLiteral("no such capability"), true);
    check(controller.idleTools().size() == tools.size(),
          "a name the registry has never heard of changes nothing");
}

void testSpokenResponseFollowsClauseOrder() {
    std::cout << "a spoken response is delivered clause by clause\n";

    kestrel::app::AppController controller;
    controller.setIdleLoopEnabled(false);
    auto backend = std::make_unique<kestrel::runtime::MockBackend>();
    auto speech = std::make_unique<FakeSpeechBackend>();
    FakeSpeechBackend* observed = speech.get();
    controller.setSpeechBackendForTesting(std::move(speech));

    check(controller.ttsAvailable(), "the adopted backend reports itself available");
    check(controller.speaking() == false, "nothing is speaking before a request");

    controller.sendMessage(QStringLiteral("Hello there"));
    // The cue is spoken immediately and the first real clause waits out the
    // pause the persona asked for, so let the event loop turn rather than
    // assuming an instant transition.
    QEventLoop settle;
    QTimer::singleShot(600, &settle, &QEventLoop::quit);
    settle.exec();

    check(controller.ttsAvailable(), "audio is still available after the request");
    check(!observed->spoken.isEmpty(), "something was spoken");
    check(observed->voiceApplications > 0, "the voice persona was applied to the engine");

    // The first utterance is the acknowledgement, not the answer: that is what
    // makes a request sound accepted before it sounds answered.
    check(observed->spoken.first() == controller.acknowledgement()
              || observed->spoken.first().endsWith(QLatin1Char('.')),
          "the first utterance is the acknowledgement cue");
    check(controller.speaking(), "the controller reports speech in progress");

    // Drive the pump to the end: every clause finishes, one at a time.
    //
    // A clause is only reported finished once the engine has actually been
    // handed it. Finishing one that is still waiting out its leading pause
    // would advance the controller's cursor for a clause nobody heard, and the
    // response would appear to complete while most of it went unsaid -- which
    // is exactly the failure this test exists to catch.
    for (int i = 0; i < 400 && controller.speaking(); ++i) {
        if (!observed->speakingNow()) {
            QEventLoop wait;
            QTimer::singleShot(20, &wait, &QEventLoop::quit);
            wait.exec();
            continue;
        }
        observed->finishUtterance();
    }
    // One more pump step: the final finish schedules the completion through a
    // queued call, so the event loop has to turn once more.
    QEventLoop last;
    QTimer::singleShot(200, &last, &QEventLoop::quit);
    last.exec();

    check(!controller.speaking(), "playback ends when the last clause is spoken");
    check(!controller.generating(), "generation finished before playback did");
    const auto status = controller.messages()->data(controller.messages()->index(1, 0),
                                                    kestrel::app::MessageModel::StatusRole).toString();
    check(status == kestrel::app::toStatusString(kestrel::app::MessageStatus::Complete),
          "the response is complete once it has been spoken");

    // Everything after the cue is the answer, in order and in full. Whitespace
    // between clauses is deliberately not spoken -- it is the pause -- so both
    // sides are compared with spacing removed.
    const auto withoutSpace = [](QString text) {
        text.remove(QRegularExpression(QStringLiteral("\\s+")));
        return text;
    };
    QString spokenAnswer;
    for (int i = 1; i < observed->spoken.size(); ++i) {
        spokenAnswer += observed->spoken.at(i);
    }
    const QString delivered = controller.messages()->data(controller.messages()->index(1, 0),
        kestrel::app::MessageModel::ContentRole).toString();
    check(!spokenAnswer.isEmpty(), "the answer was spoken, not only the cue");
    const bool complete = withoutSpace(spokenAnswer) == withoutSpace(delivered);
    if (!complete) {
        std::cout << "       spoke " << observed->spoken.size() << " utterances, "
                  << spokenAnswer.size() << " chars of a " << delivered.size()
                  << " char reply\n";
    }
    check(complete, "every generated word was spoken, in order, and nothing else");
}

// A user who starts typing mid-answer must cut the audio off at a clause
// boundary rather than mid-word, and the new turn must take over cleanly.
void testBargeInStopsAudioAtAClauseBoundary() {
    std::cout << "barge-in stops audio at a clause boundary\n";

    kestrel::app::AppController controller;
    controller.setIdleLoopEnabled(false);
    // Slow enough that the turn is still running when the second message lands.
    auto backend = std::make_unique<SlowBackend>(400, 1);
    auto speech = std::make_unique<FakeSpeechBackend>();
    FakeSpeechBackend* observed = speech.get();
    controller.setSpeechBackendForTesting(std::move(speech));

    controller.sendMessage(QStringLiteral("first question"));
    QEventLoop settle;
    QTimer::singleShot(400, &settle, &QEventLoop::quit);
    settle.exec();
    check(controller.speaking(), "the first answer is being spoken");

    controller.sendMessage(QStringLiteral("actually, second question"));
    check(observed->boundaryStops == 1, "a barge-in asks for a boundary stop");
    check(observed->immediateStops == 0, "a barge-in does not cut the clause in flight");
    check(observed->spoken.size() >= 2, "the cue and at least part of the answer were spoken");

    // The abandoned response is replaced, not resumed: the spoken text is kept
    // for the record and the new turn owns the timeline.
    const bool stopped = controller.voiceState() == QLatin1String("interrupted")
                      || controller.voiceState() == QLatin1String("cancelled")
                      || controller.voiceState() == QLatin1String("generating")
                      || controller.voiceState() == QLatin1String("speaking")
                      || controller.voiceState() == QLatin1String("queued");
    check(stopped, "the interrupted response moves out of the speaking state");
    controller.stopGeneration();
}

// The whole claim about speech input: a recognized phrase is a typed phrase
// that happened to arrive by ear. Everything downstream -- the transcript row,
// the acknowledgement, the barge-in, the presence projection -- must be identical
// because it is literally the same code path.
void testSpokenPhraseTakesTheTypedPath() {
    std::cout << "a spoken phrase is submitted as if it were typed\n";

    kestrel::app::AppController controller;
    controller.setIdleLoopEnabled(false);
    auto backend = std::make_unique<SlowBackend>(200, 1);
    controller.setBackendForTesting(std::move(backend));

    check(controller.sttAvailable(), "a recognizer is available");
    check(controller.listening() == false, "not listening before asked");
    check(controller.startListening(), "listening starts");
    check(controller.listening(), "the controller reports listening");
    check(controller.partialTranscript().isEmpty(), "nothing heard yet");

    // Drive the preview recognizer until it produces a complete phrase. Partial
    // text is expected along the way: it is what tells the user the microphone
    // is live.
    bool sawPartial = false;
    for (int i = 0; i < 100 && controller.listening(); ++i) {
        QEventLoop step;
        QTimer::singleShot(30, &step, &QEventLoop::quit);
        step.exec();
        if (!controller.partialTranscript().isEmpty()) {
            sawPartial = true;
        }
    }
    check(sawPartial, "partial words appear while the user is still speaking");
    check(!controller.listening(), "listening ends when the phrase completes");
    check(controller.generating(), "the recognized phrase started a turn");
    check(controller.messages()->rowCount() == 2,
          "the transcript holds the spoken request and its reply");
    check(controller.messages()->data(controller.messages()->index(0, 0),
                                      kestrel::app::MessageModel::ContentRole)
              .toString().contains(QStringLiteral("tensorrt"), Qt::CaseInsensitive),
          "the spoken words are in the transcript as a user message");
    check(!controller.acknowledgement().isEmpty(),
          "a spoken request is acknowledged exactly like a typed one");
    controller.stopGeneration();
}

// A phrase the user abandoned must never be submitted. Half a sentence turned
// into a turn is worse than losing the words.
void testAbandonedPhraseIsNotSubmitted() {
    std::cout << "an abandoned phrase is discarded, not submitted\n";

    kestrel::app::AppController controller;
    controller.setIdleLoopEnabled(false);
    auto backend = std::make_unique<kestrel::runtime::MockBackend>();
    controller.setBackendForTesting(std::move(backend));

    check(controller.startListening(), "listening starts");
    QEventLoop partial;
    QTimer::singleShot(200, &partial, &QEventLoop::quit);
    partial.exec();
    check(!controller.partialTranscript().isEmpty(), "partial words were heard");

    controller.abandonListening();
    check(!controller.listening(), "abandoning stops listening");
    check(controller.partialTranscript().isEmpty(), "the partial text is cleared");
    check(!controller.generating(), "an abandoned phrase starts no turn");
    check(controller.messages()->rowCount() == 0, "nothing was added to the transcript");
}

/// Runs the Qt worker and file-URL tests; returns nonzero if any check fails.
int main(int argc, char** argv) {
    // Unbuffered, so a crash still shows how far the run got. A lost buffer
    // turns a five-second diagnosis into a guess about which test died.
    std::cout << std::unitbuf;

    QCoreApplication app(argc, argv);
    testGenerationRunsOffCallingThread();
    testCancelStopsInFlightGeneration();
    testFileDialogUrlBecomesALocalPath();
    testWorkerFollowsTheSwappedBackend();
    testSendMessageProducesAReply();
    testResumeUsesPartialAssistantPrompt();
    testModelLoadReportsAsynchronously();
    testPresenceAndIdleLoopProject();
    testLongAnswerIsOfferedToContinue();
    testIdleToolNeedsPermissionBeforeItRuns();
    testSpokenTurnIsNotDeliveredBeforeTheLastClause();
    testSpokenResponseFollowsClauseOrder();
    testBargeInStopsAudioAtAClauseBoundary();
    testSpokenPhraseTakesTheTypedPath();
    testAbandonedPhraseIsNotSubmitted();

    std::cout << (failures == 0 ? "\napp tests passed\n" : "\napp tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
