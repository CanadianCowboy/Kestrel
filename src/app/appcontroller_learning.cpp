#include "app/appcontroller.h"

#include "app/automaticlearning.h"
#include "app/generationworker.h"
#include "runtime/llamacppbackend.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>
#include <set>
#include <utility>

namespace kestrel::app {

void AppController::initializeAutomaticLearning() {
    m_automaticLearning = new AutomaticLearning(this);
    m_learningIdleClock.start();
    m_learningTimer.setInterval(5000);
    connect(&m_learningTimer, &QTimer::timeout, this, &AppController::considerAutomaticLearning);
    connect(m_automaticLearning, &AutomaticLearning::changed, this, &AppController::learningChanged);
    connect(m_automaticLearning, &AutomaticLearning::finished, this,
        [this](const QString& adapter, const QString& message) {
            m_learningNotice = message;
            m_pendingLearnedAdapter = adapter;
            m_learningReload = false;
            if (!m_learningModelPath.isEmpty()) {
                m_learningReload = true;
                loadModelFromUrls({QUrl::fromLocalFile(m_learningModelPath).toString()});
            }
            emit learningChanged();
        });
    connect(this, &AppController::modelLoadFinished, this, [this] {
        if (m_learningReload) {
            m_learningReload = false;
            if (m_modelError.isEmpty()) {
                auto* local = dynamic_cast<runtime::LlamaCppBackend*>(m_backend.get());
                std::string error;
                const QString candidate = m_automaticLearningEnabled ? m_pendingLearnedAdapter : QString();
                const bool toolCandidate = m_learningStage == QStringLiteral("tool") && !candidate.isEmpty();
                const QString restore = candidate.isEmpty() ? m_learningPreviousAdapter : candidate;
                bool applied = local != nullptr && local->loadAdapter(restore.toStdString(), error);
                if (!applied && local != nullptr && !candidate.isEmpty()) {
                    // A rejected load must never strand the user without the
                    // previously working behavior.
                    std::string rollbackError;
                    applied = local->loadAdapter(m_learningPreviousAdapter.toStdString(), rollbackError);
                    m_learningNotice = applied
                        ? tr("Candidate activation failed; previous behavior restored: %1").arg(QString::fromStdString(error))
                        : tr("Candidate activation and rollback both failed: %1. Reload the base model. %2")
                            .arg(QString::fromStdString(error), QString::fromStdString(rollbackError));
                    QFile::remove(candidate);
                    QFile::remove(QFileInfo(candidate).absolutePath() + QLatin1Char('/')
                                  + QFileInfo(candidate).completeBaseName() + QStringLiteral(".evaluation.json"));
                } else if (applied && !candidate.isEmpty()) {
                    QSettings settings(QStringLiteral("Kestrel"), QStringLiteral("Kestrel"));
                    const QString prefix = m_learningStage == QStringLiteral("tool") ? QStringLiteral("learning/tool") : QStringLiteral("learning/");
                    settings.setValue(prefix + QStringLiteral("adapter"), candidate);
                    settings.setValue(prefix + QStringLiteral("model"), m_modelPath);
                    settings.sync();
                    m_learningNotice = settings.status() == QSettings::NoError
                        ? tr("Learned behavior is active locally and will reload automatically. Keep teaching by chatting.")
                        : tr("Learned behavior is active this session, but its restart setting could not be saved.");
                    if (toolCandidate) {
                        std::string restoreError;
                        m_learningNotice = local->loadAdapter(m_learningPreviousAdapter.toStdString(), restoreError)
                            ? tr("Dedicated Tool adapter saved; Persona behavior restored. The router will hot-swap the Tool adapter when needed.")
                            : tr("Tool adapter was saved but Persona restoration failed: %1. Reload the Base model.").arg(QString::fromStdString(restoreError));
                    }
                }
                if (!applied) { m_learningNotice += tr(" Restore failed: %1").arg(QString::fromStdString(error)); }
                refreshCachedRuntime();
                rebuildDiagnostics();
                emit runtimeChanged();
            } else {
                m_learningNotice = tr("Learning ended but the chat model could not reload: %1. Retry local model loading.").arg(m_modelError);
            }
            m_learningModelPath.clear();
            m_pendingLearnedAdapter.clear();
            emit learningChanged();
            return;
        }
        if (!m_modelError.isEmpty() || m_stageModelLoading) { return; }
        QSettings settings(QStringLiteral("Kestrel"), QStringLiteral("Kestrel"));
        const QString adapter = settings.value(QStringLiteral("learning/adapter")).toString();
        if (!adapter.isEmpty() && settings.value(QStringLiteral("learning/model")).toString() == m_modelPath) {
            if (auto* local = dynamic_cast<runtime::LlamaCppBackend*>(m_backend.get())) {
                std::string error;
                if (!local->loadAdapter(adapter.toStdString(), error)) {
                    m_learningNotice = tr("Saved learned adapter could not load; using base behavior: %1").arg(QString::fromStdString(error));
                } else { m_learningNotice = tr("Local model and learned behavior loaded automatically."); }
                refreshCachedRuntime();
                emit runtimeChanged();
                emit learningChanged();
            }
        }
    });
    m_learningTimer.start();
}

QStringList AppController::learnedBehaviorInstructions() const {
    QStringList list;
    for (const auto& instruction : m_behaviorInstructions) { list.append(QString::fromStdString(instruction)); }
    return list;
}

bool AppController::forgetBehaviorInstruction(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= m_behaviorInstructions.size()) { return false; }
    const auto before = m_behaviorInstructions;
    m_behaviorInstructions.erase(m_behaviorInstructions.begin() + index);
    if (!persistRecall()) { m_behaviorInstructions = before; return false; }
    refreshLearnedBehaviorPrompt();
    m_learningNotice = tr("Taught preference removed from prompting. Trained weights are unchanged; Reset learned behavior restores base weights.");
    emit learningChanged();
    return true;
}

