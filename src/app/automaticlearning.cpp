#include "app/automaticlearning.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>

#include <cmath>

namespace kestrel::app {
namespace {
QString existing(const QStringList& candidates) {
    for (const auto& candidate : candidates) {
        if (QFileInfo(candidate).isFile()) { return QFileInfo(candidate).absoluteFilePath(); }
    }
    return {};
}
QString firstDirectory(const QStringList& roots, const QString& marker) {
    for (const auto& root : roots) {
        if (QFileInfo::exists(root + QLatin1Char('/') + marker)) { return QDir(root).absolutePath(); }
    }
    return {};
}
} // namespace

AutomaticLearning::AutomaticLearning(QObject* parent) : QObject(parent) {
    const QString app = QCoreApplication::applicationDirPath();
    const QString source = QString::fromUtf8(KESTREL_SOURCE_DIR);
    const QString data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QStringList roots{app, app + QStringLiteral("/.."), source, data};
    QStringList python, trainer, models, configs;
    for (const auto& root : roots) {
#ifdef _WIN32
        python << root + QStringLiteral("/training-venv/Scripts/python.exe")
               << root + QStringLiteral("/build/training-venv/Scripts/python.exe");
#else
        python << root + QStringLiteral("/training-venv/bin/python")
               << root + QStringLiteral("/build/training-venv/bin/python");
#endif
        trainer << root + QStringLiteral("/scripts/train_persona.py");
        for (const auto& sub : {QStringLiteral("/models/"), QStringLiteral("/build/models/")}) {
            models << root + sub + QStringLiteral("Qwen3-4B-Base-bnb-4bit");
            configs << root + sub + QStringLiteral("Qwen3-4B-Base-config");
        }
    }
    m_python = existing(python);
    m_trainer = existing(trainer);
    m_model = firstDirectory(models, QStringLiteral("model.safetensors"));
    m_config = firstDirectory(configs, QStringLiteral("config.json"));
    m_converter = existing({app + QStringLiteral("/training/convert_lora_to_gguf.py"),
        source + QStringLiteral("/third_party/llama.cpp/convert_lora_to_gguf.py"),
        QDir::homePath() + QStringLiteral("/.local/src/llama.cpp/convert_lora_to_gguf.py")});
    connect(&m_process, &QProcess::readyReadStandardOutput, this, [this] {
        m_log += m_process.readAllStandardOutput();
        if (m_log.size() > 16384) { m_log = m_log.right(16384); }
    });
    connect(&m_process, &QProcess::finished, this, &AutomaticLearning::processFinished);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (m_busy && error == QProcess::FailedToStart) {
            complete({}, tr("Local learning could not start: %1. Repair the installed training environment.").arg(m_process.errorString()));
        }
    });
}

AutomaticLearning::~AutomaticLearning() {
    const bool interrupted = m_busy;
    disconnect(&m_process, nullptr, this, nullptr);
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(5000);
    }
    if (interrupted && !m_output.isEmpty()) {
        QDir(m_output).removeRecursively();
        QFile::remove(m_output + QStringLiteral(".gguf"));
        QFile::remove(m_output + QStringLiteral(".evaluation.json"));
    }
}

QString AutomaticLearning::availabilityError() const {
    if (m_python.isEmpty() || m_trainer.isEmpty() || m_model.isEmpty() || m_config.isEmpty() || m_converter.isEmpty()) {
        return tr("Learning is collecting chat examples; automatic weights need the installed local training environment, Qwen3 Base training weights/config, and llama.cpp converter. No downloads or cloud fallback were started.");
    }
    return {};
}

