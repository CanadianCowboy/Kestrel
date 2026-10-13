// Tests for the asynchronous generation path.
//
// The threading contract is the risky part of this feature, so these tests
// exercise the real thing: a backend generating on a worker thread, tokens
// crossing to the test's thread through queued signals, and cancellation
// taking effect while generation is still in flight.

#include <QCoreApplication>
#include <QDataStream>
#include <QFile>
#include <QProcess>
#include <cmath>
#include <cstring>
#include <QGuiApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
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
#include "storage/profile.h"
#include "storage/secretstore.h"

#include <memory>
#include <optional>
#include "ui/layoutaudit.h"
#include "app/generationworker.h"
#include "app/messagemodel.h"
#include "app/privilegedcommand.h"
#include "app/listensession.h"
#include "app/speechsynthesizer.h"
#include "app/echocancelsession.h"
#if KESTREL_HAS_QT_MULTIMEDIA
#include "app/localmodelspeechbackend.h"
#include <QMediaPlayer>
#endif
#include "runtime/mockbackend.h"
#include "runtime/llamacppbackend.h"
#include "runtime/modelbackend.h"
#if defined(__linux__)
#include "runtime/linuxspeechrecognizer.h"
#endif

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
    explicit SlowBackend(int tokenCount, int delayMs, BackendKind kind = BackendKind::Mock)
        : m_tokenCount(tokenCount), m_delayMs(delayMs), m_kind(kind) {}

    [[nodiscard]] BackendKind kind() const noexcept override { return m_kind; }

    [[nodiscard]] kestrel::runtime::RuntimeStatus status() const override {
        return {true, true, "Slow test backend", "test", "unit test", 0.0, 0, 4096};
    }

    bool loadModel(const std::string&, std::string&) override { return true; }

    void setTokenPieces(std::vector<std::string> pieces) { m_pieces = std::move(pieces); }
    void setSummaryResult(std::string text, bool fail = false, int delay = 0) {
        m_summaryText = std::move(text);
        m_summaryFails = fail;
        m_summaryDelay = delay;
    }

    void generate(const GenerationRequest& request,
                  kestrel::runtime::TokenCallback onToken,
                  kestrel::runtime::CompletionCallback onComplete) override {
        {
            std::lock_guard<std::mutex> lock(m_requestsMutex);
            m_requests.push_back(request);
        }
        m_generateThread = std::this_thread::get_id();
        m_cancelled.store(false, std::memory_order_release);
        if (!request.messages.empty() && request.messages.back().content.find("<remaining-answer>") != std::string::npos) {
            for (int i = 0; i < m_summaryDelay && !m_cancelled.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (m_cancelled.load()) { onComplete(false, "Generation stopped"); return; }
            if (!m_summaryFails) { onToken(m_summaryText); }
            onComplete(!m_summaryFails, m_summaryFails ? "deliberate test summary failure" : "");
            return;
        }
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
    BackendKind m_kind;
    std::string m_summaryText = "Only the remaining detail, in brief.";
    bool m_summaryFails = false;
    int m_summaryDelay = 0;
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

    void emitFinal(std::size_t session, std::string text) {
        kestrel::runtime::RecognitionResult result;
        result.text = std::move(text);
        result.isFinal = true;
        m_listening = false;
        m_results.at(session)(result);
        m_ends.at(session)(kestrel::runtime::RecognitionEnd::Silence, {});
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

    void stopImmediately() override {
        ++immediateStops;
        m_speaking = false;
    }

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

void testRuntimeDiagnosticsIdentifyTheActiveBackend() {
    std::cout << "runtime diagnostics distinguish the active backend from alternatives\n";
    AppController controller;
    const QVariantList diagnostics = controller.runtimeDiagnostics();
    bool sawActive = false;
    bool sawMockBackend = false;
    for (const QVariant& row : diagnostics) {
        const QVariantMap item = row.toMap();
        const QString label = item.value(QStringLiteral("label")).toString();
        if (label.startsWith(QStringLiteral("Active backend: "))) {
            sawActive = true;
            check(label != QStringLiteral("Active backend: mock"),
                  "a fresh controller selects a real inference backend, never preview mode");
            check(item.value(QStringLiteral("active")).toBool(),
                  "the active backend row is flagged active");
        }
        if (label.contains(QStringLiteral("mock"), Qt::CaseInsensitive)
            || item.value(QStringLiteral("value")).toString().contains(QStringLiteral("preview"), Qt::CaseInsensitive)) {
            sawMockBackend = true;
        }
    }
    check(sawActive, "diagnostics contain one explicitly named active backend");
    check(!sawMockBackend, "Mock is absent from user-facing runtime diagnostics");
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
    check(!controller.idlePrewarmEnabled(), "prewarming requires an explicit opt-in");
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
    check(controller.acknowledgement().isEmpty(),
          "the reply starts without a stock acknowledgement cue");
    check(controller.presenceState() == QStringLiteral("thinking"),
          "presence reports the turn rather than the keystroke");
    check(controller.presenceIntensity() >= 0.0 && controller.presenceIntensity() <= 1.0,
          "the animated intensity stays in range");
    check(controller.sessionTopic().contains(QStringLiteral("tensorrt")),
          "the session keeps what the conversation is about");

    runTurnToCompletion(controller);

    check(!controller.generating(), "the turn finished");
    check(controller.acknowledgement().isEmpty(), "no stock cue is staged at any point");
    check(controller.presenceState() == QStringLiteral("standing by"),
          "presence settles back to standing by");
    check(controller.statusWhisper() == QString::fromUtf8("Standing by\u2026"),
          "a short answer does not append an automatic closing line");
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
    check(controller.statusWhisper().startsWith(QStringLiteral("I can continue with the remaining detail")),
          "a long answer offers a useful continuation without a stock question");
}

// The whole point of speaking clause by clause: the answer is spoken in order,
// and the response is only delivered once the audio is.
// The regression this file exists to keep: a turn must not be declared
// delivered until the last clause has actually been spoken.
//
// The pump starts as soon as answer text arrives. An empty result before that
// means "nothing more to say right now", not "the response is over" -- and
// treating it as the second truncates the reply at whatever first tokens happen
// to exist and skips the rest of the answer.
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

    check(observed->spoken.isEmpty(), "no acknowledgement is spoken before the answer exists");
    check(controller.generating(), "the model is still working before answer text exists");
    check(controller.speaking(), "the turn is still in the speaker's mouth");

    // There is no answer text yet, and the pump must simply wait rather than
    // conclude that the response has been delivered.
    QEventLoop beforeAnswer;
    QTimer::singleShot(150, &beforeAnswer, &QEventLoop::quit);
    beforeAnswer.exec();

    check(controller.generating(), "the model is still working before the reply");
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
    for (const QString& utterance : observed->spoken) {
        spokenAnswer += utterance;
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
// Two controllers can be opened on one profile through this, and a switch makes
// every save fail so the undo path can be tested.
class SharedMemoryStore final : public kestrel::storage::SecretStore {
public:
    SharedMemoryStore(kestrel::storage::InMemorySecretStore& inner, bool* failSeals)
        : m_inner(inner), m_failSeals(failSeals) {}
    std::string description() const override { return m_inner.description(); }
    bool available(std::string& reason) const override { return m_inner.available(reason); }
    bool seal(std::string_view account, std::string_view service,
              const std::vector<std::uint8_t>& blob, std::string& error) override {
        if (*m_failSeals) {
            error = "the keystore refused the write";
            return false;
        }
        return m_inner.seal(account, service, blob, error);
    }
    std::optional<std::vector<std::uint8_t>> sealed(std::string_view account,
                                                    std::string_view service,
                                                    std::string& error) override {
        return m_inner.sealed(account, service, error);
    }
    bool forget(std::string_view account, std::string_view service, std::string& error) override {
        return m_inner.forget(account, service, error);
    }

private:
    kestrel::storage::InMemorySecretStore& m_inner;
    bool* m_failSeals;
};

std::unique_ptr<kestrel::storage::SecretStore> storeOn(kestrel::storage::InMemorySecretStore& keystore,
                                                       bool* failSeals) {
    return std::make_unique<SharedMemoryStore>(keystore, failSeals);
}

void testRecallIsOffUntilTurnedOnAndPersists() {
    std::cout << "recall is off until turned on, and what it keeps survives a restart\n";
    kestrel::storage::InMemorySecretStore keystore;
    bool failSeals = false;
    {
        kestrel::app::AppController controller;
        controller.setProfileStore(storeOn(keystore, &failSeals));
        check(!controller.recallEnabled(), "recall starts off");
        check(!controller.rememberFact(QStringLiteral("Prefers metric units.")),
              "nothing is remembered while recall is off");
        controller.setRecallEnabled(true);
        check(controller.recallEnabled(), "turning recall on takes effect");
        check(controller.rememberFact(QStringLiteral("Prefers metric units.")),
              "a fact is remembered once recall is on");
        check(controller.recallMemories().size() == 1, "and it is listed");
    }
    kestrel::app::AppController reopened;
    reopened.setProfileStore(storeOn(keystore, &failSeals));
    check(reopened.recallEnabled(), "recall is still on after a restart");
    const QVariantList memories = reopened.recallMemories();
    check(memories.size() == 1, "the fact survives a restart");
    if (memories.size() == 1) {
        const QVariantMap entry = memories.first().toMap();
        check(entry.value(QStringLiteral("text")).toString() == QStringLiteral("Prefers metric units."),
              "with its text intact");
        check(entry.value(QStringLiteral("kind")).toString() == QStringLiteral("fact"), "and its kind");
    }
}

void testRecallWithoutAKeystoreStaysOff() {
    std::cout << "without a keystore recall cannot be turned on, and the panel says why\n";
    kestrel::app::AppController controller;
    check(!controller.recallEnabled(), "recall is off");
    check(!controller.recallError().isEmpty(), "and the reason is there from the start");
    controller.setRecallEnabled(true);
    check(!controller.recallEnabled(), "turning recall on is refused with no keystore");
}

// A change that cannot be saved must not stay on screen as if it had been kept.
void testAFailedSaveUndoesTheChange() {
    std::cout << "a change that cannot be saved is undone, and the panel says why\n";
    kestrel::storage::InMemorySecretStore keystore;
    bool failSeals = false;
    kestrel::app::AppController controller;
    controller.setProfileStore(storeOn(keystore, &failSeals));
    controller.setRecallEnabled(true);
    check(controller.rememberFact(QStringLiteral("kept")), "the first fact is saved");
    failSeals = true;
    check(!controller.rememberFact(QStringLiteral("never saved")), "a fact that cannot be saved is refused");
    check(controller.recallMemories().size() == 1, "and it is not left in the panel");
    check(!controller.recallError().isEmpty(), "with the reason shown");
    controller.setRecallEnabled(false);
    check(controller.recallEnabled(), "turning recall off is refused while saves fail");
    failSeals = false;
    controller.setRecallEnabled(false);
    check(!controller.recallEnabled(), "and it works again once saves do");
}

void testForgetAndClearAreSaved() {
    std::cout << "forgetting and clearing are saved, and clearing leaves recall on\n";
    kestrel::storage::InMemorySecretStore keystore;
    bool failSeals = false;
    {
        kestrel::app::AppController controller;
        controller.setProfileStore(storeOn(keystore, &failSeals));
        controller.setRecallEnabled(true);
        check(controller.rememberFact(QStringLiteral("one")), "first fact saved");
        check(controller.rememberFact(QStringLiteral("two")), "second fact saved");
        // Newest first, so the oldest is last.
        const qlonglong oldest = controller.recallMemories().last().toMap().value(QStringLiteral("id")).toLongLong();
        controller.forgetMemory(oldest);
        check(controller.recallMemories().size() == 1, "forgetting one removes it");
    }
    {
        kestrel::app::AppController controller;
        controller.setProfileStore(storeOn(keystore, &failSeals));
        check(controller.recallMemories().size() == 1, "the removal survives a restart");
        controller.clearMemories();
        check(controller.recallMemories().isEmpty(), "clearing removes everything");
    }
    kestrel::app::AppController reopened;
    reopened.setProfileStore(storeOn(keystore, &failSeals));
    check(reopened.recallMemories().isEmpty(), "the clear survives a restart");
    check(reopened.recallEnabled(), "and recall is still on");
}

void testBlankMemoryIsRefusedWithAReason() {
    std::cout << "a blank memory is refused, and the panel says why\n";
    kestrel::storage::InMemorySecretStore keystore;
    bool failSeals = false;
    kestrel::app::AppController controller;
    controller.setProfileStore(storeOn(keystore, &failSeals));
    controller.setRecallEnabled(true);
    check(!controller.rememberFact(QStringLiteral("   ")), "blank text is not remembered");
    check(controller.recallError().contains(QStringLiteral("characters")), "and the reason is shown");
    check(controller.recallMemories().isEmpty(), "nothing is stored");
}

// A profile that cannot be read is reported and left exactly as it is. Recall
// must not write over it, because that would destroy what the user had.
void testAnUnreadableProfileIsNeverOverwritten() {
    std::cout << "an unreadable profile is reported and never overwritten\n";
    kestrel::storage::InMemorySecretStore keystore;
    bool failSeals = false;
    const std::string garbage = "{ this is not a profile";
    std::string error;
    const std::vector<std::uint8_t> bytes(garbage.begin(), garbage.end());
    check(keystore.seal(kestrel::storage::Profile::kProfileAccount, kestrel::storage::kProfileService,
                        bytes, error),
          "the test profile is seeded");
    kestrel::app::AppController controller;
    controller.setProfileStore(storeOn(keystore, &failSeals));
    check(!controller.recallError().isEmpty(), "the unreadable profile is reported");
    check(!controller.recallEnabled(), "and recall stays off");
    controller.setRecallEnabled(true);
    check(!controller.recallEnabled(), "turning recall on is refused rather than writing over the profile");
    const auto stored = keystore.sealed(kestrel::storage::Profile::kProfileAccount,
                                        kestrel::storage::kProfileService, error);
    check(stored.has_value() && std::string(stored->begin(), stored->end()) == garbage,
          "and the bytes on disk are unchanged");
}

// The model is told what the user asked it to remember, in the user's turn. The
// transcript the user reads is unchanged: it shows only what they typed.
void testRecalledFactReachesTheModelButNotTheTranscript() {
    std::cout << "a remembered fact reaches the model, not the transcript\n";
    kestrel::storage::InMemorySecretStore keystore;
    bool failSeals = false;
    kestrel::app::AppController controller;
    controller.setProfileStore(storeOn(keystore, &failSeals));
    controller.setRecallEnabled(true);
    check(controller.rememberFact(QStringLiteral("Prefers metric units.")), "a fact is remembered");

    auto backend = std::make_unique<SlowBackend>(3, 0);
    SlowBackend* observed = backend.get();
    controller.setBackendForTesting(std::move(backend));
    controller.sendMessage(QStringLiteral("How far is it to the coast?"));
    runTurnToCompletion(controller);

    const auto requests = observed->requests();
    check(!requests.empty(), "the model is asked");
    if (!requests.empty() && !requests.front().messages.empty()) {
        const auto& turn = requests.front().messages.back();
        check(turn.role == kestrel::runtime::Role::User, "the newest message is the user's");
        check(turn.content.find("Prefers metric units.") != std::string::npos,
              "the model is told the remembered fact");
        check(turn.content.find("How far is it to the coast?") != std::string::npos,
              "and it still sees the question");
    }

    kestrel::app::MessageModel* messages = controller.messages();
    check(messages->rowCount() >= 1, "the transcript has the user's message");
    if (messages->rowCount() >= 1) {
        const QString shown = messages->data(messages->index(0, 0),
                                             kestrel::app::MessageModel::ContentRole).toString();
        check(shown == QStringLiteral("How far is it to the coast?"),
              "the transcript shows only what the user typed");
    }
}

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
    // The real answer starts directly; let the event loop turn rather than
    // assuming an instant transition.
    QEventLoop settle;
    QTimer::singleShot(600, &settle, &QEventLoop::quit);
    settle.exec();

    check(controller.ttsAvailable(), "audio is still available after the request");
    check(!observed->spoken.isEmpty(), "something was spoken");
    check(observed->voiceApplications > 0, "the voice persona was applied to the engine");

    // The first utterance is part of the model's answer, not a stock cue.
    check(controller.acknowledgement().isEmpty(), "no generic voice cue is pending");
    check(!observed->spoken.isEmpty() && observed->spoken.first().endsWith(QLatin1Char('.')),
          "speech begins with a clause from the answer");
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

    // Every utterance belongs to the answer, in order and in full. Whitespace
    // between clauses is deliberately not spoken -- it is the pause -- so both
    // sides are compared with spacing removed.
    const auto withoutSpace = [](QString text) {
        text.remove(QRegularExpression(QStringLiteral("\\s+")));
        return text;
    };
    QString spokenAnswer;
    for (const QString& utterance : observed->spoken) {
        spokenAnswer += utterance;
    }
    const QString delivered = controller.messages()->data(controller.messages()->index(1, 0),
        kestrel::app::MessageModel::ContentRole).toString();
    check(!spokenAnswer.isEmpty(), "the answer was spoken");
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

// A loading voice must not delay delivery or suddenly speak an earlier reply.
void testLoadingVoiceLeavesTheCurrentReplySilent() {
    std::cout << "a loading voice leaves the current reply silent\n";

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
    // There is no acknowledgement to remove from the spoken answer.
    const QString cue = control.acknowledgement();
    check(cue.isEmpty(), "there is no stock cue before speech");
    for (int i = 0; i < 400 && control.speaking(); ++i) {
        QEventLoop wait;
        QTimer::singleShot(20, &wait, &QEventLoop::quit);
        wait.exec();
    }
    check(!controlVoice->spoken.isEmpty(), "the control reply was spoken");
    if (controlVoice->spoken.isEmpty()) {
        return;
    }
    check(control.voiceState() == QStringLiteral("completed"),
          "a ready voice completes delivery after playback");

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

    check(observed->spoken.isEmpty(), "becoming ready never starts the earlier silent reply");
    check(controller.voiceState() == QStringLiteral("completed"),
          "the loading reply is fully delivered as text");
    check(!controller.speaking(), "the completed silent reply stays silent");
    controller.sendMessage(QStringLiteral("a new question after the voice is ready"));
    runTurnToCompletion(controller);
    check(!observed->spoken.isEmpty(), "the next reply automatically uses the ready voice");
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
    const QString cue = controller.acknowledgement();
    check(cue.isEmpty(), "the first spoken clause has no preceding cue");

    // The first clause remains parked behind its natural lead-in when
    // sendMessage returns. A fix that lets lookahead through early would show
    // up as speech before the event loop has turned.
    check(observed->spoken.isEmpty(), "the first clause waits for its lead-in");

    for (int i = 0; i < 400 && controller.speaking(); ++i) {
        QEventLoop wait;
        QTimer::singleShot(20, &wait, &QEventLoop::quit);
        wait.exec();
    }

    // The clauses, in the order they are spoken, which is the order they are
    // supposed to have been asked for.
    const QStringList clauses = observed->spoken;
    const QStringList asked = observed->requests;

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
void testLoadingVoiceReportsTextOnlyWithoutWaiting() {
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

    check(controller.ttsError().contains(QLatin1String("loading"), Qt::CaseInsensitive),
          "the voice still truthfully reports that it is loading");
    check(!controller.ttsError().contains(QLatin1String("given up"), Qt::CaseInsensitive),
          "a silent reply does not invent a model-loading failure");
    check(controller.voiceState() == QStringLiteral("completed"),
          "text-only delivery completes without waiting on the voice deadline");
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

void testAudioControlsDoNotCancelGeneration() {
    std::cout << "audio controls preserve streaming text\n";
    AppController controller;
    controller.setIdleLoopEnabled(false);
    auto model = std::make_unique<SlowBackend>(80, 10);
    auto* generated = model.get();
    model->setTokenPieces({"First sentence. ", "Second sentence. "});
    controller.setBackendForTesting(std::move(model));
    auto speech = std::make_unique<FakeSpeechBackend>();
    auto* voice = speech.get();
    controller.setSpeechBackendForTesting(std::move(speech));
    const auto wait = [](int ms) {
        QEventLoop loop;
        QTimer::singleShot(ms, &loop, &QEventLoop::quit);
        loop.exec();
    };
    controller.sendMessage(QStringLiteral("stream this"));
    wait(160);
    check(controller.generating() && !voice->spoken.isEmpty(),
          "the first complete sentence is spoken before generation ends");
    controller.pauseSpeech();
    check(controller.audioPaused() && controller.generating(),
          "audio pause leaves generation running");
    check(!controller.canResumeSpeech(), "resume waits for the in-flight clause to finish");
    voice->finishUtterance();
    const auto count = voice->spoken.size();
    const int before = generated->tokensEmitted();
    wait(80);
    check(voice->spoken.size() == count && generated->tokensEmitted() > before,
          "paused audio stays quiet while tokens keep arriving");
    check(controller.canResumeSpeech(), "completed clause enables audio resume");
    check(controller.speechProgress() > 0.0 && controller.speechProgress() < 1.0,
          "source-text progress advances only after a consumed clause");
    controller.resumeSpeech();
    wait(340);
    check(voice->spoken.size() == count + 1, "resume speaks the next clause exactly once");
    const auto skippedCount = voice->spoken.size();
    controller.skipSpeech();
    wait(340);
    check(voice->immediateStops == 1 && voice->spoken.size() == skippedCount + 1,
          "skip stops the current clause and starts the following one");
    const auto stoppedCount = voice->spoken.size();
    controller.stopSpeech();
    check(!controller.speaking() && !controller.audioPaused(), "stop audio clears playback state");
    runTurnToCompletion(controller);
    check(voice->spoken.size() == stoppedCount, "stopped audio never restarts during the reply");
    check(generated->tokensEmitted() == 80, "stop audio does not cancel any generated tokens");
    check(controller.voiceState() == QStringLiteral("completed"), "the full written reply completes silently");

    // Audio paused through the end of generation remains resumable.
    controller.sendMessage(QStringLiteral("pause until generation is done"));
    wait(160);
    controller.pauseSpeech();
    voice->finishUtterance();
    runTurnToCompletion(controller);
    check(controller.canResumeSpeech(), "audio can resume after text generation finishes");
    voice->setTimerContext(&controller);
    voice->setAutoFinish(true);
    controller.resumeSpeech();
    // Avoid waiting for dozens of deliberately paced clauses in this test.
    wait(340);
    check(controller.speaking(), "resumed audio is live after generation has completed");
    controller.stopGeneration();
    check(!controller.canResumeSpeech(), "explicit cancellation drops paused audio");
}

void testRecognizedAcknowledgementAndHeldInterruption() {
    std::cout << "recognized backchannels continue and real interruptions are held\n";
    AppController controller;
    controller.setIdleLoopEnabled(false);
    auto model = std::make_unique<SlowBackend>(100, 10);
    model->setTokenPieces({"A real sentence. "});
    controller.setBackendForTesting(std::move(model));
    auto speech = std::make_unique<FakeSpeechBackend>();
    auto* voice = speech.get();
    controller.setSpeechBackendForTesting(std::move(speech));
    auto recognizer = std::make_unique<QueuedFakeRecognizer>();
    auto* ear = recognizer.get();
    controller.setSpeechRecognizerForTesting(std::move(recognizer));
    const auto wait = [](int ms) {
        QEventLoop loop;
        QTimer::singleShot(ms, &loop, &QEventLoop::quit);
        loop.exec();
    };
    controller.sendMessage(QStringLiteral("first question"));
    wait(170);
    check(controller.startListening(), "a real test recognizer listens during playback");
    ear->emitFinal(0, "okay");
    wait(20);
    check(controller.messages()->rowCount() == 2 && controller.generating(),
          "a backchannel adds no turn and does not stop generation");
    check(voice->boundaryStops == 0 && controller.pendingInterruption().isEmpty(),
          "an acknowledgment does not interrupt the voice");
    check(controller.startListening(), "a second phrase can be captured");
    ear->emitFinal(1, "Actually explain the other approach");
    wait(40);
    check(controller.pendingInterruption() == QStringLiteral("Actually explain the other approach"),
          "a real request is held visibly for confirmation");
    check(controller.messages()->rowCount() == 2, "holding the request does not replace the old answer");
    check(voice->boundaryStops == 1, "a held interruption stops at the clause boundary");
    voice->finishUtterance();
    wait(20);
    check(controller.canResume(), "the old answer remains resumable");
    controller.resumeInterruptedAnswer();
    wait(30);
    check(controller.pendingInterruption().isEmpty() && controller.generating(),
          "resuming dismisses the pending request and continues the old answer");
    check(controller.startListening(), "an interruption can be captured after resuming");
    ear->emitFinal(2, "Replace this with a new answer");
    wait(40);
    if (voice->speakingNow()) { voice->finishUtterance(); }
    wait(20);
    controller.sendPendingInterruption();
    wait(30);
    check(controller.messages()->rowCount() == 4 && controller.pendingInterruption().isEmpty(),
          "confirming a held request creates one replacement turn and clears its draft");
    controller.stopGeneration();
}

void testSpokenPlaybackCommands() {
    AppController controller;
    controller.setIdleLoopEnabled(false);
    auto model = std::make_unique<SlowBackend>(100, 15);
    auto* backend = model.get();
    model->setTokenPieces({"An answer clause. "});
    controller.setBackendForTesting(std::move(model));
    auto speech = std::make_unique<FakeSpeechBackend>();
    auto* voice = speech.get();
    controller.setSpeechBackendForTesting(std::move(speech));
    auto recognizer = std::make_unique<QueuedFakeRecognizer>();
    auto* ear = recognizer.get();
    controller.setSpeechRecognizerForTesting(std::move(recognizer));
    const auto wait = [](int ms) { QEventLoop loop; QTimer::singleShot(ms, &loop, &QEventLoop::quit); loop.exec(); };
    controller.sendMessage(QStringLiteral("answer this"));
    wait(180);
    check(controller.startListening(), "pause command capture starts");
    ear->emitFinal(0, "Kestrel pause please");
    wait(20);
    check(controller.audioPaused() && controller.generating(), "spoken pause holds only audio, not generation");
    voice->finishUtterance();
    check(controller.startListening(), "resume command capture starts");
    ear->emitFinal(1, "resume audio");
    wait(350);
    check(!controller.audioPaused() && controller.speaking(), "spoken resume restarts audio without a new turn");
    check(controller.startListening(), "stop command capture starts");
    ear->emitFinal(2, "stop speaking");
    wait(20);
    check(!controller.speaking() && controller.generating(), "spoken stop audio leaves written generation running");
    runTurnToCompletion(controller);
    check(backend->tokensEmitted() == 100 && controller.messages()->rowCount() == 2,
          "voice controls neither truncate text nor become user messages");
    controller.sendMessage(QStringLiteral("cancel this answer"));
    wait(180);
    check(controller.startListening(), "cancel command capture starts");
    ear->emitFinal(3, "cancel generation");
    wait(40);
    check(!controller.speaking() && !controller.generating(), "explicit spoken cancellation ends both audio and generation");
    check(controller.messages()->rowCount() == 4, "cancellation adds no spurious transcript turn");
}

void testSpeechControlsThroughQml() {
    std::cout << "speech controls work through the shipped QML\n";
    AppController controller;
    controller.setIdleLoopEnabled(false);
    controller.setDiagnosticsOpen(false);
    auto model = std::make_unique<SlowBackend>(30, 10);
    auto* generated = model.get();
    model->setTokenPieces({"First **clause**. ", "Next readable clause. "});
    controller.setBackendForTesting(std::move(model));
    auto speech = std::make_unique<FakeSpeechBackend>();
    auto* voice = speech.get();
    controller.setSpeechBackendForTesting(std::move(speech));
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("appController"), &controller);
    QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(KESTREL_UI_DIR "/Main.qml")));
    std::unique_ptr<QObject> scene(component.create());
    if (!scene) { std::cout << component.errorString().toStdString(); }
    check(scene != nullptr, "the real speech control scene loads");
    if (!scene) { return; }
    auto* window = qobject_cast<QQuickWindow*>(scene.get());
    auto* pause = scene->findChild<QQuickItem*>(QStringLiteral("speechPauseButton"));
    auto* stop = scene->findChild<QQuickItem*>(QStringLiteral("speechStopButton"));
    auto* skip = scene->findChild<QQuickItem*>(QStringLiteral("speechSkipButton"));
    auto* progress = scene->findChild<QQuickItem*>(QStringLiteral("speechProgressBar"));
    check(window && pause && stop && skip && progress, "QML exposes pause, stop, skip and progress");
    if (!window || !pause || !stop || !skip || !progress) { return; }
    window->resize(980, 640);
    const auto wait = [](int ms) {
        QEventLoop loop;
        QTimer::singleShot(ms, &loop, &QEventLoop::quit);
        loop.exec();
    };
    controller.sendMessage(QStringLiteral("read this"));
    wait(160);
    check(pause->isVisible() && stop->isVisible() && skip->isVisible(),
          "playback controls are visible during live streaming speech");
    check(voice->spoken.first() == QStringLiteral("First clause."),
          "the actual speech engine receives cleaned prose, not Markdown");
    check(QMetaObject::invokeMethod(pause, "clicked"), "the QML pause action activates");
    check(controller.audioPaused() && controller.generating(), "QML pause does not cancel generation");
    voice->finishUtterance();
    wait(20);
    check(progress->property("value").toDouble() > 0.0, "QML progress reflects consumed source text");
    check(pause->property("enabled").toBool() && pause->property("text").toString() == QStringLiteral("Resume"),
          "the button changes to an enabled resume action");
    const auto pausedCount = voice->spoken.size();
    check(QMetaObject::invokeMethod(pause, "clicked"), "the same QML action resumes");
    wait(340);
    check(voice->spoken.size() == pausedCount + 1, "QML resume starts the next clause");
    check(QMetaObject::invokeMethod(skip, "clicked"), "the QML skip action activates");
    wait(340);
    check(voice->spoken.size() == pausedCount + 2, "QML skip advances playback");
    check(QMetaObject::invokeMethod(stop, "clicked"), "the QML stop-audio action activates");
    runTurnToCompletion(controller);
    check(generated->tokensEmitted() == 30 && !controller.speaking(),
          "QML stop leaves a fully generated silent written answer");
    auto problems = kestrel::ui::auditLayouts(window->contentItem());
    for (const auto& problem : problems) {
        std::cout << problem.where.toStdString() << ": " << problem.what.toStdString() << '\n';
    }
    check(problems.isEmpty(), "speech controls do not collapse or overlap at minimum desktop size");

    controller.sendMessage(QStringLiteral("another answer"));
    wait(160);
    problems = kestrel::ui::auditLayouts(window->contentItem());
    check(problems.isEmpty(), "visible playback controls fit the minimum desktop layout");
    check(QMetaObject::invokeMethod(pause, "clicked"), "audio can be paused again through QML");
    voice->finishUtterance();
    wait(340);
    check(controller.canResumeSpeech(), "finished text still has resumable audio");
    controller.stopGeneration();
    check(!controller.audioPaused() && !controller.canResumeSpeech(),
          "cancelling paused audio clears the QML resume state");
    controller.newConversation();
    controller.sendMessage(QStringLiteral("new conversation works"));
    wait(160);
    check(controller.generating() && controller.messages()->rowCount() == 2,
          "cancelling paused playback releases the timeline for a new conversation");
    controller.stopGeneration();
}

void testSpokenSummaryKeepsTheWrittenAnswer() {
    const auto run = [](bool fail) {
        AppController controller;
        controller.setIdleLoopEnabled(false);
        auto model = std::make_unique<SlowBackend>(16, 0, BackendKind::LlamaCpp);
        auto* backend = model.get();
        const std::string sentence = "This is the detailed original answer with several facts and careful qualifications to preserve for the written conversation. ";
        model->setTokenPieces({sentence});
        model->setSummaryResult("Here is the remaining detail in brief.", fail);
        controller.setBackendForTesting(std::move(model));
        auto speech = std::make_unique<FakeSpeechBackend>();
        auto* voice = speech.get();
        speech->setTimerContext(&controller);
        speech->setAutoFinish(true);
        controller.setSpeechBackendForTesting(std::move(speech));
        controller.sendMessage(QStringLiteral("Explain in detail"));
        runTurnToCompletion(controller);
        // A summary request owns the worker even after chat generation ends.
        QElapsedTimer deadline;
        deadline.start();
        while ((controller.summarizingSpeech() || controller.speaking()) && deadline.elapsed() < 15000) {
            QEventLoop step;
            QTimer::singleShot(20, &step, &QEventLoop::quit);
            step.exec();
        }
        const auto requests = backend->requests();
        check(requests.size() == 2, "a long answer uses one separate summary request");
        if (requests.size() == 2) {
            const auto& prompt = requests.back().messages.back().content;
            const auto start = prompt.find("<remaining-answer>\n");
            const auto end = prompt.find("\n</remaining-answer>");
            check(start != std::string::npos && end != std::string::npos,
                  "summary source is explicitly delimited as data");
            if (start != std::string::npos && end != std::string::npos) {
                const auto material = prompt.substr(start + 19, end - start - 19);
                check(material.size() < sentence.size() * 16 && !material.empty(),
                      "the summary receives only the unspoken remainder, not the spoken opening");
            }
        }
        QString expected;
        for (int i = 0; i < 16; ++i) { expected += QString::fromStdString(sentence); }
        const QString written = controller.messages()->data(controller.messages()->index(1, 0),
            kestrel::app::MessageModel::ContentRole).toString();
        check(written == expected, "a spoken adaptation never edits the original written answer");
        check(!voice->spoken.isEmpty() && voice->spoken.first() == QString::fromStdString(sentence).trimmed(),
              "the original opening is spoken before any summary");
        if (fail) {
            check(controller.ttsError().contains(QStringLiteral("deliberate test")),
                  "summary failure is reported, not swallowed");
            check(voice->spoken.size() == 16, "failed summary falls back to every remaining original clause");
        } else {
            check(voice->spoken.contains(QStringLiteral("Here is the remaining detail in brief.")),
                  "the generated summary reaches the speech backend");
            check(voice->spoken.size() < 16, "summary replaces, rather than repeats, the original remainder");
        }
        check(!controller.summarizingSpeech() && !controller.speaking(), "summary playback and worker both drain");
        controller.stopGeneration();
    };
    run(false);
    run(true);
}

void testRealModelSpokenSummaryWhenRequested() {
    const QString path = qEnvironmentVariable("KESTREL_TEST_SPEECH_GGUF");
    if (path.isEmpty()) { return; }
    AppController controller;
    controller.setIdleLoopEnabled(false);
    auto speech = std::make_unique<FakeSpeechBackend>();
    auto* voice = speech.get();
    speech->setTimerContext(&controller);
    // Hold the first fake utterance until generation finishes. A fake voice that
    // auto-finishes immediately outruns a real CPU model and drains the whole
    // answer before there is an unspoken backlog to summarize; that tests the
    // fake playback speed rather than the summary path.
    speech->setAutoFinish(false);
    controller.setSpeechBackendForTesting(std::move(speech));
    bool loaded = false;
    QObject::connect(&controller, &AppController::modelLoadFinished, &controller, [&] { loaded = true; });
    controller.loadModelFromUrl(QUrl::fromLocalFile(path).toString());
    const auto wait = [](int ms) { QEventLoop loop; QTimer::singleShot(ms, &loop, &QEventLoop::quit); loop.exec(); };
    QElapsedTimer clock;
    clock.start();
    while (!loaded && clock.elapsed() < 30000) { wait(20); }
    check(loaded && controller.modelError().isEmpty() && !controller.modelPath().isEmpty(),
          "real GGUF loads through the controller for spoken-summary verification");
    if (!loaded || !controller.modelError().isEmpty()) { return; }
    controller.sendMessage(QStringLiteral(
        "Write a detailed explanation in six paragraphs, about 350 words, of why keeping personal conversations on a local computer can improve privacy. "
        "Explain offline processing, file permissions, encryption, backups, the risks of local malware, and the need to update software. "
        "Use plain prose only, no headings or bullet lists. Do not shorten the answer."));
    bool streamed = false;
    clock.restart();
    while (controller.generating() && clock.elapsed() < 180000) {
        streamed = streamed || !voice->spoken.isEmpty();
        wait(20);
    }
    check(!controller.generating(), "real-model written generation completes within the integration deadline");
    const QString written = controller.messages()->data(controller.messages()->index(1, 0),
        kestrel::app::MessageModel::ContentRole).toString();
    // Generation has now completed while the first clause remains in flight.
    // Finish clauses at explicit boundaries until the real controller launches
    // its summary, then drain the summary through the same playback interface.
    while (!controller.summarizingSpeech() && clock.elapsed() < 240000) {
        if (voice->speakingNow()) { voice->finishUtterance(); }
        else { wait(10); }
    }
    while ((controller.summarizingSpeech() || controller.speaking()) && clock.elapsed() < 240000) {
        if (voice->speakingNow()) { voice->finishUtterance(); }
        else { wait(10); }
    }
    std::cout << "  real spoken-summary measurement: written chars=" << written.size()
              << " spoken clauses=" << voice->spoken.size() << " elapsed=" << clock.elapsed() << " ms\n"
              << "  spoken adaptation: " << controller.spokenSummary().toStdString() << '\n';
    check(streamed, "the real reply opening was spoken while its remaining text was generating");
    check(!controller.spokenSummary().trimmed().isEmpty(), "the installed model produces a separate spoken summary");
    check(controller.messages()->data(controller.messages()->index(1, 0),
          kestrel::app::MessageModel::ContentRole).toString() == written,
          "real summary generation leaves the written reply unchanged");
    check(!controller.summarizingSpeech() && !controller.speaking(), "real summary synthesis/playback drains");
    check(controller.ttsError().isEmpty(), "the real summary path reports no speech/summary failure");
    controller.stopGeneration();
}

void testSummaryCancellationAndQmlControls() {
    AppController controller;
    controller.setIdleLoopEnabled(false);
    controller.setDiagnosticsOpen(true);
    auto model = std::make_unique<SlowBackend>(18, 0, BackendKind::LlamaCpp);
    model->setTokenPieces({"A detailed written sentence that includes the facts and warnings the user needs to retain and understand correctly. "});
    model->setSummaryResult("A concise spoken remainder.", false, 1500);
    controller.setBackendForTesting(std::move(model));
    auto speech = std::make_unique<FakeSpeechBackend>();
    auto* voice = speech.get();
    speech->setTimerContext(&controller);
    speech->setAutoFinish(true);
    controller.setSpeechBackendForTesting(std::move(speech));
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("appController"), &controller);
    QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(KESTREL_UI_DIR "/Main.qml")));
    std::unique_ptr<QObject> scene(component.create());
    check(scene != nullptr, "summary and echo settings load in the actual QML");
    if (!scene) { return; }
    QFile mainQml(QStringLiteral(KESTREL_UI_DIR "/Main.qml"));
    const QByteArray qmlSource = mainQml.open(QIODevice::ReadOnly | QIODevice::Text)
        ? mainQml.readAll() : QByteArray();
    const bool previewUiRemoved = !qmlSource.isEmpty()
        && !qmlSource.contains("Preview") && !qmlSource.contains("usePreviewBackend");
    check(previewUiRemoved, "the shipped QML contains no preview button or backend action");
    auto* summary = scene->findChild<QObject*>(QStringLiteral("automaticSpeechSummaryToggle"));
    auto* echo = scene->findChild<QObject*>(QStringLiteral("echoCancellationToggle"));
    check(summary && echo, "QML exposes both voice preferences");
    if (!summary || !echo) { return; }
    const auto wait = [](int ms) { QEventLoop loop; QTimer::singleShot(ms, &loop, &QEventLoop::quit); loop.exec(); };
    check(QMetaObject::invokeMethod(summary, "toggled"), "QML summary toggle activates");
    check(!controller.automaticSpeechSummary(), "QML can disable automatic summaries");
    QMetaObject::invokeMethod(summary, "toggled");
    check(controller.automaticSpeechSummary(), "QML can enable summaries again");
    controller.sendMessage(QStringLiteral("long answer"));
    QElapsedTimer deadline;
    deadline.start();
    while (!controller.summarizingSpeech() && deadline.elapsed() < 4000) { wait(20); }
    check(controller.summarizingSpeech(), "the adaptation worker is live after the original opening");
    controller.pauseSpeech();
    const auto before = voice->spoken.size();
    wait(1800);
    check(controller.canResumeSpeech() && voice->spoken.size() == before,
          "a summary completing during audio pause is held without starting speech");
    controller.resumeSpeech();
    wait(400);
    check(voice->spoken.contains(QStringLiteral("A concise spoken remainder.")),
          "resuming held summary audio speaks the generated adaptation");
    controller.stopGeneration();

    controller.newConversation();
    controller.sendMessage(QStringLiteral("long answer to cancel"));
    deadline.restart();
    while (!controller.summarizingSpeech() && deadline.elapsed() < 4000) { wait(20); }
    check(controller.summarizingSpeech(), "another long answer starts a separate adaptation");
    controller.newConversation();
    wait(100);
    check(controller.messages()->rowCount() == 0 && !controller.summarizingSpeech() && !controller.speaking(),
          "leaving cancels and drains the summary worker before replacing the conversation");
    const auto count = voice->spoken.size();
    wait(1600);
    check(voice->spoken.size() == count, "a late canceled summary cannot speak in the new conversation");
}