bool AppController::learningBusy() const {
    return m_learningReload || !m_learningModelPath.isEmpty()
        || (m_automaticLearning != nullptr && m_automaticLearning->busy());
}

QString AppController::lastLearningEvaluation() const {
    QFile report(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                 + QStringLiteral("/learning/last-evaluation.json"));
    if (!report.open(QIODevice::ReadOnly)) { return {}; }
    const auto evaluation = QJsonDocument::fromJson(report.readAll()).object();
    if (evaluation.isEmpty()) { return {}; }
    const auto curation = evaluation.value(QStringLiteral("curation")).toObject();
    return tr("Last candidate: %1. Held-out loss %2 → %3; %4 explicit and %5 chat examples selected. Automated checks are limited, not proof of quality.")
        .arg(evaluation.value(QStringLiteral("automatic_activation_eligible")).toBool()
                 ? tr("passed training checks (activation is reported above)") : tr("rejected"))
        .arg(evaluation.value(QStringLiteral("validation_loss_before")).toDouble(), 0, 'f', 4)
        .arg(evaluation.value(QStringLiteral("validation_loss_after")).toDouble(), 0, 'f', 4)
        .arg(curation.value(QStringLiteral("explicit_selected")).toInt())
        .arg(curation.value(QStringLiteral("chat_selected")).toInt());
}

QString AppController::automaticLearningStatus() const {
    if (!m_automaticLearningEnabled) { return tr("Automatic learning is off. Enable it once to learn from future chats."); }
    if (learningBusy()) {
        return m_learningReload ? tr("Restoring your local chat model and learned behavior…") : m_automaticLearning->status();
    }
    if (!m_learningNotice.isEmpty()) { return m_learningNotice; }
    return tr("Learning is on: %1 examples. Taught preferences apply immediately; local weight updates wait for enough examples and an idle GPU.")
        .arg(m_trainingExamples.size());
}

