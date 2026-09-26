#include "core/conversation.h"
#include "core/voicesession.h"
#include "runtime/mockbackend.h"

#include <cassert>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

using namespace kestrel;

bool eventsContainInOrder(const core::VoiceSession& session,
                          core::ResponseId id,
                          std::initializer_list<core::VoiceEventKind> expected) {
    auto it = expected.begin();
    for (const core::VoiceEvent& event : session.events()) {
        if (it == expected.end()) {
            break;
        }
        if (event.responseId == id && event.kind == *it) {
            ++it;
        }
    }
    return it == expected.end();
}

void testConversationBasics() {
    core::Conversation conversation;
    assert(conversation.size() == 0);
    assert(!conversation.appendToLastMessage("orphan"));
    assert(!conversation.removeLastMessage());

    conversation.addMessage(core::MessageRole::User, "Hello");
    conversation.addMessage(core::MessageRole::Assistant, "Hi");
    assert(conversation.size() == 2);
    assert(conversation.messages().front().content == "Hello");

    assert(conversation.appendToLastMessage(" there"));
    assert(conversation.messages().back().content == "Hi there");

    assert(conversation.removeLastMessage());
    assert(conversation.size() == 1);
    assert(conversation.messages().back().content == "Hello");

    conversation.clear();
    assert(conversation.size() == 0);
}

void testMockBackend() {
    runtime::MockBackend backend;
    assert(backend.status().available);
    assert(backend.status().contextUsed == 0);

    std::string streamed;
    bool completed = false;
    backend.generate({"test", 0.7F, 32},
                     [&streamed](std::string_view token) { streamed += token; },
                     [&completed](bool success, std::string_view) { completed = success; });
    assert(completed);
    assert(streamed.find("local preview response") != std::string::npos);

    const runtime::RuntimeStatus status = backend.status();
    assert(status.contextUsed > 0);
    assert(status.contextUsed <= status.contextLimit);
    assert(status.tokensPerSecond > 0.0);

    const std::size_t usedAfterFirst = status.contextUsed;
    backend.generate({"again", 0.7F, 32}, [](std::string_view) {},
                     [](bool, std::string_view) {});
    assert(backend.status().contextUsed > usedAfterFirst);
}

void testVoiceHappyPathAndEvents() {
    core::VoiceSession session;
    const core::ResponseId id = session.queueResponse("model=demo temperature=0.7");
    assert(id != core::kInvalidResponseId);
    assert(session.find(id) != nullptr);
    assert(session.find(id)->state() == core::ResponseState::Queued);
    assert(session.find(id)->runtimeContext() == "model=demo temperature=0.7");

    const core::GenerationId gen = session.beginGeneration(id);
    assert(gen != core::kInvalidGenerationId);
    assert(session.find(id)->state() == core::ResponseState::Generating);
    assert(session.activeResponseId() == id);

    assert(session.appendText(id, gen, "Kestrels hover while hunting. "));
    assert(session.advancePlayback(id, 10));
    assert(session.find(id)->state() == core::ResponseState::Speaking);
    assert(session.appendText(id, gen, "They see ultraviolet light."));
    assert(session.finishGeneration(id, gen));
    assert(session.find(id)->generationComplete());

    const std::size_t total = session.find(id)->generatedText().size();
    assert(session.advancePlayback(id, total));
    assert(session.find(id)->state() == core::ResponseState::Completed);
    assert(session.activeResponseId() == core::kInvalidResponseId);

    assert(eventsContainInOrder(session, id,
                                {core::VoiceEventKind::ResponseQueued,
                                 core::VoiceEventKind::GenerationStarted,
                                 core::VoiceEventKind::SpeakingStarted,
                                 core::VoiceEventKind::GenerationFinished,
                                 core::VoiceEventKind::ResponseCompleted}));
}