void testEchoCancelSessionLifecycle() {
    kestrel::app::EchoCancelSession session;
    check(!session.active() && !session.starting(), "echo discovery does not capture at construction");
    // Deterministically exercise missing-tool fallback without opening audio.
    const QByteArray path = qgetenv("PATH");
    qputenv("PATH", "/nonexistent-kestrel-tools");
    QString error;
    check(!session.start(error), "missing PipeWire tools refuse echo cancellation");
    check(!error.isEmpty() && !session.active(), "missing tools produce actionable diagnostics");
    qputenv("PATH", path);
    if (!qEnvironmentVariableIsSet("KESTREL_TEST_ECHO_LIVE")) { return; }
    check(session.start(error), "live echo-cancel session starts asynchronously");
    QElapsedTimer deadline;
    deadline.start();
    while (session.starting() && deadline.elapsed() < 6500) {
        QEventLoop step;
        QTimer::singleShot(20, &step, &QEventLoop::quit);
        step.exec();
    }
    std::cout << "  echo route: " << session.detail().toStdString() << '\n';
    check(session.active(), "live PipeWire microphone and render-reference links are active");
    check(session.captureDevice().startsWith(QStringLiteral("pipewire:NODE=")),
          "capture is pinned to the verified clean source, not a default microphone");
    if (session.active() && qEnvironmentVariableIsSet("KESTREL_TEST_ECHO_ACOUSTIC")) {
        // Explicit opt-in: synthetic playback, microphone PCM in RAM only.
        // Compare energy, but do not call a room-specific reduction a unit-test guarantee.
        QTemporaryFile stimulus(QDir::tempPath() + QStringLiteral("/kestrel-aec-stimulus-XXXXXX.wav"));
        check(stimulus.open(), "a temporary synthetic-only stimulus can be created");
        QByteArray pcm;
        QDataStream audio(&pcm, QIODevice::WriteOnly);
        audio.setByteOrder(QDataStream::LittleEndian);
        unsigned random = 12345;
        for (int i = 0; i < 6 * 48000; ++i) {
            random = random * 1664525u + 1013904223u;
            const double noise = static_cast<double>((random >> 16) & 65535u) / 32768.0 - 1.0;
            const double envelope = 0.5 + 0.5 * std::sin(static_cast<double>(i) * 0.0007);
            audio << static_cast<qint16>(noise * envelope * 2200.0);
        }
        QDataStream wav(&stimulus);
        wav.setByteOrder(QDataStream::LittleEndian);
        wav.writeRawData("RIFF", 4); wav << static_cast<quint32>(36 + pcm.size());
        wav.writeRawData("WAVEfmt ", 8); wav << quint32(16) << quint16(1) << quint16(1)
            << quint32(48000) << quint32(96000) << quint16(2) << quint16(16);
        wav.writeRawData("data", 4); wav << static_cast<quint32>(pcm.size());
        wav.writeRawData(pcm.constData(), static_cast<int>(pcm.size()));
        stimulus.flush();
        QProcess raw, clean, playback;
        // Use the same ALSA/PipeWire route and 16 kHz PCM as the app. pw-record
        // on this host exits 1 at sample-count completion even with full data,
        // so use arecord's explicit duration rather than accepting error codes.
        const QStringList arguments{QStringLiteral("-q"), QStringLiteral("-t"), QStringLiteral("raw"),
            QStringLiteral("-f"), QStringLiteral("S16_LE"), QStringLiteral("-r"), QStringLiteral("16000"),
            QStringLiteral("-c"), QStringLiteral("1"), QStringLiteral("-d"), QStringLiteral("7"), QStringLiteral("-")};
        raw.start(QStringLiteral("arecord"), QStringList{QStringLiteral("-D"), QStringLiteral("default")} + arguments);
        clean.start(QStringLiteral("arecord"), QStringList{QStringLiteral("-D"), session.captureDevice()} + arguments);
        check(raw.waitForStarted(1000) && clean.waitForStarted(1000), "raw and echo-canceled capture start concurrently");
        QByteArray rawAudio, cleanAudio;
        QElapsedTimer elapsed;
        elapsed.start();
        bool played = false;
        while (elapsed.elapsed() < 9000 && (raw.state() != QProcess::NotRunning || clean.state() != QProcess::NotRunning)) {
            if (!played && elapsed.elapsed() >= 500) {
                playback.start(QStringLiteral("pw-play"), {QStringLiteral("--volume"), QStringLiteral("0.5"), stimulus.fileName()});
                played = true;
            }
            rawAudio += raw.readAllStandardOutput();
            cleanAudio += clean.readAllStandardOutput();
            QEventLoop step;
            QTimer::singleShot(20, &step, &QEventLoop::quit);
            step.exec();
        }
        rawAudio += raw.readAllStandardOutput();
        cleanAudio += clean.readAllStandardOutput();
        std::cout << "  capture status: raw=" << raw.exitCode() << " clean=" << clean.exitCode()
                  << " raw bytes=" << rawAudio.size() << " clean bytes=" << cleanAudio.size()
                  << " raw error=" << raw.readAllStandardError().toStdString()
                  << " clean error=" << clean.readAllStandardError().toStdString() << '\n';
        check(raw.state() == QProcess::NotRunning && raw.exitCode() == 0
              && clean.state() == QProcess::NotRunning && clean.exitCode() == 0,
              "both bounded capture processes complete without device errors");
        check(playback.state() == QProcess::NotRunning && playback.exitCode() == 0,
              "synthetic playback completes while microphone capture is running");
        for (auto* process : {&raw, &clean, &playback}) {
            if (process->state() != QProcess::NotRunning) { process->kill(); process->waitForFinished(1000); }
        }
        const auto energy = [](const QByteArray& data) {
            double sum = 0;
            const auto samples = data.size() / static_cast<qsizetype>(sizeof(qint16));
            for (qsizetype i = samples / 2; i < samples; ++i) {
                qint16 sample = 0;
                std::memcpy(&sample, data.constData() + i * static_cast<qsizetype>(sizeof(qint16)), sizeof(qint16));
                const double value = static_cast<double>(sample) / 32768.0;
                sum += value * value;
            }
            return samples > 0 ? sum / static_cast<double>(samples - samples / 2) : -1.0;
        };
        const double rawEnergy = energy(rawAudio), cleanEnergy = energy(cleanAudio);
        check(rawAudio.size() >= 16000 * 2 && cleanAudio.size() >= 16000 * 2
              && rawEnergy >= 0 && cleanEnergy >= 0, "both real capture paths deliver finite PCM without saving microphone recordings");
        if (rawEnergy > 0 && cleanEnergy > 0) {
            std::cout << "  acoustic measurement: raw RMS=" << std::sqrt(rawEnergy)
                      << " clean RMS=" << std::sqrt(cleanEnergy)
                      << " energy reduction=" << 10.0 * std::log10(rawEnergy / cleanEnergy)
                      << " dB; duration=" << elapsed.elapsed() << " ms (not isolated ERLE or double-talk certification)\n";
        }
    }
    session.stop();
    check(!session.active() && session.captureDevice().isEmpty(), "stop releases the active echo route");
#if defined(__linux__)
    // Exercise the same app/microphone interface a user activates, with a
    // bounded live recording canceled before any audio is saved or transcribed.
    const QByteArray mock = qgetenv("KESTREL_SPEECH_INPUT");
    qputenv("KESTREL_SPEECH_INPUT", "platform");
    {
        AppController controller;
        controller.setIdleLoopEnabled(false);
        useImmediateModel(controller);
        auto speech = std::make_unique<FakeSpeechBackend>();
        controller.setSpeechBackendForTesting(std::move(speech));
        const QString model = qEnvironmentVariable("KESTREL_TEST_WHISPER_MODEL");
        if (!model.isEmpty()) {
            check(controller.applyDictationSettings(model, QStringLiteral("default")),
                  "the controller selects real Whisper and default PipeWire capture");
            controller.sendMessage(QStringLiteral("keep speaking while recording"));
            QEventLoop settle; QTimer::singleShot(200, &settle, &QEventLoop::quit); settle.exec();
            check(controller.startListening(), "the app starts echo-canceled listening during speech");
            deadline.restart();
            while (!controller.echoCancellationActive() && controller.listening() && deadline.elapsed() < 6000) {
                QEventLoop step; QTimer::singleShot(20, &step, &QEventLoop::quit); step.exec();
            }
            check(controller.echoCancellationActive() && controller.listening() && controller.speaking(),
                  "the actual controller keeps speech and echo-canceled capture live simultaneously");
            QEventLoop capture; QTimer::singleShot(500, &capture, &QEventLoop::quit); capture.exec();
            controller.abandonListening();
            check(!controller.listening() && !controller.echoCancellationActive(),
                  "the user microphone-stop path disables capture and the echo route");
            check(controller.messages()->rowCount() == 2, "canceling live capture does not invent another user message");
            controller.stopGeneration();
        }
    }
    qputenv("KESTREL_SPEECH_INPUT", mock);
#endif
}