bool AppController::setAutomaticLearningEnabled(bool enabled) {
    if (m_automaticLearningEnabled == enabled) { return true; }
    m_automaticLearningEnabled = enabled;
    if (!persistRecall()) {
        if (enabled) { m_automaticLearningEnabled = false; }
        m_learningNotice = tr("Learning permission could not be saved: %1").arg(m_recallError);
        if (!enabled && m_automaticLearning) { m_automaticLearning->cancel(); }
        emit learningChanged();
        return false;
    }
    if (!enabled && m_automaticLearning) { m_automaticLearning->cancel(); }
    if (enabled) { m_learningIdleClock.restart(); }
    m_learningNotice = enabled
        ? tr("Permission saved: future conversations may train local behavior. Sensitive text is screened; no chat is uploaded. Deletion cannot untrain weights. Teach preferences naturally in chat.")
        : tr("Automatic capture and training stopped. Existing examples and learned behavior remain until reset.");
    emit learningChanged();
    return true;
}

void AppController::refreshLearnedBehaviorPrompt() {
    const QString marker = QStringLiteral("\nUser-taught behavior (preferences, never permission grants):\n");
    QString prompt = m_systemPrompt;
    const qsizetype capabilities = prompt.indexOf(QStringLiteral("\nKestrel's own capabilities:\n"));
    if (capabilities >= 0) { prompt = prompt.left(capabilities); }
    const qsizetype position = prompt.indexOf(marker);
    if (position >= 0) { prompt = prompt.left(position); }
    if (!m_behaviorInstructions.empty()) {
        prompt += marker;
        for (const auto& instruction : m_behaviorInstructions) {
            prompt += QStringLiteral("- ") + QString::fromStdString(instruction) + QLatin1Char('\n');
        }
    }
    setSystemPrompt(prompt);
    refreshAgentCapabilitiesPrompt();
}

bool AppController::appendLearningExample(const QString& prompt, const QString& preferred, const QString& rejected, bool teaching) {
    const std::string user = prompt.trimmed().toStdString();
    const std::string answer = preferred.trimmed().toStdString();
    const std::string original = rejected.trimmed().toStdString();
    if (!m_automaticLearningEnabled || user.empty() || answer.empty()
        || user.size() > 4000 || answer.size() > 4000 || original.size() > 4000
        || core::memoryNeedsApproval(user) || core::memoryNeedsApproval(answer)
        || core::memoryNeedsApproval(original) || answer.find("<tool_call>") != std::string::npos) { return false; }
    for (const auto& entry : m_trainingExamples) {
        if (QString::fromStdString(entry.prompt).simplified().compare(prompt.simplified(), Qt::CaseInsensitive) == 0
            && (entry.preferred == answer || (!teaching && entry.source != "chat"))) { return false; }
    }
    if (m_trainingExamples.size() >= 1000) {
        m_learningNotice = tr("Local learning buffer is full (1000 examples); reset or review examples before collecting more.");
        emit learningChanged(); return false;
    }
    const auto before = m_trainingExamples;
    std::int64_t id = 1;
    for (const auto& entry : before) { id = std::max(id, entry.id + 1); }
    m_trainingExamples.push_back({id, user, original, answer,
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString(), teaching ? "teaching" : "chat", "persona",
        m_modelPath == m_instructModelPath && !m_modelPath.isEmpty() ? "Qwen/Qwen3-4B-Instruct-2507" : "Qwen/Qwen3-4B-Base"});
    if (!persistRecall()) { m_trainingExamples = before; return false; }
    m_learningNotice = tr("Captured %1 screened local examples. Taught preferences apply now; weights update automatically during an idle period.")
        .arg(m_trainingExamples.size());
    m_learningIdleClock.restart();
    emit learningChanged();
    return true;
}