void testBargeInPreservesCutoffAndRejectsStaleOutput() {
    core::VoiceSession session;
    const core::ResponseId id = session.queueResponse();
    const core::GenerationId gen = session.beginGeneration(id);
    assert(session.appendText(id, gen, "Sentence one. Sentence two."));
    assert(session.advancePlayback(id, 14));

    assert(session.interrupt(id, "wait, actually"));
    const core::VoiceResponse* response = session.find(id);
    assert(response->state() == core::ResponseState::Interrupted);
    assert(response->interruptions().size() == 1);
    assert(response->interruptions().front().spokenOffset == 14);
    assert(response->interruptions().front().capturedUserText == "wait, actually");
    assert(!response->interruptions().front().resolved);
    assert(response->spokenText() == "Sentence one. ");
    assert(response->unspokenText() == "Sentence two.");

    const std::string before = response->generatedText();
    assert(!session.appendText(id, gen, " stale tail"));
    assert(session.find(id)->generatedText() == before);
    assert(session.events().back().kind == core::VoiceEventKind::StaleOutputRejected);
    assert(session.events().back().detail == " stale tail");
    assert(!session.finishGeneration(id, gen));
    assert(!session.advancePlayback(id, 20));
}

void testPauseResumeContinuesFromCutoff() {
    core::VoiceSession session;
    const core::ResponseId id = session.queueResponse();
    const core::GenerationId gen = session.beginGeneration(id);
    assert(session.appendText(id, gen, "Alpha beta gamma. Delta epsilon."));
    assert(session.finishGeneration(id, gen));
    assert(session.advancePlayback(id, 18));

    assert(session.interrupt(id, "hold on"));
    const auto resolved = session.resolveInterruption(id, core::InterruptionIntent::Pause);
    assert(resolved.has_value() && *resolved == core::kInvalidGenerationId);
    assert(session.find(id)->state() == core::ResponseState::Paused);
    assert(session.find(id)->interruptions().front().resolved);
    assert(session.find(id)->interruptions().front().intent == core::InterruptionIntent::Pause);
    assert(session.find(id)->unspokenText() == "Delta epsilon.");

    const auto resumed = session.resume(id);
    assert(resumed.has_value() && *resumed == core::kInvalidGenerationId);
    assert(session.find(id)->state() == core::ResponseState::Speaking);
    assert(session.advancePlayback(id, session.find(id)->generatedText().size()));
    assert(session.find(id)->state() == core::ResponseState::Completed);
}

void testResolveResumeIntent() {
    core::VoiceSession session;
    const core::ResponseId id = session.queueResponse();
    const core::GenerationId gen = session.beginGeneration(id);
    assert(session.appendText(id, gen, "Continue right away."));
    assert(session.finishGeneration(id, gen));
    assert(session.advancePlayback(id, 8));

    assert(session.interrupt(id, "sorry, keep going"));
    const auto resumed = session.resolveInterruption(id, core::InterruptionIntent::Resume);
    assert(resumed.has_value() && *resumed == core::kInvalidGenerationId);
    assert(session.find(id)->state() == core::ResponseState::Speaking);
    assert(session.find(id)->spokenOffset() == 8);
    assert(session.advancePlayback(id, session.find(id)->generatedText().size()));
    assert(session.find(id)->state() == core::ResponseState::Completed);
}

void testCorrectionWithProvidedRemainder() {
    core::VoiceSession session;
    const core::ResponseId id = session.queueResponse();
    const core::GenerationId gen = session.beginGeneration(id);
    assert(session.appendText(id, gen,
                              "Paris is the capital of France. It has about two million residents."));
    assert(session.advancePlayback(id, 32));

    assert(session.interrupt(id, "I meant the metro area population"));
    const auto resolved = session.resolveInterruption(
        id, core::InterruptionIntent::Correction,
        "The metro area has about thirteen million residents.");
    assert(resolved.has_value() && *resolved == core::kInvalidGenerationId);

    const core::VoiceResponse* response = session.find(id);
    assert(response->state() == core::ResponseState::Paused);
    assert(response->generationComplete());
    assert(response->spokenText() == "Paris is the capital of France. ");
    assert(response->unspokenText() == "The metro area has about thirteen million residents.");

    const auto resumed = session.resume(id);
    assert(resumed.has_value() && *resumed == core::kInvalidGenerationId);
    assert(session.advancePlayback(id, session.find(id)->generatedText().size()));
    assert(session.find(id)->state() == core::ResponseState::Completed);
}