void testSendingWithoutModelReportsActionableError() {
    AppController controller;
    controller.setIdleLoopEnabled(false);
    if (controller.runtimeAvailable()) {
        return;
    }
    controller.sendMessage(QStringLiteral("do not pretend to answer"));
    check(controller.messages()->rowCount() == 0,
          "a missing local model does not create a fake conversation turn");
    check(controller.modelError().contains(QStringLiteral("Load a GGUF")),
          "a missing local model produces a clear load-model instruction");
}

void testDiagnosticsPreferencePersists() {
    QSettings settings(QStringLiteral("Kestrel"), QStringLiteral("Kestrel"));
    settings.remove(QStringLiteral("ui/diagnosticsOpen"));
    settings.sync();
    { AppController controller;
      check(controller.diagnosticsOpen(), "diagnostics start open on first launch");
      controller.setDiagnosticsOpen(false); }
    { AppController controller;
      check(!controller.diagnosticsOpen(), "closing diagnostics persists across controllers");
      controller.setDiagnosticsOpen(true); }
    { AppController controller;
      check(controller.diagnosticsOpen(), "reopening diagnostics persists too"); }
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
    auto backend = std::make_unique<SlowBackend>(8, 50);
    backend->setTokenPieces({"A complete spoken clause. "});
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
    check(observed->spoken.size() >= 1, "an actual answer clause was spoken");
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

void testNewTurnReleasesOwedAudioAndOmitsToolHistory() {
    kestrel::app::AppController controller;
    controller.setIdleLoopEnabled(false);
    auto model = std::make_unique<SlowBackend>(1, 0);
    SlowBackend* observed = model.get();
    controller.setBackendForTesting(std::move(model));
    auto speech = std::make_unique<FakeSpeechBackend>();
    speech->startLoading();
    controller.setSpeechBackendForTesting(std::move(speech));
    controller.sendMessage(QStringLiteral("first question"));
    runTurnToCompletion(controller);
    check(!controller.speaking(), "the loading voice holds the first reply without speaking");
    controller.messages()->appendMessage(kestrel::core::MessageRole::Tool,
                                         QStringLiteral("private index result"),
                                         kestrel::app::MessageStatus::Complete);
    controller.sendMessage(QStringLiteral("second question"));
    runTurnToCompletion(controller);
    const auto requests = observed->requests();
    check(requests.size() == 2, "owed audio does not block the next generation");
    if (requests.size() == 2) {
        bool hasTool = false;
        for (const auto& message : requests.back().messages) {
            hasTool = hasTool || message.role == kestrel::runtime::Role::Tool
                      || message.content == "private index result";
        }
        check(!hasTool, "tool results do not enter model history");
    }
    controller.stopGeneration();
}

void testPreviewInputRequiresExplicitOptIn() {
    // Construct the scripted recognizer deterministically, then remove consent
    // before starting it, as with the automatic preview fallback.
    qputenv("KESTREL_SPEECH_INPUT", "mock");
    kestrel::app::AppController controller;
    controller.setIdleLoopEnabled(false);
    qunsetenv("KESTREL_SPEECH_INPUT");
    check(!controller.startListening(), "preview input requires explicit mock selection");
    check(controller.listenError() == controller.sttDetail(), "refusal reports speech detail");
    check(controller.sttDetail().contains(QStringLiteral("preview"), Qt::CaseInsensitive),
          "automatic fallback is identified as preview only");
#if defined(__linux__)
    check(controller.sttDetail().contains(QStringLiteral("Whisper"), Qt::CaseInsensitive),
          "Linux dictation explains its offline Whisper setup");
#endif
    check(!controller.listening(), "refusing preview input leaves listening stopped");
    check(controller.messages()->rowCount() == 0, "refusal never invents a user message");
    qputenv("KESTREL_SPEECH_INPUT", "mock");
}

#if defined(__linux__)
void testLinuxDictationSettings() {
    QSettings settings(QStringLiteral("Kestrel"), QStringLiteral("Kestrel"));
    settings.clear();
    QTemporaryFile model(QDir::tempPath() + QStringLiteral("/Whisper model XXXXXX.bin"));
    check(model.open(), "a local model path with spaces is available");
    const QString url = QUrl::fromLocalFile(model.fileName()).toString();
    {
        AppController controller;
        check(controller.linuxDictationSettings(), "Linux exposes dictation settings even without SDKs");
        check(controller.dictationMicrophones().contains(QStringLiteral("default")), "the default ALSA route is always selectable");
        check(!controller.applyDictationSettings(QStringLiteral("https://example.com/model.bin"), QStringLiteral("default")),
              "remote model URLs are refused without downloading");
        check(!controller.applyDictationSettings(model.fileName() + QStringLiteral(".missing"), QStringLiteral("default")),
              "missing local models are refused");
        check(!controller.dictationSettingsError().isEmpty(), "invalid settings report an actionable error");
        check(controller.startListening(), "preview session starts for the active-session guard");
        check(!controller.applyDictationSettings(url, QStringLiteral("custom_capture")), "settings cannot discard an active phrase");
        check(controller.listening(), "refused settings leave the active session alone");
        controller.abandonListening();
        const QByteArray before = qgetenv("KESTREL_AUDIO_DEVICE");
        check(controller.applyDictationSettings(url, QStringLiteral("custom_capture")), "local file URLs and custom ALSA routes are accepted");
        check(controller.dictationModelPath() == model.fileName(), "percent-encoded model URLs become local paths");
        check(controller.dictationAudioDevice() == QStringLiteral("custom_capture"), "the custom microphone is selected");
        check(controller.dictationSettingsError().isEmpty(), "successful apply clears validation errors");
        check(qgetenv("KESTREL_AUDIO_DEVICE") == before, "desktop settings never mutate process-global environment");
    }
    {
        AppController restored;
        check(restored.dictationModelPath() == model.fileName(), "the model survives a new controller");
        check(restored.dictationAudioDevice() == QStringLiteral("custom_capture"), "the microphone survives a new controller");
        check(restored.dictationMicrophones().contains(QStringLiteral("custom_capture")), "a saved custom route remains selectable");
        check(restored.applyDictationSettings(QString(), QString()), "clearing the model disables dictation");
        check(!restored.sttAvailable(), "an empty model is not reported as ready");
        check(restored.dictationAudioDevice() == QStringLiteral("default"), "an empty device restores the default route");
    }
    settings.clear();
}

void testLinuxDictationSettingsDialog() {
    QSettings settings(QStringLiteral("Kestrel"), QStringLiteral("Kestrel"));
    settings.clear();
    AppController controller;
    controller.setIdleLoopEnabled(false);
    controller.setDiagnosticsOpen(true);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("appController"), &controller);
    QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(KESTREL_UI_DIR "/Main.qml")));
    std::unique_ptr<QObject> scene(component.create());
    if (!scene) { std::cout << component.errorString().toStdString(); }
    check(scene != nullptr, "the shipped Main.qml loads");
    if (!scene) { return; }
    auto* window = qobject_cast<QQuickWindow*>(scene.get());
    auto* button = scene->findChild<QQuickItem*>(QStringLiteral("dictationSettingsButton"));
    auto* dialog = scene->findChild<QObject*>(QStringLiteral("dictationSettingsDialog"));
    auto* field = scene->findChild<QObject*>(QStringLiteral("dictationModelField"));
    auto* device = scene->findChild<QObject*>(QStringLiteral("dictationDeviceField"));
    auto* apply = scene->findChild<QObject*>(QStringLiteral("dictationApplyButton"));
    check(window && button && dialog && field && device && apply, "the runtime panel exposes every dictation control");
    if (!window || !button || !dialog || !field || !device || !apply) { return; }
    QEventLoop layout;
    QTimer::singleShot(100, &layout, &QEventLoop::quit);
    layout.exec();
    check(button->isVisible() && button->width() > 0 && button->height() > 0, "the settings entry is visible and laid out");
    check(kestrel::ui::auditLayouts(window->contentItem()).isEmpty(), "adding settings does not collapse the runtime panel's other controls");
    check(QMetaObject::invokeMethod(button, "clicked"), "the runtime settings button can be activated");
    QCoreApplication::processEvents();
    check(dialog->property("visible").toBool(), "activation opens the real settings dialog");
    field->setProperty("text", QStringLiteral("/missing/whisper-model.bin"));
    check(QMetaObject::invokeMethod(apply, "clicked"), "the dialog apply action can be activated");
    check(!controller.dictationSettingsError().isEmpty(), "invalid dialog input reaches controller validation");
    QTemporaryFile model(QDir::tempPath() + QStringLiteral("/UI Whisper XXXXXX.bin"));
    check(model.open(), "a model path for the UI is available");
    field->setProperty("text", QUrl::fromLocalFile(model.fileName()).toString());
    device->setProperty("editText", QStringLiteral("ui_capture"));
    check(QMetaObject::invokeMethod(apply, "clicked"), "valid settings can be applied through the dialog");
    check(controller.dictationModelPath() == model.fileName() && controller.dictationAudioDevice() == QStringLiteral("ui_capture"),
          "the actual QML controls select the model and microphone");
    check(controller.dictationSettingsError().isEmpty(), "the dialog clears its previous validation error");
    window->resize(980, 640);
    QEventLoop resized;
    QTimer::singleShot(100, &resized, &QEventLoop::quit);
    resized.exec();
    auto* popup = qobject_cast<QQuickItem*>(dialog->property("contentItem").value<QObject*>());
    check(popup && popup->height() > 0 && popup->height() < window->height(), "dictation content fits the minimum desktop height");
    const auto problems = kestrel::ui::auditLayouts(window->contentItem());
    for (const auto& problem : problems) {
        std::cout << problem.where.toStdString() << ": " << problem.what.toStdString() << '\n';
    }
    check(problems.isEmpty(), "the dialog and runtime panel have no collapsed or overlapping cells at minimum size");
    settings.clear();
}

