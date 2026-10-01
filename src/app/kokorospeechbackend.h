#pragma once

#include "app/localmodelspeechbackend.h"

namespace kestrel::app {

// The voice Windows does not have.
//
// Everything else in this app speaks through Qt's text-to-speech, which is
// limited to whatever voices the machine has registered -- on a stock English
// install that is three recordings from the early 2000s, and they are the reason
// an assistant sounds like a navigation system rather than a colleague. No
// better voice can be installed as a system voice, because none of the good ones
// are SAPI engines.
//
// Kokoro sidesteps that: a local model driven over a pipe, with the audio played
// by the app. It is 82 million parameters, Apache-2.0, and synthesises faster
// than real time on the CPU, so a clause-by-clause reply still sounds like
// speech rather than a slideshow. Piper is the other option; both are this
// class with a different model underneath.
class KokoroSpeechBackend final : public LocalModelSpeechBackend {
    Q_OBJECT

public:
    // `python` is the interpreter that has kokoro-onnx installed and
    // `serverScript` the driver. Both are discovered rather than hardcoded, so a
    // machine without the model simply reports that it has no voice.
    //
    // `startImmediately` is for asking what this engine can do without running
    // it. Discovery needs the voice list, and getting it by constructing a
    // working backend would start a process, load a 325MB model, and leave an
    // orphan behind -- all to answer a question about a list of strings.
    KokoroSpeechBackend(QString python, QString serverScript,
                        bool startImmediately = true, QObject* parent = nullptr);

    // The British male voices first: this is the register the voice was chosen
    // for. `bm_george` is the default and what this was tuned around.
    [[nodiscard]] QStringList engineVoices() const override;

protected:
    void startEngine() override;
    void synthesise(const QString& text, const QString& path) override;
    [[nodiscard]] QString engineName() const override { return QStringLiteral("Kokoro"); }

private:
    void onReadyRead();

    QString m_python;
    QString m_serverScript;
    QProcess* m_process = nullptr;
};

} // namespace kestrel::app