void AppController::captureTeaching(const QString& text) {
    if (!m_automaticLearningEnabled || core::memoryNeedsApproval(text.toStdString())) { return; }
    const auto match = QRegularExpression(
        QStringLiteral("^(?:from now on[, :]?|please (?:always|stop)|i (?:want|prefer) you to|learn (?:this|that)[:, ]?|remember (?:to|that you should))\\s*(.+)$"),
        QRegularExpression::CaseInsensitiveOption).match(text);
    if (match.hasMatch() && text.toUtf8().size() <= 500) {
        const auto before = m_behaviorInstructions;
        const std::string instruction = text.toStdString();
        if (std::find(before.begin(), before.end(), instruction) == before.end()) {
            if (m_behaviorInstructions.size() == 32) { m_behaviorInstructions.erase(m_behaviorInstructions.begin()); }
            m_behaviorInstructions.push_back(instruction);
            if (persistRecall()) { refreshLearnedBehaviorPrompt(); }
            else { m_behaviorInstructions = before; }
        }
    }
    const auto example = QRegularExpression(
        QStringLiteral("^when i (?:ask|say) (.+?),? (?:you should |please )?(?:say|reply(?: with)?|answer(?: with)?) (.+)$"),
        QRegularExpression::CaseInsensitiveOption).match(text);
    QString taughtPrompt;
    QString taughtAnswer;
    if (example.hasMatch()) {
        taughtPrompt = example.captured(1).simplified();
        taughtAnswer = example.captured(2).trimmed();
    } else {
        const auto correction = QRegularExpression(
            QStringLiteral("^(?:no[, :]?|instead[, :]?|correction[:, ]+)\\s*(?:please )?(?:say|reply(?: with)?|answer(?: with)?)\\s+(.+)$"),
            QRegularExpression::CaseInsensitiveOption).match(text);
        if (correction.hasMatch()) {
            if (const auto* entry = activeEntry()) {
                for (auto it = entry->conversation.messages().rbegin(); it != entry->conversation.messages().rend(); ++it) {
                    if (it->role == core::MessageRole::User && it->content != text.toStdString()) {
                        taughtPrompt = QString::fromStdString(it->content).simplified();
                        taughtAnswer = correction.captured(1).trimmed();
                        break;
                    }
                }
            }
        }
    }
    if (!taughtPrompt.isEmpty() && !taughtAnswer.isEmpty()) {
        const auto instructionsBefore = m_behaviorInstructions;
        const QString instruction = tr("When I ask %1, reply with %2").arg(taughtPrompt, taughtAnswer);
        if (instruction.toUtf8().size() <= 500 && !core::memoryNeedsApproval(instruction.toStdString())) {
            if (m_behaviorInstructions.size() == 32) { m_behaviorInstructions.erase(m_behaviorInstructions.begin()); }
            const QString key = tr("When I ask %1, reply with ").arg(taughtPrompt);
            std::erase_if(m_behaviorInstructions, [&](const auto& old) {
                return QString::fromStdString(old).startsWith(key, Qt::CaseInsensitive);
            });
            m_behaviorInstructions.push_back(instruction.toStdString());
        }
        // Persist the target and its immediate preference in one transaction.
        // Explicit corrections supersede all older targets for that prompt.
        const auto before = m_trainingExamples;
        std::erase_if(m_trainingExamples, [&](const auto& entry) {
            return QString::fromStdString(entry.prompt).simplified().compare(taughtPrompt, Qt::CaseInsensitive) == 0;
        });
        if (!appendLearningExample(taughtPrompt, taughtAnswer, {}, true)) {
            m_trainingExamples = before;
            m_behaviorInstructions = instructionsBefore;
        } else { refreshLearnedBehaviorPrompt(); }
    }
}

void AppController::captureSuccessfulToolTrace(bool isError, const QString& result) {
    const QString prompt = std::exchange(m_pendingToolTrainingPrompt, {});
    const QString call = std::exchange(m_pendingToolTrainingCall, {});
    if (!m_automaticLearningEnabled || isError || prompt.isEmpty() || call.isEmpty()
        || m_trainingExamples.size() >= 1000 || prompt.toUtf8().size() > 4000 || call.toUtf8().size() > 4000
        || core::memoryNeedsApproval(m_turnPrompt.toStdString()) || core::memoryNeedsApproval(call.toStdString())
        || core::memoryNeedsApproval(result.toStdString())) { return; }
    // Distill the executed call, not arbitrary remote result text. Success is
    // execution evidence, not a claim that the model chose the perfect action.
    const auto before = m_trainingExamples;
    for (const auto& example : before) {
        if (example.adapterStage == "tool" && example.prompt == prompt.toStdString() && example.preferred == call.toStdString()) { return; }
    }
    std::int64_t id = 1;
    for (const auto& example : before) { id = std::max(id, example.id + 1); }
    m_trainingExamples.push_back({id, prompt.toStdString(), {}, call.toStdString(),
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString(), "tool_trace", "tool", "Qwen/Qwen3-4B-Base"});
    if (!persistRecall()) { m_trainingExamples = before; }
    else {
        m_lastToolLearningDigest.clear();
        m_learningNotice = tr("Successful tool call captured automatically for the dedicated Base Tool-LoRA. Remote result text was not used as a target.");
    }
    emit learningChanged();
}