void testRealLinuxDictationThroughControllerWhenAsked() {
    const QByteArray model = qgetenv("KESTREL_TEST_WHISPER_MODEL");
    const QByteArray device = qgetenv("KESTREL_TEST_ALSA_DEVICE");
    if (model.isEmpty() || device.isEmpty()) { return; }
    QSettings settings(QStringLiteral("Kestrel"), QStringLiteral("Kestrel"));
    settings.clear();
    qputenv("KESTREL_SPEECH_INPUT", "platform");
    {
        kestrel::app::AppController controller;
        controller.setIdleLoopEnabled(false);
        controller.setBackendForTesting(std::make_unique<kestrel::runtime::MockBackend>());
        check(controller.applyDictationSettings(QUrl::fromLocalFile(QString::fromUtf8(model)).toString(), QString::fromUtf8(device)),
              "desktop settings select real Whisper weights and prerecorded ALSA capture");
        check(controller.sttAvailable(), "configured native offline dictation is available");
        check(controller.startListening(), "controller starts real prerecorded ALSA/Whisper dictation");
        QElapsedTimer deadline;
        deadline.start();
        while (controller.messages()->rowCount() == 0 && deadline.elapsed() < 15000) {
            QEventLoop step;
            QTimer::singleShot(10, &step, &QEventLoop::quit);
            step.exec();
        }
        check(controller.messages()->rowCount() >= 2, "real dictated audio starts a conversation turn");
        if (controller.messages()->rowCount() > 0) {
            const QString heard = controller.messages()->data(controller.messages()->index(0, 0),
                kestrel::app::MessageModel::ContentRole).toString();
            check(heard.contains(QStringLiteral("country"), Qt::CaseInsensitive),
                  "Whisper's recognized words reach the visible user message");
        }
        controller.abandonListening();
        controller.stopGeneration();
    }
    settings.clear();
    qputenv("KESTREL_SPEECH_INPUT", "mock");
}

