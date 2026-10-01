#include "app/localvoiceengines.h"

#if KESTREL_HAS_QT_MULTIMEDIA
#include "app/kokorospeechbackend.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QVariantMap>
#endif

namespace kestrel::app {

#if KESTREL_HAS_QT_MULTIMEDIA

LocalVoiceEngines LocalVoiceEngines::discover() {
    LocalVoiceEngines result;
    // One level up from the binary, which is where the models and the driver
    // scripts live: the build directory is a subdirectory of the worktree, and
    // a developer who moved the executable would want it found in both places.
    const QStringList roots = {
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("..")),
        QCoreApplication::applicationDirPath(),
    };

    for (const QString& root : roots) {
        const QString python = QDir(root).filePath(
#ifdef Q_OS_WIN
            QStringLiteral(".kestrel-voice/Scripts/python.exe"));
#else
            QStringLiteral(".kestrel-voice/bin/python"));
#endif
        const QString kokoro = QDir(root).filePath(
            QStringLiteral("tools/kokoro_voice_server.py"));
        const bool haveKokoro = QFileInfo::exists(python) && QFileInfo::exists(kokoro)
            && QFileInfo::exists(QDir(root).filePath(
                QStringLiteral(".kestrel-voice/models/kokoro-v1.0.onnx")));
        if (haveKokoro) {
            Engine engine;
            engine.id = QStringLiteral("kokoro");
            engine.displayName = QStringLiteral("Kokoro");
            engine.executable = python;
            engine.serverScript = kokoro;
            // Built once, purely to ask which voices it can speak. Discarded
            // immediately: the engine that is actually used is built later.
            // Asked without starting anything: constructing a working engine
            // here would spawn a process and load the model, and then discard
            // it, all to read a list of names.
            engine.voices = KokoroSpeechBackend(python, kokoro, false).engineVoices();
            if (!engine.voices.isEmpty()) {
                result.m_engines.push_back(engine);
            }
        }

        // Piper is deliberately absent. It is a second engine with a wider
        // British catalogue, and it was built and measured: about a third of a
        // second per clause against Kokoro's one to three, with one model per
        // voice instead of one model with many. It is not wired in because a
        // second engine in the panel is a maintenance surface nobody is using
        // yet. The seam for it -- LocalVoiceEngines, and the base class both
        // engines would share -- is here and tested, so adding it back is a
        // subclass and a block of discovery rather than a change of design.
    }
    return result;
}

QString LocalVoiceEngines::defaultEngineId() const {
    for (const Engine& engine : m_engines) {
        if (!engine.voices.isEmpty()) {
            return engine.id;
        }
    }
    return {};
}

std::unique_ptr<SpeechBackend> LocalVoiceEngines::create(const QString& id) const {
    for (const Engine& engine : m_engines) {
        if (engine.id != id) {
            continue;
        }
        if (id == QLatin1String("kokoro")) {
            return std::make_unique<KokoroSpeechBackend>(engine.executable,
                                                         engine.serverScript);
        }
    }
    return nullptr;
}

QVariantList LocalVoiceEngines::describe() const {
    QVariantList result;
    for (const Engine& engine : m_engines) {
        QVariantMap entry;
        entry.insert(QStringLiteral("id"), engine.id);
        entry.insert(QStringLiteral("name"), engine.displayName);
        entry.insert(QStringLiteral("voices"), engine.voices);
        result.append(entry);
    }
    return result;
}

#else

LocalVoiceEngines LocalVoiceEngines::discover() { return {}; }
QString LocalVoiceEngines::defaultEngineId() const { return {}; }
std::unique_ptr<SpeechBackend> LocalVoiceEngines::create(const QString&) const { return nullptr; }
QVariantList LocalVoiceEngines::describe() const { return {}; }

#endif

} // namespace kestrel::app
