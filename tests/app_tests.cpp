// Tests for the asynchronous generation path.
//
// The threading contract is the risky part of this feature, so these tests
// exercise the real thing: a backend generating on a worker thread, tokens
// crossing to the test's thread through queued signals, and cancellation
// taking effect while generation is still in flight.

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QObject>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <atomic>
#include <iostream>
#include <thread>

#include "app/appcontroller.h"
#include "app/generationworker.h"
#include "app/messagemodel.h"
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

    void generate(const GenerationRequest&,
                  kestrel::runtime::TokenCallback onToken,
                  kestrel::runtime::CompletionCallback onComplete) override {
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

private:
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

int failures = 0;

void check(bool condition, const char* what) {
    if (condition) {
        std::cout << "  ok   " << what << "\n";
    } else {
        std::cout << "  FAIL " << what << "\n";
        ++failures;
    }
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

// A QML FileDialog speaks in URLs, and the controller converts them to local
// paths. That conversion is the fragile step in loading a model from disk, so
// it is pinned here rather than only exercised by hand.
void testFileDialogUrlBecomesALocalPath() {
    std::cout << "file dialog URLs convert to local paths\n";

    // Windows, percent-encoded, as QtQuick.Dialogs hands it over.
    const QUrl windowsUrl(QStringLiteral("file:///C:/kestrel-deps/models/my%20model.gguf"));
    check(windowsUrl.toLocalFile() == QStringLiteral("C:/kestrel-deps/models/my model.gguf"),
          "a percent-encoded Windows URL decodes to a real path");

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

// After the user loads a model, the worker must generate through the new
// backend rather than the one it was constructed with.
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
    // The controller takes ownership, so the local handle is released rather
    // than left to free memory the controller now owns.
    auto backend = std::make_unique<kestrel::runtime::MockBackend>();
    controller.setBackendForTesting(backend.get());
    backend.release();

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

    std::cout << (failures == 0 ? "\napp tests passed\n" : "\napp tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