void testLinuxRecognizerReachesListenSession() {
    class RecordedCapture final : public kestrel::runtime::SpeechCapture {
    public:
        bool capture(std::vector<float>& samples, const kestrel::runtime::SpeechCancelled&,
                     std::string&) override { samples.assign(16000, 0.1F); return true; }
    };
    class RecordedTranscriber final : public kestrel::runtime::SpeechTranscriber {
    public:
        bool transcribe(const std::vector<float>&, const kestrel::runtime::SpeechCancelled&,
                        std::string& text, std::string&) override {
            text = "local Linux phrase"; return true;
        }
    };
    kestrel::runtime::LinuxSpeechRecognizer recognizer(
        [] { return std::make_unique<RecordedCapture>(); },
        [] { return std::make_unique<RecordedTranscriber>(); });
    kestrel::app::ListenSession session(recognizer);
    QString heard;
    QEventLoop wait;
    QObject::connect(&session, &kestrel::app::ListenSession::utteranceFinal, &wait,
                     [&](const QString& text, double confidence) { heard = text; check(confidence == 0.0, "no invented Whisper confidence"); });
    QObject::connect(&session, &kestrel::app::ListenSession::listeningEnded, &wait, &QEventLoop::quit);
    QTimer::singleShot(2000, &wait, &QEventLoop::quit);
    QString error;
    check(session.startListening(error), "native Linux recognizer starts through ListenSession");
    wait.exec();
    check(heard == QStringLiteral("local Linux phrase"), "worker transcript reaches the Qt listening interface");
    check(!session.listening(), "native phrase completion ends the UI listening session");
}
#endif