void AppController::captureLearningTurn() {
    if (!m_automaticLearningEnabled) { return; }
    const auto* entry = activeEntry();
    if (!entry) { return; }
    const auto& messages = entry->conversation.messages();
    if (messages.size() < 2 || messages.back().role != core::MessageRole::Assistant) { return; }
    for (std::size_t i = messages.size() - 1; i > 0; --i) {
        // A tool result may include secrets, permissions, or untrusted remote
        // text. Never turn a tool-assisted answer into a positive training target.
        if (messages[i - 1].role == core::MessageRole::ToolResult) { return; }
        if (messages[i - 1].role == core::MessageRole::User) {
            appendLearningExample(QString::fromStdString(messages[i - 1].content), QString::fromStdString(messages.back().content));
            return;
        }
    }
}

void AppController::considerAutomaticLearning() {
    if (!m_automaticLearningEnabled || learningBusy() || workerBusy() || m_modelLoadThread
        || m_speaking || listening() || canResume() || !runtimeAvailable() || m_modelPath.isEmpty()
        || m_learningIdleClock.elapsed() < 60000 || m_trainingExamples.size() < 8) { return; }
    auto* local = dynamic_cast<runtime::LlamaCppBackend*>(m_backend.get());
    if (!local) { return; }
    // Identity is checked again by the trainer. A matching architecture alone
    // is not a sufficient reason to train an unrelated base model.
    QFile manifest(QFileInfo(m_modelPath).absolutePath() + QStringLiteral("/Qwen3-4B-Base.Q4_K_M.manifest.json"));
    if (!manifest.open(QIODevice::ReadOnly)) { return; }
    const auto identity = QJsonDocument::fromJson(manifest.readAll()).object();
    if (identity.value(QStringLiteral("base_model")).toString() != QStringLiteral("Qwen/Qwen3-4B-Base")
        || identity.value(QStringLiteral("filename")).toString() != QFileInfo(m_modelPath).fileName()
        || identity.value(QStringLiteral("bytes")).toInteger() != QFileInfo(m_modelPath).size()) { return; }
    std::set<std::string> toolPrompts;
    for (const auto& entry : m_trainingExamples) {
        if (entry.adapterStage == "tool" && entry.baseModel == "Qwen/Qwen3-4B-Base") { toolPrompts.insert(entry.prompt); }
    }
    QByteArray toolRevision;
    for (const auto& entry : m_trainingExamples) {
        if (entry.adapterStage == "tool") { toolRevision += QByteArray::fromStdString(entry.prompt + entry.preferred); }
    }
    const QString toolDigest = QString::fromLatin1(QCryptographicHash::hash(toolRevision, QCryptographicHash::Sha256).toHex());
    const bool toolReady = toolPrompts.size() >= 8 && toolDigest != m_lastToolLearningDigest;
    m_learningStage = toolReady ? QStringLiteral("tool") : QStringLiteral("persona");
    QByteArray dataset;
    std::set<std::string> prompts;
    for (const auto& entry : m_trainingExamples) {
        if (entry.adapterStage != m_learningStage.toStdString() || entry.baseModel != "Qwen/Qwen3-4B-Base") { continue; }
        if (core::memoryNeedsApproval(entry.prompt) || core::memoryNeedsApproval(entry.preferred)) { continue; }
        prompts.insert(entry.prompt);
        const QJsonObject row{{QStringLiteral("schema_version"), 1},
            {QStringLiteral("reviewed"), entry.source == "reviewed"},
            {QStringLiteral("chat_learning_consent"), true},
            {QStringLiteral("source"), QString::fromStdString(entry.source)},
            {QStringLiteral("base_model"), QStringLiteral("Qwen/Qwen3-4B-Base")},
            {QStringLiteral("adapter_stage"), m_learningStage},
            {QStringLiteral("tool_success"), entry.source == "tool_trace"},
            {QStringLiteral("prompt"), QString::fromStdString(entry.prompt)},
            {QStringLiteral("preferred"), QString::fromStdString(entry.preferred)},
            {QStringLiteral("rejected"), QString::fromStdString(entry.rejected)}};
        dataset += QJsonDocument(row).toJson(QJsonDocument::Compact) + '\n';
    }
    if (prompts.size() < 8) { return; }
    const QString digest = QString::fromLatin1(QCryptographicHash::hash(dataset, QCryptographicHash::Sha256).toHex());
    if (m_learningStage == QStringLiteral("tool")) { m_lastToolLearningDigest = toolDigest; }
    else {
        if (digest == m_lastLearningDigest) { return; }
        m_lastLearningDigest = digest;
    } // Failed data does not create an automatic retry loop.
    const QString unavailable = m_automaticLearning->availabilityError();
    if (!unavailable.isEmpty()) { m_learningNotice = unavailable; emit learningChanged(); return; }
    m_learningModelPath = m_modelPath;
    m_learningPreviousAdapter = QString::fromStdString(local->adapterPath());
    m_backend = std::make_unique<runtime::LlamaCppBackend>(); // Release inference VRAM before PyTorch starts.
    m_worker->setBackend(m_backend.get());
    refreshCachedRuntime();
    emit runtimeChanged();
    m_automaticLearning->start(dataset, m_learningModelPath, m_learningStage);
    emit learningChanged();
}

