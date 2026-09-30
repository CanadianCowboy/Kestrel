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
#include "app/listensession.h"
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

    void setTokenPieces(std::vector<std::string> pieces) { m_pieces = std::move(pieces); }

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
            onToken(m_pieces.empty() ? std::string_view("tok")
                                    : std::string_view(m_pieces[static_cast<std::size_t>(i) % m_pieces.size()]));
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
    std::vector<std::string> m_pieces;
    int m_tokenCount;
    int m_delayMs;
    std::atomic<bool> m_cancelled{false};
    std::atomic<int> m_tokensEmitted{0};
    std::thread::id m_generateThread{};
};

// A queued fake recognizer that lets the test deliver callbacks from a stopped
// session after a replacement session has started.
class QueuedFakeRecognizer final : public kestrel::runtime::SpeechRecognizer {
public:
    bool available() const override { return true; }
    std::string detail() const override { return "queued callback test recognizer"; }
    bool start(ResultCallback onResult, EndCallback onEnd, std::string&) override {
        m_results.push_back(std::move(onResult));
        m_ends.push_back(std::move(onEnd));
        m_listening = true;
        return true;
    }
    void stop() override { m_listening = false; }
    void stopAndWait() override { stop(); }
    bool listening() const override { return m_listening; }

    void emitQueuedResult(std::size_t session, std::string text) {
        kestrel::runtime::RecognitionResult result;
        result.text = std::move(text);
        m_results.at(session)(result);
    }

private:
    bool m_listening = false;
    std::vector<ResultCallback> m_results;
    std::vector<EndCallback> m_ends;
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
    [[nodiscard]] bool usable() const override { return m_usable; }
    [[nodiscard]] QString description() const override {
        // Truthful about the two states, as the real engines are: a voice that
        // is still coming up and a voice that is working are not the same
        // report.
        return m_usable ? QStringLiteral("fake voice")
                        : QStringLiteral("the fake voice is still loading");
    }

    void applyVoice(const kestrel::core::VoicePersona& persona) override {
        ++voiceApplications;
        lastWarmth = persona.warmth;
        // The pace is captured here and remembered, exactly as the local voice
        // model does with speedForRequest(). That is the whole point of this
        // fake: a backend holds the last pace it was handed, and audio already
        // synthesised at the wrong one cannot be corrected when it plays.
        m_speed = kestrel::core::paceFor(persona);
    }

    // A local model is installed before it can answer, which is the whole
    // difference this fake has to be able to express.
    [[nodiscard]] bool present() const override { return m_present; }