#if KESTREL_HAS_QT_MULTIMEDIA
class PendingSpeechBackend final : public kestrel::app::LocalModelSpeechBackend {
public:
    PendingSpeechBackend() { completeSetup(); }
    using LocalModelSpeechBackend::noteSynthesised;
    using LocalModelSpeechBackend::noteClauseFailed;
    QStringList requests;
protected:
    void startEngine() override { markLaunched(); markStarted(); }
    void synthesise(const QString& text, const QString&) override { requests.append(text); }
    QStringList engineVoices() const override { return {QStringLiteral("test")}; }
    QString engineName() const override { return QStringLiteral("test"); }
};

void testSpeakingAdoptsPendingPrefetch() {
    PendingSpeechBackend backend;
    backend.prefetch(QStringLiteral("first"));
    backend.speak(QStringLiteral("first"));
    backend.prefetch(QStringLiteral("second"));
    check(backend.requests.size() == 2, "speaking reuses an in-flight prefetch");
    QTemporaryFile audio;
    check(audio.open(), "a local audio path is available");
    backend.noteSynthesised(audio.fileName());
    const auto* player = backend.findChild<QMediaPlayer*>();
    check(player != nullptr && player->source() == QUrl::fromLocalFile(audio.fileName()),
          "the pending prefetch is played when it completes");
    backend.stopImmediately();

    backend.prefetch(QStringLiteral("failed"));
    backend.prefetch(QStringLiteral("survivor"));
    backend.noteClauseFailed(QStringLiteral("one clause failed"));
    check(backend.usable(), "a failed clause leaves the engine available");
    backend.speak(QStringLiteral("survivor"));
    check(backend.requests.size() == 4, "a clause failure preserves the next pending request");
    backend.stopImmediately();
}
#endif

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
    check(controller.acknowledgement().isEmpty(),
          "a spoken request begins with its answer, not a stock cue");
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
// The approval gate is the whole safety story for administrator commands, so
// its refusal path is tested directly: no approver, and an approver that says
// no, must both leave the command unstarted.
void testPrivilegedCommandRefusesWithoutApproval() {
    kestrel::app::PrivilegedRequest request;
    request.program = QStringLiteral("true");
    request.reason = QStringLiteral("test");

    bool refusedWithoutApprover = false;
    kestrel::app::runPrivilegedCommand(request, {}, [&](kestrel::app::PrivilegedResult result) {
        refusedWithoutApprover = !result.ran
            && result.error.contains(QStringLiteral("not approved"));
    });
    check(refusedWithoutApprover, "privileged command does not run without an approver");

    bool asked = false;
    bool declinedCleanly = false;
    kestrel::app::runPrivilegedCommand(
        request,
        [&asked](const kestrel::app::PrivilegedRequest&, kestrel::app::PrivilegedDecision decide) {
            asked = true;
            decide(false);
        },
        [&](kestrel::app::PrivilegedResult result) { declinedCleanly = !result.ran; });
    check(asked, "the user is asked before a privileged command runs");
    check(declinedCleanly, "a declined privileged command does not run");
}

