#include "app/groqbackend.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace kestrel::app {

GroqBackend::GroqBackend(std::string apiKey, NetworkFactory networkFactory)
    : m_apiKey(std::move(apiKey)), m_networkFactory(std::move(networkFactory)) {
    m_status.available = !m_apiKey.empty();
    m_status.backendName = "Groq (cloud)";
    m_status.detail = "Cloud inference is opt-in. Chats and tool results are sent to Groq. "
                      "Prompt caching is automatic, not guaranteed; token estimates are approximate.";
}

runtime::BackendKind GroqBackend::kind() const noexcept { return runtime::BackendKind::Groq; }

runtime::RuntimeStatus GroqBackend::status() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

bool GroqBackend::loadModel(const std::string& model, std::string& error) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_apiKey.empty()) {
        error = "Set GROQ_API_KEY or choose a Groq API key file before selecting cloud inference.";
        return false;
    }
    if (model != "openai/gpt-oss-120b" && model != "openai/gpt-oss-20b") {
        error = "Choose openai/gpt-oss-120b or openai/gpt-oss-20b for Groq chat.";
        return false;
    }
    m_status.modelName = model;
    m_status.modelLoaded = true;
    m_status.contextLimit = 131072;
    error.clear();
    return true;
}

void GroqBackend::cancel() { m_cancelled.store(true, std::memory_order_release); }

void GroqBackend::setSystemPrompt(std::string_view text) {
    // Only copying the prefix is protected: an Undermind connection may refresh
    // it on the UI thread while the previous request is still streaming.
    std::lock_guard<std::mutex> lock(m_mutex);
    m_systemPrompt = text;
}

void GroqBackend::clearSharedPrefix() { setSystemPrompt({}); }

void GroqBackend::resetContextUsage() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_status.contextUsed = 0;
}

std::size_t GroqBackend::cachedPrefixTokens() const {
    // Provider cache hits can include conversation history, not just the prefix;
    // never present those as a locally resident prefix cache.
    return 0;
}