void testCorrectionWithRegeneration() {
    core::VoiceSession session;
    const core::ResponseId id = session.queueResponse();
    const core::GenerationId staleGen = session.beginGeneration(id);
    assert(session.appendText(id, staleGen, "The answer is four."));
    assert(session.advancePlayback(id, 10));

    assert(session.interrupt(id, "use three plus three instead"));
    const auto resolved = session.resolveInterruption(id, core::InterruptionIntent::Correction);
    assert(resolved.has_value() && *resolved == core::kInvalidGenerationId);
    assert(session.find(id)->state() == core::ResponseState::Paused);
    assert(session.find(id)->generatedText() == "The answer");
    assert(!session.find(id)->generationComplete());

    assert(!session.appendText(id, staleGen, " is four."));
    assert(session.events().back().kind == core::VoiceEventKind::StaleOutputRejected);

    const auto resumed = session.resume(id);
    assert(resumed.has_value() && *resumed != core::kInvalidGenerationId);
    assert(*resumed != staleGen);
    assert(session.find(id)->state() == core::ResponseState::Speaking);

    assert(session.appendText(id, *resumed, " is six because three plus three is six."));
    assert(session.finishGeneration(id, *resumed));
    assert(session.advancePlayback(id, session.find(id)->generatedText().size()));
    assert(session.find(id)->state() == core::ResponseState::Completed);
    assert(session.find(id)->generatedText()
           == "The answer is six because three plus three is six.");
}

void testQuestionDefersAndReturns() {
    core::VoiceSession session;
    const core::ResponseId first = session.queueResponse();
    const core::GenerationId firstGen = session.beginGeneration(first);
    assert(session.appendText(first, firstGen, "First answer part one. Part two."));
    assert(session.advancePlayback(first, 10));

    assert(session.interrupt(first, "quick question first"));
    const auto deferred = session.resolveInterruption(first, core::InterruptionIntent::Question);
    assert(deferred.has_value() && *deferred == core::kInvalidGenerationId);
    assert(session.find(first)->state() == core::ResponseState::Paused);

    const core::ResponseId second = session.queueResponse();
    const core::GenerationId secondGen = session.beginGeneration(second);
    assert(secondGen != core::kInvalidGenerationId);
    assert(!session.resume(first).has_value());

    assert(session.appendText(second, secondGen, "Here is the side answer."));
    assert(session.finishGeneration(second, secondGen));
    assert(session.complete(second));
    assert(session.find(second)->state() == core::ResponseState::Completed);

    const auto resumed = session.resume(first);
    assert(resumed.has_value() && *resumed != core::kInvalidGenerationId);
    assert(session.find(first)->state() == core::ResponseState::Speaking);
    assert(session.appendText(first, *resumed, " Part three."));
    assert(session.finishGeneration(first, *resumed));
    assert(session.advancePlayback(first, session.find(first)->generatedText().size()));
    assert(session.find(first)->state() == core::ResponseState::Completed);
}

void testReplacementDiscardsUnfinishedResponse() {
    core::VoiceSession session;
    const core::ResponseId id = session.queueResponse();
    const core::GenerationId gen = session.beginGeneration(id);
    assert(session.appendText(id, gen, "Old topic sentence. More old topic."));
    assert(session.advancePlayback(id, 20));

    assert(session.interrupt(id, "never mind, new topic"));
    const auto resolved = session.resolveInterruption(id, core::InterruptionIntent::Replacement);
    assert(resolved.has_value() && *resolved == core::kInvalidGenerationId);

    const core::VoiceResponse* response = session.find(id);
    assert(response->state() == core::ResponseState::Cancelled);
    assert(response->isTerminal());
    assert(response->spokenText() == "Old topic sentence. ");
    assert(response->interruptions().front().intent == core::InterruptionIntent::Replacement);
    assert(eventsContainInOrder(session, id,
                                {core::VoiceEventKind::Interrupted,
                                 core::VoiceEventKind::InterruptionResolved,
                                 core::VoiceEventKind::ResponseCancelled}));
    assert(session.activeResponseId() == core::kInvalidResponseId);
}