// Replies a scripted sequence, one per generation, streamed in small chunks so
// that a tool tag is split across tokens the way a real model splits it.
class ScriptedToolBackend final : public kestrel::runtime::ModelBackend {
public:
    explicit ScriptedToolBackend(std::vector<std::string> replies)
        : m_replies(std::move(replies)) {}

    [[nodiscard]] BackendKind kind() const noexcept override { return BackendKind::Mock; }

    [[nodiscard]] kestrel::runtime::RuntimeStatus status() const override {
        return {true, true, "Scripted tool backend", "test", "unit test", 0.0, 0, 4096};
    }

    bool loadModel(const std::string&, std::string&) override { return true; }

    void generate(const GenerationRequest& request,
                  kestrel::runtime::TokenCallback onToken,
                  kestrel::runtime::CompletionCallback onComplete) override {
        std::string reply;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_requests.push_back(request);
            reply = m_next < m_replies.size() ? m_replies[m_next] : std::string("Done.");
            ++m_next;
        }
        for (std::size_t offset = 0; offset < reply.size(); offset += 7) {
            onToken(std::string_view(reply).substr(offset, 7));
        }
        onComplete(true, {});
    }

    void cancel() override {}

    [[nodiscard]] std::vector<GenerationRequest> requests() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_requests;
    }