void AutomaticLearning::start(const QByteArray& dataset, const QString& inferenceModel, const QString& stage) {
    if (m_busy) { return; }
    if (stage != QStringLiteral("persona") && stage != QStringLiteral("tool")) { complete({}, tr("Unknown adapter training stage.")); return; }
    m_stage = stage;
    const QString error = availabilityError();
    if (!error.isEmpty()) { complete({}, error); return; }
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/learning");
    if (!QDir().mkpath(root) || !QFile::setPermissions(root, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
        complete({}, tr("Could not create an owner-only local learning directory.")); return;
    }
    m_dataset = std::make_unique<QTemporaryDir>(root + QStringLiteral("/dataset-XXXXXX"));
    QFile file(m_dataset->path() + QStringLiteral("/chat.jsonl"));
    if (!m_dataset->isValid() || !file.open(QIODevice::WriteOnly)
        || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        || file.write(dataset) != dataset.size()) {
        complete({}, tr("Could not create the private temporary training dataset.")); return;
    }
    file.close();
    m_output = root + QStringLiteral("/adapter-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_busy = true;
    m_cancelled = false;
    m_converting = false;
    m_log.clear();
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto& key : environment.keys()) {
        if (key.contains(QStringLiteral("TOKEN"), Qt::CaseInsensitive)
            || key.contains(QStringLiteral("API_KEY"), Qt::CaseInsensitive)) { environment.remove(key); }
    }
    environment.insert(QStringLiteral("HF_HUB_OFFLINE"), QStringLiteral("1"));
    environment.insert(QStringLiteral("TRANSFORMERS_OFFLINE"), QStringLiteral("1"));
    environment.insert(QStringLiteral("WANDB_DISABLED"), QStringLiteral("true"));
    m_process.setProcessEnvironment(environment);
    m_process.setProcessChannelMode(QProcess::MergedChannels);
    m_status = tr("Learning locally: validating held-out examples and training an astronomy candidate. Chat resumes automatically when finished.");
    emit changed();
    QStringList arguments{m_trainer, QStringLiteral("--dataset"), file.fileName(),
        QStringLiteral("--model"), m_model, QStringLiteral("--output"), m_output,
        QStringLiteral("--steps"), QStringLiteral("8"), QStringLiteral("--max-length"), QStringLiteral("512"),
        QStringLiteral("--rank"), QStringLiteral("4"), QStringLiteral("--approve-training"),
        QStringLiteral("--allow-consented-chat"), QStringLiteral("--automatic-evaluation"),
        QStringLiteral("--adapter-stage"), m_stage,
        QStringLiteral("--inference-model"), inferenceModel};
    if (m_stage == QStringLiteral("persona")) {
        arguments << QStringLiteral("--domain") << QStringLiteral("astronomy") << QStringLiteral("--include-seeds");
    } else {
        arguments << QStringLiteral("--include-tool-seeds");
    }
    m_status = tr("Learning locally: training the dedicated %1 adapter against held-out examples.").arg(m_stage);
    m_process.start(m_python, arguments);
    QTimer::singleShot(300000, this, [this, output = m_output] {
        if (m_busy && m_output == output) { m_cancelled = true; m_process.kill(); }
    });
}

void AutomaticLearning::cancel() {
    if (!m_busy) { return; }
    m_cancelled = true;
    m_status = tr("Stopping local learning and restoring the chat model…");
    emit changed();
    m_process.kill();
}

void AutomaticLearning::processFinished(int code, QProcess::ExitStatus exitStatus) {
    if (!m_busy) { return; }
    m_log += m_process.readAllStandardOutput();
    if (m_cancelled) { complete({}, tr("Learning stopped; previous behavior restored.")); return; }
    if (code != 0 || exitStatus != QProcess::NormalExit) {
        complete({}, tr("Local learning failed; previous behavior is unchanged. %1").arg(QString::fromUtf8(m_log.right(2000))));
        return;
    }
    if (!m_converting) {
        QFile report(m_output + QStringLiteral("/evaluation.json"));
        const bool readable = report.open(QIODevice::ReadOnly);
        const QJsonObject evaluation = readable ? QJsonDocument::fromJson(report.readAll()).object() : QJsonObject{};
        report.close(); // Windows must release the handle before rejected files are removed.
        if (evaluation.isEmpty()) {
            complete({}, tr("Candidate produced no readable evaluation record; previous behavior retained.")); return;
        }
        QSaveFile saved(QFileInfo(m_output).absolutePath() + QStringLiteral("/last-evaluation.json"));
        const QByteArray summary = QJsonDocument(evaluation).toJson();
        if (!saved.open(QIODevice::WriteOnly)
            || !saved.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
            || saved.write(summary) != summary.size() || !saved.commit()) {
            complete({}, tr("Could not save a private evaluation record; candidate not activated. Check learning-directory permissions.")); return;
        }
        if (!evaluation.value(QStringLiteral("automatic_activation_eligible")).toBool()) {
            QDir(m_output).removeRecursively();
            complete({}, tr("Candidate rejected by held-out loss/behavior checks; previous behavior retained. More teaching examples are needed."));
            return;
        }
        m_converting = true;
        m_status = tr("Candidate passed local checks; preparing its adapter automatically…");
        m_log.clear();
        emit changed();
        m_process.start(m_python, {m_converter, QStringLiteral("--base"), m_config,
            QStringLiteral("--outfile"), m_output + QStringLiteral(".gguf"), QStringLiteral("--outtype"),
            QStringLiteral("f16"), m_output});
        return;
    }
    const QString adapter = m_output + QStringLiteral(".gguf");
    if (!QFileInfo(adapter).isFile() || QFileInfo(adapter).size() < 32) {
        complete({}, tr("Local conversion did not produce an adapter; previous behavior retained.")); return;
    }
    if (!QFile::setPermissions(adapter, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        QFile::remove(adapter);
        complete({}, tr("Could not protect the learned adapter; previous behavior retained.")); return;
    }
    // Preserve this candidate's metrics, not its plaintext dataset or tokenizer.
    const QString evaluation = m_output + QStringLiteral(".evaluation.json");
    if (!QFile::copy(m_output + QStringLiteral("/evaluation.json"), evaluation)
        || !QFile::setPermissions(evaluation, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        complete({}, tr("Could not retain the candidate's private evaluation record; previous behavior retained.")); return;
    }
    QDir(m_output).removeRecursively(); // PEFT weights/tokenizer are disposable.
    complete(adapter, tr("Local candidate passed automated checks. These checks do not guarantee general chat quality."));
}

void AutomaticLearning::complete(const QString& adapter, const QString& message) {
    if (adapter.isEmpty() && !m_output.isEmpty()) {
        QDir(m_output).removeRecursively();
        QFile::remove(m_output + QStringLiteral(".gguf"));
        QFile::remove(m_output + QStringLiteral(".evaluation.json"));
    }
    m_output.clear(); // A later failed start must never remove an accepted adapter.
    m_busy = false;
    m_dataset.reset();
    m_status = message;
    emit changed();
    emit finished(adapter, message);
}

} // namespace kestrel::app