void GroqBackend::generate(const runtime::GenerationRequest& request,
                           runtime::TokenCallback onToken, runtime::CompletionCallback onComplete) {
    m_cancelled.store(false, std::memory_order_release);
    std::string prefix;
    std::string model;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_status.modelLoaded) {
            onComplete(false, "Select a Groq model before sending a message.");
            return;
        }
        prefix = m_systemPrompt;
        model = m_status.modelName;
    }
    if (request.empty()) {
        onComplete(false, "Enter a message to send to Groq.");
        return;
    }
    QJsonArray messages;
    if (!prefix.empty()) {
        messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("system")},
                                    {QStringLiteral("content"), QString::fromStdString(prefix)}});
    }
    for (const runtime::ChatMessage& message : request.messages) {
        // Kestrel's tool protocol is textual (<tool_call>), not OpenAI function
        // calls. Tool results are user context, without invented tool_call_ids.
        QString role = QStringLiteral("user");
        if (message.role == runtime::Role::System) { role = QStringLiteral("system"); }
        if (message.role == runtime::Role::Assistant) { role = QStringLiteral("assistant"); }
        messages.append(QJsonObject{{QStringLiteral("role"), role},
                                    {QStringLiteral("content"), QString::fromStdString(message.content)}});
    }
    if (!request.addAssistantCue) {
        messages.append(QJsonObject{
            {QStringLiteral("role"), QStringLiteral("user")},
            {QStringLiteral("content"), QStringLiteral(
                "Continue your previous answer exactly where it stopped. Output only the continuation; do not repeat text.")}});
    }
    const QJsonObject body{
        {QStringLiteral("model"), QString::fromStdString(model)},
        {QStringLiteral("messages"), messages},
        {QStringLiteral("temperature"), static_cast<double>(request.temperature)},
        {QStringLiteral("max_completion_tokens"), std::max(1, request.maxTokens)},
        {QStringLiteral("reasoning_effort"), QStringLiteral("low")},
        {QStringLiteral("include_reasoning"), false},
        {QStringLiteral("stream"), true}};
    auto network = m_networkFactory ? m_networkFactory() : std::make_unique<QNetworkAccessManager>();
    QNetworkRequest headers(QUrl(QStringLiteral("https://api.groq.com/openai/v1/chat/completions")));
    headers.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    headers.setRawHeader("Authorization", QByteArray("Bearer ") + QByteArray::fromStdString(m_apiKey));
    headers.setRawHeader("Accept", "text/event-stream");
    // Never follow a redirect carrying the provider credential to another host.
    headers.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    headers.setTransferTimeout(60000);
    QNetworkReply* reply = network->post(headers, QJsonDocument(body).toJson(QJsonDocument::Compact));
    reply->setReadBufferSize(1024 * 1024);
    QEventLoop loop;
    QElapsedTimer elapsed;
    elapsed.start();
    QTimer poll;
    poll.setInterval(20);
    bool timedOut = false;
    bool done = false;
    bool sawContent = false;
    bool sawStop = false;
    QString failure;
    QByteArray pending;
    QByteArray eventData;
    QJsonObject usage;
    constexpr qsizetype kMaxEventBytes = 1024 * 1024;
    const auto fail = [&](const QString& text) {
        failure = text;
        reply->abort();
    };
    const auto event = [&] {
        if (eventData.isEmpty() || !failure.isEmpty()) { return; }
        const QByteArray data = std::exchange(eventData, {}).trimmed();
        if (data == "[DONE]") { done = true; return; }
        if (done) { fail(QStringLiteral("Groq sent data after ending its response. Retry the message.")); return; }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            fail(QStringLiteral("Groq returned an invalid stream event. Retry the message."));
            return;
        }
        const QJsonObject chunk = document.object();
        if (chunk.contains(QStringLiteral("error"))) {
            fail(QStringLiteral("Groq reported an error during the response. Retry or check your account limits."));
            return;
        }
        QJsonObject reportedUsage = chunk.value(QStringLiteral("usage")).toObject();
        if (reportedUsage.isEmpty()) {
            reportedUsage = chunk.value(QStringLiteral("x_groq")).toObject().value(QStringLiteral("usage")).toObject();
        }
        if (!reportedUsage.isEmpty()) { usage = reportedUsage; }
        const QJsonArray choices = chunk.value(QStringLiteral("choices")).toArray();
        if (choices.isEmpty()) { return; } // A final usage-only event is valid.
        const QJsonObject choice = choices.first().toObject();
        const QJsonObject delta = choice.value(QStringLiteral("delta")).toObject();
        if (delta.contains(QStringLiteral("tool_calls"))) {
            fail(QStringLiteral("Groq returned unsupported native tool calls. Retry using Kestrel's tool protocol."));
            return;
        }
        const QString text = delta.value(QStringLiteral("content")).toString();
        if (!text.isEmpty()) {
            sawContent = true;
            const QByteArray utf8 = text.toUtf8();
            onToken(std::string_view(utf8.constData(), static_cast<std::size_t>(utf8.size())));
        }
        const QString reason = choice.value(QStringLiteral("finish_reason")).toString();
        if (reason == QStringLiteral("stop")) { sawStop = true; }
        else if (!reason.isEmpty()) {
            fail(reason == QStringLiteral("length")
                ? QStringLiteral("Groq reached the response token limit. Resume the answer or ask for a shorter reply.")
                : QStringLiteral("Groq could not finish this answer. Try rephrasing the message."));
        }
    };
    const auto consume = [&] {
        if (m_cancelled.load(std::memory_order_acquire) || !failure.isEmpty()) { return; }
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray incoming = reply->readAll();
        if (status < 200 || status >= 300) {
            // Discard error bodies; they may echo sensitive request data.
            return;
        }
        if (!reply->header(QNetworkRequest::ContentTypeHeader).toByteArray().startsWith("text/event-stream")) {
            fail(QStringLiteral("Groq did not return an event stream. Retry the message."));
            return;
        }
        pending += incoming;
        qsizetype newline;
        while ((newline = pending.indexOf('\n')) >= 0 && failure.isEmpty()) {
            QByteArray line = pending.left(newline);
            pending.remove(0, newline + 1);
            if (line.endsWith('\r')) { line.chop(1); }
            if (line.isEmpty()) { event(); }
            else if (line.startsWith("data:")) {
                QByteArray data = line.mid(5);
                if (data.startsWith(' ')) { data.remove(0, 1); }
                if (!eventData.isEmpty()) { eventData += '\n'; }
                eventData += data;
            }
            if (eventData.size() > kMaxEventBytes) {
                fail(QStringLiteral("Groq returned an oversized stream event. Retry with a shorter request."));
            }
        }
        if (pending.size() > kMaxEventBytes) {
            fail(QStringLiteral("Groq returned an oversized stream event. Retry with a shorter request."));
        }
    };
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, consume);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (m_cancelled.load(std::memory_order_acquire) || elapsed.elapsed() >= 60000) {
            timedOut = !m_cancelled.load(std::memory_order_acquire);
            reply->abort();
            loop.quit();
        }
    });
    poll.start();
    if (!reply->isFinished()) { loop.exec(); }
    poll.stop();
    consume();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (m_cancelled.load(std::memory_order_acquire)) { failure = QStringLiteral("Generation stopped"); }
    else if (timedOut) { failure = QStringLiteral("Groq timed out after 60 seconds. Check your connection and retry."); }
    else if (status == 429) {
        const QByteArray retry = reply->rawHeader("retry-after");
        bool valid = false;
        const int seconds = retry.toInt(&valid);
        failure = valid && seconds >= 0
            ? QStringLiteral("Groq rate limit reached. Retry after %1 seconds; check your Groq account limits.").arg(seconds)
            : QStringLiteral("Groq rate limit reached. Wait before retrying; check your Groq account limits.");
    } else if (status == 401 || status == 403) {
        failure = QStringLiteral("Groq rejected this API key. Choose a valid key and check its model permissions.");
    } else if (status < 200 || status >= 300) {
        // No raw error body: it can echo prompts or credentials.
        failure = status == 0
            ? QStringLiteral("Cannot reach Groq. Check your internet connection and TLS/network settings.")
            : QStringLiteral("Groq request failed (HTTP %1). Check the model, account limits, and service status.").arg(status);
    } else if (failure.isEmpty() && reply->error() != QNetworkReply::NoError) {
        failure = QStringLiteral("The Groq connection ended unexpectedly. Check your network and retry.");
    } else if (failure.isEmpty() && (!done || !sawStop || !sawContent)) {
        failure = QStringLiteral("Groq returned an empty or incomplete response. Retry the message.");
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const int total = usage.value(QStringLiteral("total_tokens")).toInt();
        if (total > 0) { m_status.contextUsed = static_cast<std::size_t>(total); }
        const double seconds = usage.value(QStringLiteral("completion_time")).toDouble();
        m_status.tokensPerSecond = seconds > 0
            ? usage.value(QStringLiteral("completion_tokens")).toDouble() / seconds : 0.0;
    }
    delete reply;
    const std::string error = failure.toStdString();
    onComplete(failure.isEmpty(), error);
}

} // namespace kestrel::app