private:
    mutable std::mutex m_mutex;
    std::vector<std::string> m_replies;
    std::size_t m_next = 0;
    std::vector<GenerationRequest> m_requests;
};

// The chat tool loop end to end: the model writes a tool call, the call is run
// through the dispatcher, its result becomes model context, and the model's
// next reply is shown. The call itself never appears in the transcript.
void testChatToolLoopRunsCallAndAnswers() {
    kestrel::core::McpDispatcher dispatcher;
    dispatcher.addTool({
        "kestrel_status",
        "Report status.",
        {kestrel::core::ToolPermission::ReadConversations},
        [](const kestrel::storage::json::Value&) {
            return kestrel::core::McpToolResult{false, "ok"};
        },
        nullptr});
    dispatcher.permissions().setEnabled("kestrel_status", true);

    AppController controller;
    controller.setIdleLoopEnabled(false);
    controller.setAgentTools(&dispatcher);
    auto model = std::make_unique<ScriptedToolBackend>(std::vector<std::string>{
        "Let me check. <tool_call>{\"name\":\"list_tools\",\"arguments\":{}}</tool_call>",
        "The status tool is available."});
    ScriptedToolBackend* observed = model.get();
    controller.setBackendForTesting(std::move(model));
    controller.sendMessage(QStringLiteral("What tools do you have?"));

    // Generation pauses while the tool runs, so wait for the second reply itself
    // rather than for the first one to finish.
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(5);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (observed->requests().size() == 2 && !controller.generating()) {
            loop.quit();
        }
    });
    QTimer::singleShot(20000, &loop, &QEventLoop::quit);
    poll.start();
    loop.exec();

    const auto requests = observed->requests();
    check(requests.size() == 2, "a tool call starts exactly one follow-up generation");
    if (requests.size() == 2) {
        bool sawListing = false;
        for (const auto& message : requests.back().messages) {
            sawListing = sawListing || (message.role == kestrel::runtime::Role::User
                                        && message.content.find("kestrel_status") != std::string::npos);
        }
        check(sawListing, "the tool result is part of the model's context for the next reply");
    }

    bool leaked = false;
    bool sawResultRow = false;
    bool sawAnswer = false;
    const kestrel::app::MessageModel* messages = controller.messages();
    for (int row = 0; row < messages->rowCount(); ++row) {
        const QModelIndex index = messages->index(row, 0);
        const QString content = messages->data(index, kestrel::app::MessageModel::ContentRole).toString();
        const QString author = messages->data(index, kestrel::app::MessageModel::AuthorRole).toString();
        leaked = leaked || content.contains(QStringLiteral("<tool_call>"));
        sawResultRow = sawResultRow || author == QStringLiteral("tool");
        sawAnswer = sawAnswer || content.contains(QStringLiteral("The status tool is available."));
    }
    check(!leaked, "the tool call is hidden from the transcript");
    check(sawResultRow, "the tool result is recorded in the transcript");
    check(sawAnswer, "the model's answer after the tool result is shown");
    controller.stopGeneration();
}

int main(int argc, char** argv) {
    // Unbuffered, so a crash still shows how far the run got. A lost buffer
    // turns a five-second diagnosis into a guess about which test died.
    std::cout << std::unitbuf;

    QTemporaryDir configuration;
    if (!configuration.isValid()) { return 1; }
    qputenv("XDG_CONFIG_HOME", configuration.path().toUtf8());
    QGuiApplication app(argc, argv);
    // Do not read or overwrite the developer's actual desktop preferences.
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, configuration.path());

    // Pin the recognizer. The app picks one for the machine it runs on -- SAPI 5
    // when there is a microphone, the scripted preview recognizer when there is
    // not -- and the tests below assert on partial words and on the timing of a
    // barge-in, which only the scripted one produces. Without this, plugging in
    // a microphone would change what the suite means.
    qputenv("KESTREL_SPEECH_INPUT", "mock");

    if (qEnvironmentVariableIsSet("KESTREL_TEST_SPEECH_GGUF")) {
        testRealModelSpokenSummaryWhenRequested();
        return failures == 0 ? 0 : 1;
    }

    testListenSessionIgnoresQueuedCallbacksFromPreviousSession();
    testPrivilegedCommandRefusesWithoutApproval();
    testGenerationRunsOffCallingThread();
    testTokensStreamBeforeCompletionAndPreserveUtf8();
    testCancelBeforeWorkerStarts();
    testCancelStopsInFlightGeneration();
    testFileDialogUrlBecomesALocalPath();
    testWorkerFollowsTheSwappedBackend();
    testSendMessageProducesAReply();
    testResumeUsesPartialAssistantPrompt();
    testModelLoadReportsAsynchronously();
    testRuntimeDiagnosticsIdentifyTheActiveBackend();
    testSendingWithoutModelReportsActionableError();
    testDiagnosticsPreferencePersists();
    testEchoCancelSessionLifecycle();
    testSpokenSummaryKeepsTheWrittenAnswer();
    testSummaryCancellationAndQmlControls();
    testAudioControlsDoNotCancelGeneration();
    testSpeechControlsThroughQml();
    testRecognizedAcknowledgementAndHeldInterruption();
    testSpokenPlaybackCommands();
    testPresenceAndIdleLoopProject();
    testLongAnswerIsOfferedToContinue();
    testIdleToolNeedsPermissionBeforeItRuns();
    testSpokenTurnIsNotDeliveredBeforeTheLastClause();
    testSpokenResponseFollowsClauseOrder();
    testBargeInStopsAudioAtAClauseBoundary();
    testWarmthReachesTheVoice();
    testLoadingVoiceLeavesTheCurrentReplySilent();
    testReplyIsDeliveredAsTextWhenTheVoiceNeverArrives();
    testPaceIsCommittedWhereTheAudioIsAskedFor();
    testTheEngineIsAskedForClausesInTheOrderTheyAreSpoken();
    testLeavingAConversationDropsTheReplyItWasHolding();
    testStoppingDropsAReplyHeldForAVoice();
    testAReplySentMidSpeechTakesItsOwnWarmth();
    testLoadingVoiceReportsTextOnlyWithoutWaiting();
    testNewTurnReleasesOwedAudioAndOmitsToolHistory();
    testChatToolLoopRunsCallAndAnswers();
    testPreviewInputRequiresExplicitOptIn();
#if defined(__linux__)
    testLinuxRecognizerReachesListenSession();
    testLinuxDictationSettings();
    testLinuxDictationSettingsDialog();
    testRealLinuxDictationThroughControllerWhenAsked();
#endif
#if KESTREL_HAS_QT_MULTIMEDIA
    testSpeakingAdoptsPendingPrefetch();
#endif
    testSpokenPhraseTakesTheTypedPath();
    testAbandonedPhraseIsNotSubmitted();
    testRecallIsOffUntilTurnedOnAndPersists();
    testRecallWithoutAKeystoreStaysOff();
    testAFailedSaveUndoesTheChange();
    testForgetAndClearAreSaved();
    testBlankMemoryIsRefusedWithAReason();
    testAnUnreadableProfileIsNeverOverwritten();
    testRecalledFactReachesTheModelButNotTheTranscript();

    std::cout << (failures == 0 ? "\napp tests passed\n" : "\napp tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