    void speak(const QString& text) override {
        spoken.append(text);
        // Everything the engine is asked for, in the order it is asked, whether
        // to speak it now or to have it ready. A reply whose second clause is
        // requested before its first is one the engine numbers backwards.
        requests.append(text);
        speakSpeeds.append(m_speed);
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

    // Stands in for a local model coming up. Reports the same transition the
    // engine reports, so the controller is driven by the engine announcing
    // itself rather than by a test watching a boolean.
    void becomeReady() {
        m_usable = true;
        reportAvailable();
    }

    // Stands in for an engine that will never answer.
    void giveUp() {
        m_present = false;
        m_usable = false;
        reportUnavailable();
    }

    // The starting state: installed, launched, still loading its model.
    void startLoading() {
        m_present = true;
        m_usable = false;
    }

    // A plain class cannot be a QTimer context, so the owner lends one. The
    // queued report is dropped if the owner dies first.
    void setTimerContext(QObject* context) { m_context = context; }

    // A backend that can start work early records the pace it is being asked
    // for right now, which is the pace that audio is then stuck with.
    void prefetch(const QString& text) override {
        prefetched.append(text);
        requests.append(text);
        prefetchSpeeds.append(m_speed);
    }

    void stop() override { ++boundaryStops; }

    void stopImmediately() override { ++immediateStops; }

    [[nodiscard]] bool speakingNow() const override { return m_speaking; }

    // Simulates the engine reaching the end of an utterance.
    void finishUtterance() {
        m_speaking = false;
        reportFinished();
    }

    QStringList spoken;
    QStringList prefetched;
    // Every request the engine received, in order, prefetches included.
    QStringList requests;
    // The pace in force at the moment each clause was asked for, in the order
    // they were asked. A reply whose clauses disagree here is a reply that
    // changes speed partway through.
    QList<double> speakSpeeds;
    QList<double> prefetchSpeeds;
    int voiceApplications = 0;
    // The warmth of the last voice persona the synthesizer was handed.
    float lastWarmth = 0.0F;
    // The pace the engine is currently set to.
    [[nodiscard]] double speed() const { return m_speed; }
    int boundaryStops = 0;
    int immediateStops = 0;

private:
    double m_speed = 1.0;
    bool m_usable = true;
    bool m_present = true;
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

// Gives a controller a model backend that answers immediately, for any test
// that sends a message and then asserts on what came back.
//
// This is the counterpart to useSilentVoice and exists for the same underlying
// reason. A controller with no backend injected adopts whatever the registry
// offers, and for as long as this build linked no llama.cpp that was the mock
// -- so a great many tests here were, without saying so, testing the mock.
// The moment a real backend became available they adopted it instead, no model
// was loaded, nothing was generated, and the assertions failed for a reason
// that had nothing to do with what they were written to check.
//
// A test that names its backend cannot fail that way, and cannot quietly stop
// being a test of the controller and become a test of llama.cpp.
void useImmediateModel(kestrel::app::AppController& controller) {
    controller.setBackendForTesting(std::make_unique<kestrel::runtime::MockBackend>());
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

void testListenSessionIgnoresQueuedCallbacksFromPreviousSession() {
    std::cout << "listen session ignores stale queued callbacks after restart\n";
    QueuedFakeRecognizer recognizer;
    kestrel::app::ListenSession session(recognizer);
    QString error;
    check(session.startListening(error), "first listening session starts");
    recognizer.emitQueuedResult(0, "stale words");
    session.stopListening();
    check(session.startListening(error), "replacement listening session starts");
    recognizer.emitQueuedResult(1, "current words");
    QCoreApplication::processEvents();
    check(session.partialText() == QStringLiteral("current words"),
          "queued results from an earlier session cannot overwrite the current transcript");
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
    worker.start(1, {kestrel::runtime::ChatMessage{kestrel::runtime::Role::User, "hello"}}, 0.7F, 512);

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

void testTokensStreamBeforeCompletionAndPreserveUtf8() {
    std::cout << "tokens stream before completion and preserve split UTF-8\n";
    SlowBackend backend(3, 30);
    backend.setTokenPieces({"live ", "\xe6\xa8", "\xa1"});
    QThread thread;
    GenerationWorker worker(&backend);
    worker.moveToThread(&thread);
    thread.start();
    Collector collector;
    bool sawLiveToken = false;
    QObject::connect(&worker, &GenerationWorker::tokenReady, &collector,
                     [&](quint64, const QString& token) {
                         collector.tokens.append(token);
                         sawLiveToken = sawLiveToken || (!collector.finished && backend.tokensEmitted() < 3);
                     });
    // The ASCII token must reach the UI before generation has emitted all
    // pieces. A callback queued back onto the worker fails this check.
    QObject::connect(&worker, &GenerationWorker::finished, &collector,
                     [&](quint64, bool success, const QString&) {
                         collector.success = success;
                         collector.finished = true;
                         emit collector.finishedSignal();
                     });
    worker.start(3, QStringLiteral("hello"), 0.7F, 16);
    check(pumpUntilFinished(collector, 5000), "streaming test completes");
    check(sawLiveToken,
          "tokens are received while generation is still running");
    check(collector.tokens.join(QString()) == QString::fromUtf8("live \xe6\xa8\xa1"),
          "UTF-8 bytes split across native tokens form one intact character");
    check(collector.success, "streamed generation succeeds");
    thread.quit();
    thread.wait();
}

void testCancelBeforeWorkerStarts() {
    std::cout << "cancel before queued generation starts\n";
    SlowBackend backend(4, 0);
    QThread thread;
    GenerationWorker worker(&backend);
    worker.moveToThread(&thread);
    Collector collector;
    QObject::connect(&worker, &GenerationWorker::finished, &collector,
                     [&](quint64, bool success, const QString&) {
                         collector.success = success;
                         collector.finished = true;
                         emit collector.finishedSignal();
                     });
    worker.start(4, QStringLiteral("hello"), 0.7F, 16);
    worker.cancel();
    thread.start();
    check(pumpUntilFinished(collector, 5000), "pre-start cancellation completes");
    check(!collector.success && backend.requests().empty(),
          "cancelled queued request never enters the backend");
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

    worker.start(2, {kestrel::runtime::ChatMessage{kestrel::runtime::Role::User, "hello"}}, 0.7F, 512);

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
    worker.start(1, {kestrel::runtime::ChatMessage{kestrel::runtime::Role::User, "hello"}}, 0.7F, 512);
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
        check(requests[0].messages.size() == 1
                  && requests[0].messages[0].role == kestrel::runtime::Role::User
                  && requests[0].messages[0].content == "unique current turn",
              "history contains the current user turn exactly once");
        check(requests[1].messages.size() == 2
                  && requests[1].messages.back().role == kestrel::runtime::Role::Assistant
                  && requests[1].messages.back().content == suffix.toStdString()
                  && !requests[1].addAssistantCue,
              "resume preserves history and leaves the partial assistant turn open");
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
    // Prewarming is on by default now, and can still be switched off. It was
    // the reverse, and that default meant the warmup never ran: a capability
    // that is off is not a capability, whatever the setting is called. So this
    // asserts both halves -- on by default, and still switchable -- because
    // only the first was ever verified.
    check(controller.idlePrewarmEnabled(),
          "prewarming is on by default, so a cold GPU is warmed at all");
    controller.setIdlePrewarmEnabled(false);
    check(!controller.idlePrewarmEnabled(), "prewarming can still be turned off");
    controller.setIdlePrewarmEnabled(true);
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
    useImmediateModel(controller);
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
    // stops a grant from becoming a blank cheque. Re-read the value rather
    // than reusing `permitted`: that local was captured before this grant, so
    // asserting on it here passed whether or not the grant had any effect, and
    // a test that cannot fail is not a test.
    controller.setToolPermission(QStringLiteral("network"), true);
    check(find("index recent threads").value(QStringLiteral("permitted")).toBool(),
          "an unrelated grant does not alter what the tool may do");

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
    // The backend is injected, not inherited. A `backend` local that was
    // constructed and then dropped on the floor was the only reason this test
    // passed: the controller picked whatever the registry offered, and for as
    // long as no llama.cpp was linked that was the mock. The moment a real
    // backend became available the controller adopted it, no model was loaded,
    // nothing was generated, and the assertions below failed -- which is the
    // same coupling that made the shipped app answer with the mock.
    //
    // A test that states which backend it drives cannot break that way.
    controller.setBackendForTesting(std::move(backend));
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

// The persona's warmth dial has to reach the engine, or "warmer as the mood
// shifts" is a claim about a constant. The persona is otherwise only reachable
// through the idle loop, which drifts a dial far too slowly to assert on, so it
// is put where the loop would have put it and the value the engine was handed
// is read back.
void testWarmthReachesTheVoice() {
    std::cout << "the persona's warmth dial reaches the voice\n";

    kestrel::app::AppController controller;
    useImmediateModel(controller);
    controller.setIdleLoopEnabled(false);
    auto speech = std::make_unique<FakeSpeechBackend>();
    FakeSpeechBackend* observed = speech.get();
    controller.setSpeechBackendForTesting(std::move(speech));

    // Deliberately not the voice persona's own default of 0.6, so a backend
    // handed the constant and a backend handed the dial are distinguishable.
    const float warm = 0.9F;
    const float cool = 0.2F;

    kestrel::core::PersonaState state;
    state.warmth = warm;
    controller.setPersonaStateForTesting(state);
    controller.sendMessage(QStringLiteral("a warm question"));
    QEventLoop warmTurn;
    QTimer::singleShot(300, &warmTurn, &QEventLoop::quit);
    warmTurn.exec();
    check(observed->voiceApplications > 0, "the warm voice was handed to the engine");
    check(qAbs(observed->lastWarmth - warm) < 0.001F,
          "the engine was handed the persona's warmth, not the default");

    // And the other direction, on a second response, so the check is that the
    // dial is read per response rather than that a value was latched once.
    for (int i = 0; i < 400 && controller.speaking(); ++i) {
        if (!observed->speakingNow()) {
            QEventLoop wait;
            QTimer::singleShot(20, &wait, &QEventLoop::quit);
            wait.exec();
            continue;
        }
        observed->finishUtterance();
    }
    state.warmth = cool;
    controller.setPersonaStateForTesting(state);
    controller.sendMessage(QStringLiteral("a cool question"));
    QEventLoop coolTurn;
    QTimer::singleShot(300, &coolTurn, &QEventLoop::quit);
    coolTurn.exec();
    check(qAbs(observed->lastWarmth - cool) < 0.001F,
          "a colder mood is handed over on the next response");
    controller.stopGeneration();
}

// A reply asked for before the voice can answer is still a reply that was asked
// for, and the first thing anyone hears in a session should not be text just
// because a local model was still loading. The reply is owed audio until the
// engine turns up, and spoken then -- from its own unspoken remainder, so
// nothing is said twice and nothing left over from an earlier turn is spoken
// in its place.
void testReplyOwedAudioIsSpokenWhenTheVoiceArrives() {
    std::cout << "a reply asked for before the voice is ready is spoken once it is\n";

    // A control run first: the same question, with the voice ready from the
    // start. What the late voice has to produce is exactly this, which is the
    // only way to notice a clause said twice or a previous turn's text spoken
    // in place of this one's.
    kestrel::app::AppController control;
    control.setIdleLoopEnabled(false);
    useImmediateModel(control);
    auto controlSpeech = std::make_unique<FakeSpeechBackend>();
    FakeSpeechBackend* controlVoice = controlSpeech.get();
    controlSpeech->setTimerContext(&control);
    controlSpeech->setAutoFinish(true);
    control.setSpeechBackendForTesting(std::move(controlSpeech));
    const QString question = QStringLiteral("what did you make of that");
    control.sendMessage(question);
    // Read before the loop, while the cue is still the one about to be spoken.
    const QString cue = control.acknowledgement();
    for (int i = 0; i < 400 && control.speaking(); ++i) {
        QEventLoop wait;
        QTimer::singleShot(20, &wait, &QEventLoop::quit);
        wait.exec();
    }
    check(!controlVoice->spoken.isEmpty(), "the control reply was spoken");
    if (controlVoice->spoken.isEmpty()) {
        return;
    }
    // The cue belongs to the moment the request was made. A reply whose audio
    // starts late has no cue left to give, and saying "got it" several seconds
    // late would be worse than not saying it, so the cue is the one thing
    // allowed to differ. Everything after it is the reply.
    QStringList expected = controlVoice->spoken;
    if (!cue.isEmpty() && expected.first() == cue) {
        expected.removeFirst();
    }

    // The real case: the engine is installed and launched but has not finished
    // loading, which is what a cold start looks like.
    kestrel::app::AppController controller;
    useImmediateModel(controller);
    controller.setIdleLoopEnabled(false);
    auto speech = std::make_unique<FakeSpeechBackend>();
    FakeSpeechBackend* observed = speech.get();
    speech->setTimerContext(&controller);
    speech->setAutoFinish(true);
    speech->startLoading();
    controller.setSpeechBackendForTesting(std::move(speech));

    controller.sendMessage(question);
    QEventLoop loading;
    QTimer::singleShot(400, &loading, &QEventLoop::quit);
    loading.exec();

    check(observed->spoken.isEmpty(), "nothing is asked of a voice that cannot answer yet");
    check(!controller.speaking(), "no playback is claimed while the voice is loading");

    // The model finishes loading and the engine says so.
    observed->becomeReady();
    check(controller.ttsAvailable(), "the voice is available once the engine says it is");

    for (int i = 0; i < 400 && controller.speaking(); ++i) {
        QEventLoop wait;
        QTimer::singleShot(20, &wait, &QEventLoop::quit);
        wait.exec();
    }

    check(!observed->spoken.isEmpty(), "the held reply is spoken once the voice arrives");
    check(observed->spoken == expected,
          "the held reply speaks exactly the clauses a ready voice would have spoken");
    check(observed->spoken.size() == expected.size(),
          "no clause is spoken twice");
    bool distinct = true;
    for (int i = 0; i < observed->spoken.size(); ++i) {
        for (int j = i + 1; j < observed->spoken.size(); ++j) {
            distinct = distinct && observed->spoken.at(i) != observed->spoken.at(j);
        }
    }
    check(distinct, "nothing from an earlier turn is spoken in place of this one");
    check(!controller.speaking(), "playback finished");
    controller.stopGeneration();
}

// An engine that will never answer is the other half of the same decision: the
// reply is owed audio only while something is still going to produce it. With
// nothing coming, the text is the honest delivery and the turn must not hang
// waiting for a voice that does not exist.
void testReplyIsDeliveredAsTextWhenTheVoiceNeverArrives() {
    std::cout << "a held reply is delivered as text when the engine gives up\n";

    kestrel::app::AppController controller;
    useImmediateModel(controller);
    controller.setIdleLoopEnabled(false);
    auto speech = std::make_unique<FakeSpeechBackend>();
    FakeSpeechBackend* observed = speech.get();
    speech->setTimerContext(&controller);
    speech->setAutoFinish(true);
    speech->startLoading();
    controller.setSpeechBackendForTesting(std::move(speech));

    controller.sendMessage(QStringLiteral("a question asked of a voice that never arrives"));
    QEventLoop loading;
    QTimer::singleShot(400, &loading, &QEventLoop::quit);
    loading.exec();
    check(observed->spoken.isEmpty(), "nothing is spoken while the engine is still starting");

    observed->giveUp();
    QEventLoop gaveUp;
    QTimer::singleShot(200, &gaveUp, &QEventLoop::quit);
    gaveUp.exec();

    check(observed->spoken.isEmpty(), "an engine that gave up speaks nothing");
    check(!controller.generating(), "the turn is over rather than left spinning");
    check(!controller.speaking(), "nothing is claimed to be speaking");
    controller.stopGeneration();
}

// A clause with a leading pause is held back so the gap before it is real
// silence rather than a gap somewhere else. While it is held, the engine has not
// been asked for it -- and the next clause's prefetch used to go straight past
// and be asked for first. The reply was still spoken in the right order, because
// the synthesiser commits the held clause in turn, but the engine numbered the
// audio it produced in the order it was asked, so the reply's first sentence was
// written to clause-1 and its second to clause-0. Ordering has to be a property
// of the clause, not of when a file happened to be produced.
void testTheEngineIsAskedForClausesInTheOrderTheyAreSpoken() {
    std::cout << "the engine is asked for clauses in the order they are spoken\n";

    kestrel::app::AppController controller;
    useImmediateModel(controller);
    controller.setIdleLoopEnabled(false);
    auto speech = std::make_unique<FakeSpeechBackend>();
    FakeSpeechBackend* observed = speech.get();
    speech->setTimerContext(&controller);
    speech->setAutoFinish(true);
    controller.setSpeechBackendForTesting(std::move(speech));

    controller.sendMessage(QStringLiteral("why does a held clause still come first"));
    // Read while the cue is still the one about to be spoken.
    const QString cue = controller.acknowledgement();

    // The pause is load-bearing and has to stay that way: the cue carries no
    // pause and goes straight to the engine, while the first clause is still
    // parked behind its lead-in when sendMessage returns. A fix that let the
    // lookahead through by sending the clause early would show up here as a
    // second entry in spoken before the event loop has even turned.
    check(observed->spoken.size() <= 1,
          "the first clause is still held back by its leading pause");

    for (int i = 0; i < 400 && controller.speaking(); ++i) {
        QEventLoop wait;
        QTimer::singleShot(20, &wait, &QEventLoop::quit);
        wait.exec();
    }

    // The clauses, in the order they are spoken, which is the order they are
    // supposed to have been asked for. The cue is not one of them: it is spoken
    // with no pause before it, so it never sat in front of anything.
    QStringList clauses = observed->spoken;
    if (!cue.isEmpty() && !clauses.isEmpty() && clauses.first() == cue) {
        clauses.removeFirst();
    }
    QStringList asked = observed->requests;
    if (!cue.isEmpty()) {
        asked.removeAll(cue);
    }

    check(clauses.size() >= 2, "the reply had more than one clause");
    check(!asked.isEmpty(), "the engine was asked for something");
    if (clauses.size() < 2 || asked.isEmpty()) {
        controller.stopGeneration();
        return;
    }

    check(asked.first() == clauses.first(),
          "the first clause is the first thing the engine is asked for");

    // And never a later clause before an earlier one, anywhere in the reply.
    int furthest = -1;
    bool inOrder = true;
    for (const QString& text : asked) {
        const int at = clauses.indexOf(text);
        if (at < 0) {
            inOrder = false;
            break;
        }
        if (at < furthest) {
            inOrder = false;
            break;
        }
        furthest = at;
    }
    check(inOrder, "no clause is asked for before the clause in front of it");
    controller.stopGeneration();
}

// A reply held for a voice belongs to the conversation it was asked in. Leaving
// that conversation abandons its audio, the same way leaving abandons its
// generation -- otherwise the engine arriving a moment later speaks the reply
// into whatever the user has moved on to, with nothing on screen to match it.
void testLeavingAConversationDropsTheReplyItWasHolding() {
    std::cout << "leaving a conversation drops the reply it was holding\n";

    kestrel::app::AppController controller;
    useImmediateModel(controller);
    controller.setIdleLoopEnabled(false);
    auto speech = std::make_unique<FakeSpeechBackend>();
    FakeSpeechBackend* observed = speech.get();
    speech->setTimerContext(&controller);
    speech->setAutoFinish(true);
    speech->startLoading();
    controller.setSpeechBackendForTesting(std::move(speech));

    controller.sendMessage(QStringLiteral("a question asked of a voice still loading"));
    QEventLoop loading;
    QTimer::singleShot(300, &loading, &QEventLoop::quit);
    loading.exec();
    check(observed->spoken.isEmpty(), "nothing is spoken while the engine is loading");

    controller.newConversation();
    observed->becomeReady();
    QEventLoop arrived;
    QTimer::singleShot(300, &arrived, &QEventLoop::quit);
    arrived.exec();

    check(observed->spoken.isEmpty(),
          "a reply held for the conversation the user left is not spoken afterwards");
    check(!controller.speaking(), "nothing is left playing in the new conversation");
    controller.stopGeneration();
}

// Escape is the user's way of saying stop. A reply waiting for a voice is still
// audio they are waiting on, so stopping has to reach it.
void testStoppingDropsAReplyHeldForAVoice() {
    std::cout << "stopping drops a reply held for a voice\n";

    kestrel::app::AppController controller;
    useImmediateModel(controller);
    controller.setIdleLoopEnabled(false);
    auto speech = std::make_unique<FakeSpeechBackend>();
    FakeSpeechBackend* observed = speech.get();
    speech->setTimerContext(&controller);
    speech->setAutoFinish(true);
    speech->startLoading();
    controller.setSpeechBackendForTesting(std::move(speech));

    controller.sendMessage(QStringLiteral("a question asked of a voice still loading"));
    QEventLoop loading;
    QTimer::singleShot(300, &loading, &QEventLoop::quit);
    loading.exec();
    check(observed->spoken.isEmpty(), "nothing is spoken while the engine is loading");

    controller.stopGeneration();
    observed->becomeReady();
    QEventLoop arrived;
    QTimer::singleShot(300, &arrived, &QEventLoop::quit);
    arrived.exec();

    check(observed->spoken.isEmpty(), "a stopped reply is not spoken when the voice arrives");
    check(!controller.speaking(), "nothing is left playing");
}

// A message sent while a reply is still speaking is a new response, and it takes
// its own reading of the dials. It used to inherit the interrupted response's
// persona, because the reading was only taken once and nothing cleared it before
// the next one started -- so a back-and-forth at speed spoke every reply at the
// first reply's pace.
void testAReplySentMidSpeechTakesItsOwnWarmth() {
    std::cout << "a reply sent mid-speech takes its own warmth\n";

    kestrel::app::AppController controller;
    useImmediateModel(controller);
    controller.setIdleLoopEnabled(false);
    auto speech = std::make_unique<FakeSpeechBackend>();
    FakeSpeechBackend* observed = speech.get();
    speech->setTimerContext(&controller);
    controller.setSpeechBackendForTesting(std::move(speech));

    kestrel::core::PersonaState state;
    state.warmth = 0.9F;
    controller.setPersonaStateForTesting(state);
    controller.sendMessage(QStringLiteral("the first question"));
    QEventLoop settle;
    QTimer::singleShot(600, &settle, &QEventLoop::quit);
    settle.exec();
    check(observed->voiceApplications > 0, "the first reply was given the warm reading");
    check(qAbs(observed->lastWarmth - 0.9F) < 0.001F, "the engine was handed 0.9");

    // The user answers before the first reply has finished being spoken.
    state.warmth = 0.2F;
    controller.setPersonaStateForTesting(state);
    controller.sendMessage(QStringLiteral("the second question, sent early"));
    QEventLoop second;
    QTimer::singleShot(600, &second, &QEventLoop::quit);
    second.exec();
    if (controller.speaking() && observed->speakingNow()) {
        observed->finishUtterance();
        QCoreApplication::processEvents();
    }

    check(qAbs(observed->lastWarmth - 0.2F) < 0.001F,
          "the new response reads the dials for itself, not the interrupted one's");
    controller.stopGeneration();
}

// A held reply is waiting on a model that may never arrive. Half-synced weights,
// a wedged interpreter, a load that runs out of memory: in each of those the
// process is up and nothing ever says ready, so without a deadline the reply
// waits forever -- text on screen, no audio, no reason given. The deadline has
// to end it as text, say specifically what happened, and leave the app able to
// speak again once a voice does turn up.
void testAHeldReplyGivesUpIfTheVoiceNeverArrives() {
    std::cout << "a held reply gives up if the voice never arrives\n";

    kestrel::app::AppController controller;
    useImmediateModel(controller);
    controller.setIdleLoopEnabled(false);
    auto speech = std::make_unique<FakeSpeechBackend>();
    FakeSpeechBackend* observed = speech.get();
    speech->setTimerContext(&controller);
    speech->setAutoFinish(true);
    speech->startLoading();
    controller.setSpeechBackendForTesting(std::move(speech));
    // The real deadline is sized against a measured load; shortened here so the
    // test is not a minute long.
    controller.setVoiceLoadTimeoutForTesting(150);

    controller.sendMessage(QStringLiteral("a question the voice will never answer"));
    QEventLoop held;
    QTimer::singleShot(80, &held, &QEventLoop::quit);
    held.exec();
    check(observed->spoken.isEmpty(), "the reply is held while the engine is loading");
    check(!controller.ttsError().contains(QLatin1String("given up"), Qt::CaseInsensitive),
          "nothing has given up yet");

    // Past the deadline, with the engine still silent.
    QEventLoop expired;
    QTimer::singleShot(500, &expired, &QEventLoop::quit);
    expired.exec();

    check(controller.ttsError().contains(QLatin1String("given up"), Qt::CaseInsensitive),
          "the app says the voice gave up rather than failing silently");
    check(controller.ttsError().contains(QLatin1String("model"), Qt::CaseInsensitive),
          "the reason says what the voice was doing, not merely that it is loading");
    check(!controller.generating(), "the turn is finished rather than left waiting");
    check(!controller.speaking(), "nothing is claimed to be speaking");

    // A second reply must not be held all over again for an engine that has
    // already let us down once.
    controller.sendMessage(QStringLiteral("a second question, to a voice that gave up"));
    QEventLoop second;
    QTimer::singleShot(300, &second, &QEventLoop::quit);
    second.exec();
    observed->becomeReady();
    QEventLoop arrived;
    QTimer::singleShot(400, &arrived, &QEventLoop::quit);
    arrived.exec();
    check(observed->spoken.isEmpty(),
          "a reply after the give-up is not held and then spoken out of the blue");

    // And once a voice really is there, the app speaks again.
    controller.sendMessage(QStringLiteral("a third question, to a working voice"));
    QEventLoop third;
    QTimer::singleShot(600, &third, &QEventLoop::quit);
    third.exec();
    check(!observed->spoken.isEmpty(), "a later reply speaks normally once the voice is up");
    check(!controller.ttsError().contains(QLatin1String("given up"), Qt::CaseInsensitive),
          "the stale reason is cleared when the voice comes back");
    controller.stopGeneration();
}

// The pace a clause was synthesised at has to be the pace of the persona its
// response was given, and that has to be established where the audio is asked
// for rather than once when playback happened to begin. The engine holds the
// pace it was last handed, and audio made under a pace this response was never
// given cannot be corrected when it plays: a prefetched clause is made seconds
// before it is heard, and an engine rebuilt mid-reply knows nothing at all.
void testPaceIsCommittedWhereTheAudioIsAskedFor() {
    std::cout << "the response pace is committed where audio is asked for\n";

    kestrel::app::AppController controller;
    useImmediateModel(controller);
    controller.setIdleLoopEnabled(false);
    auto speech = std::make_unique<FakeSpeechBackend>();
    FakeSpeechBackend* observed = speech.get();
    speech->setTimerContext(&controller);
    controller.setSpeechBackendForTesting(std::move(speech));

    // Deliberately not the persona's own default, so a backend handed the
    // constant and a backend handed the dial are distinguishable.
    const float warm = 0.9F;
    kestrel::core::PersonaState state;
    state.warmth = warm;
    controller.setPersonaStateForTesting(state);

    controller.sendMessage(QStringLiteral("a question that takes a while to answer"));
    QEventLoop settle;
    QTimer::singleShot(600, &settle, &QEventLoop::quit);
    settle.exec();

    check(!observed->speakSpeeds.isEmpty(), "clauses of the reply were made");
    if (observed->speakSpeeds.isEmpty()) {
        controller.stopGeneration();
        return;
    }
    const double replyPace = observed->speakSpeeds.first();
    check(qAbs(replyPace - 1.0) >= 0.0001,
          "the reply was not made at an untouched engine's default pace");
    check(qAbs(observed->lastWarmth - warm) < 0.001F,
          "the engine was handed the persona's warmth");

    // Move the dial mid-reply, which the idle persona does on its own. The
    // response has to keep the pace it was given: a speed that shifts inside a
    // reply is the fault this is about, and re-reading the dial per clause
    // would trade that fault for a new one.
    state.warmth = 0.2F;
    controller.setPersonaStateForTesting(state);

    // The engine is rebuilt while the reply is still being spoken, as it is
    // when the voice engine is switched. The replacement has never been told
    // about this response and begins at its own default, so the clauses still
    // owed are precisely the ones that can come out at the wrong pace.
    auto replacement = std::make_unique<FakeSpeechBackend>();
    FakeSpeechBackend* swapped = replacement.get();
    replacement->setTimerContext(&controller);
    replacement->setAutoFinish(true);
    controller.setSpeechBackendForTesting(std::move(replacement));
    // The clause the old engine was holding went with it, so no end-of-utterance
    // report is coming from there. Let the pump move on to the new engine.
    swapped->finishUtterance();

    for (int i = 0; i < 400 && controller.speaking(); ++i) {
        QEventLoop wait;
        QTimer::singleShot(20, &wait, &QEventLoop::quit);
        wait.exec();
    }

    check(swapped->speakSpeeds.size() >= 1, "the rest of the reply reached the new engine");
    check(swapped->voiceApplications > 0,
          "the response's persona reached the engine that spoke its remainder");
    check(qAbs(swapped->lastWarmth - warm) < 0.001F,
          "the new engine kept the warmth the response was given, not the moved dial");

    // Every clause of the remainder, and every clause asked for early, was made
    // at the pace the response was assigned.
    bool uniform = !swapped->speakSpeeds.isEmpty();
    for (const double pace : swapped->speakSpeeds) {
        uniform = uniform && qAbs(pace - replyPace) < 0.0001;
    }
    for (const double pace : swapped->prefetchSpeeds) {
        uniform = uniform && qAbs(pace - replyPace) < 0.0001;
    }
    check(uniform, "the remainder was synthesised at the response's own pace");
    check(!swapped->prefetchSpeeds.isEmpty(), "clauses were still asked for early");
    controller.stopGeneration();
}

// A user who starts typing mid-answer must cut the audio off at a clause
// boundary rather than mid-word, and the new turn must take over cleanly.
void testBargeInStopsAudioAtAClauseBoundary() {
    std::cout << "barge-in stops audio at a clause boundary\n";

    kestrel::app::AppController controller;
    controller.setIdleLoopEnabled(false);
    // Slow enough that the turn is still running when the second message lands.
    auto backend = std::make_unique<SlowBackend>(400, 1);
    controller.setBackendForTesting(std::move(backend));
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
    check(observed->spoken.size() >= 1, "the acknowledgement cue was spoken");
    check(controller.speaking(), "the replacement response waits for the old clause boundary");
    check(observed->boundaryStops == 1,
          "the replacement does not mistake the old clause completion for its own");

    // The abandoned response is replaced, not resumed: the spoken text is kept
    // for the record and the new turn owns the timeline.
    //
    // Checking the state name was no use. The disjunction had to admit
    // "generating", "speaking" and "queued" to survive a controller that had
    // already moved on, and those are three of the five states a running
    // controller reports -- so it passed whatever happened, including the
    // barge-in silently doing nothing. What is worth asserting is the thing
    // that actually matters: the clause in flight lands, and the replacing
    // response then runs to completion rather than stalling on a stopCompleted
    // that never arrives.
    observed->finishUtterance();
    // Then drive the replacing turn to its end. The fake reports the end of an
    // utterance only when told to -- that is what makes it able to express a
    // boundary stop at all -- so a turn runs out by handing it one clause
    // boundary at a time, and the loop is also the wait for the slow backend
    // to finish generating.
    for (int i = 0; i < 500 && (controller.speaking() || controller.generating()); ++i) {
        if (observed->speakingNow()) {
            observed->finishUtterance();
        }
        QEventLoop wait;
        QTimer::singleShot(20, &wait, &QEventLoop::quit);
        wait.exec();
    }
    check(!controller.speaking() && !controller.generating(),
          "the replacing response completes after a boundary stop");
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

    // The controller here is holding the preview recognizer, so sttAvailable
    // is false -- and that is the assertion. It used to be true, on the
    // reasoning that the preview is "available", and the consequence was a
    // microphone button that cannot hear the user while the app appears to be
    // listening: the preview is always available precisely because it ignores
    // the microphone. It is still reachable through startListening below,
    // which is what this test is about; it just no longer claims to be a
    // working input device.
    check(!controller.sttAvailable(),
          "the preview recognizer is not reported as a working microphone");
    check(controller.sttDetail().contains(QLatin1String("preview"), Qt::CaseInsensitive)
              || controller.sttDetail().contains(QLatin1String("script"), Qt::CaseInsensitive),
          "what is actually in use is described as a preview rather than a device");
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

    // Pin the recognizer. The app picks one for the machine it runs on -- SAPI 5
    // when there is a microphone, the scripted preview recognizer when there is
    // not -- and the tests below assert on partial words and on the timing of a
    // barge-in, which only the scripted one produces. Without this, plugging in
    // a microphone would change what the suite means.
    qputenv("KESTREL_SPEECH_INPUT", "mock");

    testListenSessionIgnoresQueuedCallbacksFromPreviousSession();
    testGenerationRunsOffCallingThread();
    testTokensStreamBeforeCompletionAndPreserveUtf8();
    testCancelBeforeWorkerStarts();
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
    testWarmthReachesTheVoice();
    testReplyOwedAudioIsSpokenWhenTheVoiceArrives();
    testReplyIsDeliveredAsTextWhenTheVoiceNeverArrives();
    testPaceIsCommittedWhereTheAudioIsAskedFor();
    testTheEngineIsAskedForClausesInTheOrderTheyAreSpoken();
    testLeavingAConversationDropsTheReplyItWasHolding();
    testStoppingDropsAReplyHeldForAVoice();
    testAReplySentMidSpeechTakesItsOwnWarmth();
    testAHeldReplyGivesUpIfTheVoiceNeverArrives();
    testSpokenPhraseTakesTheTypedPath();
    testAbandonedPhraseIsNotSubmitted();

    std::cout << (failures == 0 ? "\napp tests passed\n" : "\napp tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