void testTextOnlyFallbackCompletes() {
    core::VoiceSession session;
    const core::ResponseId id = session.queueResponse();
    const core::GenerationId gen = session.beginGeneration(id);
    assert(session.appendText(id, gen, "Delivered as text only."));
    assert(!session.complete(id));
    assert(session.finishGeneration(id, gen));
    assert(session.complete(id));
    assert(session.find(id)->state() == core::ResponseState::Completed);
    assert(session.find(id)->spokenOffset() == 0);
}

void testFailureRecovery() {
    core::VoiceSession session;
    const core::ResponseId first = session.queueResponse();
    const core::GenerationId firstGen = session.beginGeneration(first);
    assert(session.appendText(first, firstGen, "Speaking before the device failed."));
    assert(session.advancePlayback(first, 8));
    assert(session.fail(first, "audio device lost"));
    assert(session.find(first)->state() == core::ResponseState::Failed);
    assert(session.find(first)->error() == "audio device lost");
    assert(session.find(first)->generatedText() == "Speaking before the device failed.");
    assert(session.activeResponseId() == core::kInvalidResponseId);
    assert(!session.fail(first, "twice"));

    const core::ResponseId second = session.queueResponse();
    const core::GenerationId secondGen = session.beginGeneration(second);
    assert(secondGen != core::kInvalidGenerationId);
    assert(session.pause(second, "output device removed"));
    assert(session.find(second)->state() == core::ResponseState::Paused);

    const auto resumed = session.resume(second);
    assert(resumed.has_value() && *resumed != core::kInvalidGenerationId);
    assert(session.find(second)->state() == core::ResponseState::Generating);
    assert(session.appendText(second, *resumed, "Recovered as text."));
    assert(session.finishGeneration(second, *resumed));
    assert(session.complete(second));
}

void testInvariantsAndBounds() {
    core::VoiceSession session;
    const core::ResponseId id = session.queueResponse();
    const core::GenerationId gen = session.beginGeneration(id);
    assert(session.appendText(id, gen, "abc"));

    assert(!session.advancePlayback(id, 4));
    assert(session.advancePlayback(id, 2));
    assert(!session.advancePlayback(id, 1));
    assert(session.advancePlayback(id, 2));

    assert(session.beginGeneration(id) == core::kInvalidGenerationId);
    const core::ResponseId other = session.queueResponse();
    assert(session.beginGeneration(other) == core::kInvalidGenerationId);

    assert(!session.appendText(id, core::kInvalidGenerationId, "x"));
    assert(!session.resolveInterruption(id, core::InterruptionIntent::Pause).has_value());
    assert(!session.resume(id).has_value());
    assert(!session.interrupt(other, "not active"));
    assert(!session.complete(id));

    assert(session.advancePlayback(id, 3));
    assert(session.finishGeneration(id, gen));
    assert(session.find(id)->state() == core::ResponseState::Completed);
    assert(!session.cancel(id));
    assert(session.cancel(other));
    assert(!session.interrupt(id, "already finished"));

    assert(session.find(core::ResponseId{9999}) == nullptr);
    assert(session.beginGeneration(core::ResponseId{9999}) == core::kInvalidGenerationId);
}

void testSentenceStartBefore() {
    const std::string text = "Hello world. Second sentence here.";
    assert(core::sentenceStartBefore(text, 5) == 0);
    assert(core::sentenceStartBefore(text, 20) == 13);
    assert(core::sentenceStartBefore(text, 13) == 13);
    assert(core::sentenceStartBefore("line one\nline two", 12) == 9);
    assert(core::sentenceStartBefore("Hi.", 99) == 3);
    assert(core::sentenceStartBefore("", 4) == 0);
    assert(core::sentenceStartBefore("no terminator at all", 20) == 0);
}

} // namespace

int main() {
    testConversationBasics();
    testMockBackend();
    testVoiceHappyPathAndEvents();
    testBargeInPreservesCutoffAndRejectsStaleOutput();
    testPauseResumeContinuesFromCutoff();
    testResolveResumeIntent();
    testCorrectionWithProvidedRemainder();
    testCorrectionWithRegeneration();
    testQuestionDefersAndReturns();
    testReplacementDiscardsUnfinishedResponse();
    testTextOnlyFallbackCompletes();
    testFailureRecovery();
    testInvariantsAndBounds();
    testSentenceStartBefore();
    return 0;
}