void AppController::resetLearnedBehavior() {
    if (workerBusy() || m_modelLoadThread) {
        m_learningNotice = tr("Stop the current response or wait for loading before resetting learned behavior.");
        emit learningChanged(); return;
    }
    const auto examples = m_trainingExamples;
    const auto instructions = m_behaviorInstructions;
    const bool enabled = m_automaticLearningEnabled;
    m_automaticLearningEnabled = false;
    m_trainingExamples.clear();
    m_behaviorInstructions.clear();
    if (!persistRecall()) {
        m_trainingExamples = examples;
        m_behaviorInstructions = instructions;
        m_automaticLearningEnabled = enabled;
        m_learningNotice = tr("Reset could not be saved: %1").arg(m_recallError);
        emit learningChanged(); return;
    }
    m_learningPreviousAdapter.clear();
    m_pendingLearnedAdapter.clear();
    if (m_automaticLearning) { m_automaticLearning->cancel(); }
    if (auto* local = dynamic_cast<runtime::LlamaCppBackend*>(m_backend.get())) {
        std::string error;
        if (!local->loadAdapter({}, error)) { m_learningNotice = QString::fromStdString(error); }
    }
    QSettings settings(QStringLiteral("Kestrel"), QStringLiteral("Kestrel"));
    settings.remove(QStringLiteral("learning/tooladapter"));
    settings.remove(QStringLiteral("learning/toolmodel"));
    settings.remove(QStringLiteral("learning/adapter"));
    settings.remove(QStringLiteral("learning/model"));
    settings.sync();
    refreshCachedRuntime();
    rebuildDiagnostics();
    emit runtimeChanged();
    refreshLearnedBehaviorPrompt();
    m_lastLearningDigest.clear();
    m_lastToolLearningDigest.clear();
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/learning");
    if (!learningBusy() && QFileInfo::exists(root) && !QDir(root).removeRecursively()) {
        m_learningNotice = tr("Learning reset, but old local adapter files could not be deleted. Check permissions in %1.").arg(root);
    } else {
        m_learningNotice = tr("Learning stopped; stored examples, taught preferences, and active learned weights reset. Existing exports are unchanged.");
    }
    emit learningChanged();
}

} // namespace kestrel::app
