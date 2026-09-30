#pragma once

#include "app/speechsynthesizer.h"

#include <QString>
#include <QStringList>
#include <QVariantList>

#include <memory>
#include <vector>

namespace kestrel::app {

// The local voice engines this machine could speak with, and the wiring to make
// one of them.
//
// Two engines ship in-tree because they trade places. Kokoro has one model with
// many voices and produces them faster than real time, so it sounds better but
// costs more to start. Piper has a separate model per voice and is quicker to
// begin with, which is what makes its wider British catalogue usable. Which one
// is in charge is a runtime choice, because the right answer depends on what has
// been downloaded rather than on anything the code can decide.
//
// Discovery looks on disk and reports what is genuinely there. An engine that
// has not been installed is absent from the list rather than present and
// failing, so the panel never offers a switch that cannot work.
class LocalVoiceEngines {
public:
    struct Engine {
        // Stable identifier used to ask for this engine again.
        QString id;
        QString displayName;
        // Present only when the engine was found; empty when it is not.
        QString executable;
        QString serverScript;
        QString dataDir;
        // The voices this engine can speak with right now.
        QStringList voices;
    };

    // Looks for the engines beside the running binary. `applicationDirPath` is
    // the anchor so a build in a subdirectory still finds the models that live
    // at the root of the worktree.
    [[nodiscard]] static LocalVoiceEngines discover();

    /// Returns a copy of the discovered local engine records.
    [[nodiscard]] std::vector<Engine> engines() const { return m_engines; }
    // The id of the engine that should speak by default: the first one found
    // that actually has voices, because an engine with no models is not a voice.
    [[nodiscard]] QString defaultEngineId() const;
    // Builds the backend for `id`, or nullptr when it is not one of ours.
    [[nodiscard]] std::unique_ptr<SpeechBackend> create(const QString& id) const;

    // For the panel: one entry per engine, each naming its voices.
    [[nodiscard]] QVariantList describe() const;

private:
    std::vector<Engine> m_engines;
};

} // namespace kestrel::app
