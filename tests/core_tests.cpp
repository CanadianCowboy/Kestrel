#include "core/conversation.h"
#include "core/idlepersona.h"
#include "core/idletool.h"
#include "core/persona.h"
#include "core/presence.h"
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

// Exercises the mock backend's generation, context accounting, and the
// (deliberate) absence of a fabricated throughput figure.
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
    // The backend deliberately reports no throughput figure. A hardcoded rate
    // would put a number in the UI that nothing produced, and the app measures
    // real throughput from delivered tokens instead. This assertion exists to
    // fail loudly if a fabricated value is reintroduced.
    assert(status.tokensPerSecond == 0.0);

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

// --- Personality layer -------------------------------------------------------

// The presence line is the whole persona as far as the model is concerned, and
// it is the one part of the prompt the backend keeps cached. So it has to be
// derived from the fixed tone profile and nothing that moves.
void testPersonaPresenceLineIsStable() {
    core::Persona persona;
    const std::string line = persona.presenceLine();
    assert(line.find("You are Kestrel") != std::string::npos);
    assert(line.find("calm") != std::string::npos);
    assert(persona.systemPromptFragment() == line);

    persona.drift(0.4F, 0.4F, 0.4F, -0.4F, 0.4F);
    assert(persona.presenceLine() == line);

    core::ToneProfile wry;
    wry.wry = true;
    persona.setTone(wry);
    assert(persona.presenceLine() != line);
    assert(persona.presenceLine().find("wit") != std::string::npos);
    assert(persona.tone().wry);
}

void testPersonaDialsStayInRange() {
    core::Persona persona;
    persona.drift(5.0F, 5.0F, 5.0F, 5.0F, 5.0F);
    assert(persona.state().focus == 1.0F);
    persona.drift(-5.0F, -5.0F, -5.0F, -5.0F, -5.0F);
    assert(persona.state().calmness == 0.0F);

    core::PersonaState wild;
    wild.focus = 4.0F;
    wild.curiosity = -3.0F;
    persona.setState(wild);
    assert(persona.state().focus == 1.0F);
    assert(persona.state().curiosity == 0.0F);

    // The mood label is a shared decision: the same dials must read the same way
    // to the persona, the presence engine, and the idle loop.
    assert(core::moodFor(persona.state()) == persona.mood());
    assert(core::toString(core::PersonaMood::Contemplative) == std::string("contemplative"));
}

void testPersonaAcknowledgementRotates() {
    core::Persona persona;
    std::vector<std::string> cues;
    for (int i = 0; i < 4; ++i) {
        cues.push_back(persona.acknowledgement());
        assert(!cues.back().empty());
    }
    for (std::size_t i = 0; i < cues.size(); ++i) {
        for (std::size_t j = i + 1; j < cues.size(); ++j) {
            assert(cues[i] != cues[j]);
        }
    }
    assert(persona.sequence() == 4);
    // Rotation rather than a random draw, so the cycle is reproducible.
    assert(persona.acknowledgement() == cues.front());
    assert(persona.acknowledgementPauseMs() > 0);
}

void testPersonaAnticipation() {
    core::Persona persona;

    const auto offer = persona.react(core::PersonaTrigger::LongResponse);
    assert(offer.has_value());
    assert(offer->kind == core::AnticipationKind::OfferContinue);
    assert(offer->text == "Would you like me to continue?");

    const auto correction = persona.react(core::PersonaTrigger::UserCorrection);
    assert(correction.has_value());
    assert(correction->kind == core::AnticipationKind::AcknowledgeCorrection);

    const auto done = persona.react(core::PersonaTrigger::TaskCompleted);
    assert(done.has_value());
    assert(done->text == "Task complete.");

    const auto back = persona.react(core::PersonaTrigger::UserReturned);
    assert(back.has_value());
    assert(back->kind == core::AnticipationKind::ReadyWhenYouAre);

    // An ordinary short answer only earns a closing prompt from an assistant
    // with the initiative to mean it.
    assert(!persona.react(core::PersonaTrigger::TurnCompleted).has_value());
    persona.drift(0.0F, 0.0F, 0.2F, 0.0F, 0.0F);
    assert(persona.react(core::PersonaTrigger::TurnCompleted).has_value());

    // States the UI already shows are not repeated as a line.
    assert(!persona.react(core::PersonaTrigger::UserPaused).has_value());
    assert(!persona.react(core::PersonaTrigger::UserResumed).has_value());
    assert(!persona.react(core::PersonaTrigger::UserInterruption).has_value());

    core::ToneProfile plain;
    plain.anticipatory = false;
    persona.setTone(plain);
    assert(!persona.react(core::PersonaTrigger::LongResponse).has_value());
}

void testPersonaStatusWhispers() {
    core::Persona persona;
    assert(persona.statusWhisper(core::PersonaActivity::StandingBy) == "Standing by\u2026");
    assert(persona.statusWhisper(core::PersonaActivity::Listening) == "Listening\u2026");
    assert(persona.statusWhisper(core::PersonaActivity::Thinking) == "Thinking\u2026");
    assert(persona.statusWhisper(core::PersonaActivity::Speaking) == "Speaking\u2026");
    assert(persona.statusWhisper(core::PersonaActivity::Reflecting) == "Reflecting\u2026");
    assert(persona.statusWhisper(core::PersonaActivity::Preparing)
           == "Preparing response\u2026");
}

// Per-session continuity. Not storage: nothing is written anywhere, and the
// point is that the idle loop and the greeting have something concrete to refer
// to instead of inventing context.
void testPersonaSessionTopic() {
    core::Persona persona;
    assert(persona.sessionTopic().empty());
    assert(persona.turnCount() == 0);

    persona.noteUserMessage("How do I configure the TensorRT backend?");
    assert(persona.turnCount() == 1);
    const std::string topic = persona.sessionTopic();
    assert(topic.find("tensorrt") != std::string::npos);
    assert(topic.find("backend") != std::string::npos);
    assert(topic.find("the") == std::string::npos);

    for (int i = 0; i < 8; ++i) {
        persona.noteUserMessage("another question about caching");
    }
    const std::string later = persona.sessionTopic();
    assert(later.find("tensorrt") == std::string::npos);
    assert(later.find("caching") != std::string::npos);

    // A blank message is still a turn, it just contributes no topic.
    persona.noteUserMessage("   ");
    assert(persona.turnCount() == 10);
}

// --- Idle loop ---------------------------------------------------------------

void testIdleStaysSilentWhileTheUserIsPresent() {
    core::Persona persona;
    core::IdlePersona idle(persona);

    core::IdleGate gate;
    gate.generating = true;
    idle.setGate(gate);
    assert(!idle.tick(0).produced);
    // Even after a long silence, while the gate says someone is here.
    assert(!idle.tick(600000).produced);
    assert(idle.cycles() == 0);
    assert(idle.history().empty());

    gate.generating = false;
    gate.voiceActive = true;
    idle.setGate(gate);
    assert(!idle.tick(900000).produced);

    gate.voiceActive = false;
    gate.userInputPending = true;
    idle.setGate(gate);
    assert(!idle.tick(1200000).produced);
    assert(idle.cycles() == 0);
}

void testIdleDefaultPolicyIsLocalOnly() {
    core::IdlePolicy policy;
    // Everything that is just a string is allowed; the one capability that
    // leaves pure computation is not.
    assert(policy.permittedCount() == 6);
    assert(!policy.permits(core::IdleTaskKind::ModelWarmup));
    assert(policy.permits(core::IdleTaskKind::SelfReflection));
    assert(policy.permits(core::IdleTaskKind::AmbientWhisper));
    assert(policy.permits(core::IdleTaskKind::ContextReindex));
    assert(policy.permits(core::IdleTaskKind::CacheAudit));
    assert(policy.permits(core::IdleTaskKind::CreativeThought));
    assert(policy.permits(core::IdleTaskKind::GreetingPrep));

    // And a policy that grants nothing produces nothing, forever. This is the
    // state a caller reaches by switching every capability off, and it is the
    // assertion that the boundary is real rather than advisory.
    core::IdlePolicy none;
    none.allowSelfReflection = false;
    none.allowAmbientWhisper = false;
    none.allowContextReindex = false;
    none.allowCacheAudit = false;
    none.allowCreativeThoughts = false;
    none.allowGreetingPrep = false;
    none.allowModelWarmup = false;
    assert(none.permittedCount() == 0);

    core::Persona persona;
    core::IdlePersona idle(persona);
    idle.setPolicy(none);
    assert(!idle.tick(1).produced);
    assert(!idle.tick(500000).produced);
    assert(idle.cycles() == 0);
}

void testIdleOnlyProducesPermittedWork() {
    core::Persona persona;
    core::IdlePersona idle(persona);

    core::IdlePolicy policy;
    policy.allowCreativeThoughts = false;
    policy.allowGreetingPrep = false;
    policy.allowCacheAudit = false;
    policy.allowModelWarmup = false;
    idle.setPolicy(policy);
    idle.setTopic("tensorrt");

    std::uint64_t now = 1000;
    static_cast<void>(idle.tick(now)); // anchors the clock
    int produced = 0;
    for (int i = 0; i < 40; ++i) {
        now += 20000;
        const core::IdleTick tick = idle.tick(now);
        if (!tick.produced) {
            continue;
        }
        ++produced;
        assert(idle.policy().permits(tick.task.kind));
        assert(tick.task.kind == core::IdleTaskKind::SelfReflection
               || tick.task.kind == core::IdleTaskKind::AmbientWhisper
               || tick.task.kind == core::IdleTaskKind::ContextReindex);
        assert(!tick.task.detail.empty());
        // Only the whisper is meant for the interface; thoughts stay internal.
        if (tick.task.kind == core::IdleTaskKind::AmbientWhisper) {
            assert(!tick.whisper.empty() && tick.thought.empty());
        }
    }
    assert(produced > 0);
    assert(idle.cycles() == static_cast<std::size_t>(produced));
    // Bounded: a process left running for a week holds the same handful.
    assert(idle.history().size() <= 16);
    assert(idle.nextTickAt() != 0);
}

void testIdleDriftsDialsButNeverRamps() {
    core::Persona persona;
    core::IdlePersona idle(persona);
    idle.setIntervalMs(100);
    idle.setQuietPeriodMs(100);
    static_cast<void>(idle.tick(0)); // anchors the clock

    const float curiosityBefore = persona.state().curiosity;
    std::uint64_t now = 0;
    for (int i = 0; i < 800; ++i) {
        now += 1000;
        static_cast<void>(idle.tick(now));
    }
    const core::PersonaState& state = persona.state();
    for (const float dial : {state.focus, state.curiosity, state.initiative,
                             state.calmness, state.presenceIntensity}) {
        assert(dial >= 0.05F && dial <= 0.95F);
    }
    // The decay has to win over the bumps. An assistant that only ever gained
    // curiosity and initiative would, given enough idle time, become someone
    // nobody wants to talk to.
    assert(state.curiosity <= curiosityBefore);
    // One set of numbers, two readers: the loop and the persona cannot drift
    // apart because there is only one copy.
    assert(&idle.state() == &persona.state());
}

void testIdleGreetsOncePerAbsence() {
    core::Persona persona;
    core::IdlePersona idle(persona);

    core::IdlePolicy greetings;
    greetings.allowSelfReflection = false;
    greetings.allowAmbientWhisper = false;
    greetings.allowContextReindex = false;
    greetings.allowCacheAudit = false;
    greetings.allowCreativeThoughts = false;
    idle.setPolicy(greetings);
    idle.setTopic("tensorrt");

    static_cast<void>(idle.tick(0));    // anchors the clock
    assert(!idle.tick(1000).produced);   // still inside the quiet period
    assert(idle.tick(7000).produced);
    assert(idle.hasGreeting());
    assert(idle.history().back().detail.find("tensorrt") != std::string::npos);

    // A greeting waits for a real absence, not just any idle moment.
    assert(!idle.userReturned(20000, 45000));
    assert(idle.userReturned(60000, 45000));

    const std::string greeting = idle.takeGreeting();
    assert(!greeting.empty());
    assert(!idle.hasGreeting());
    // Once per absence. Every tick after this staying silent is what makes the
    // greeting feel like it noticed rather than nagged.
    assert(idle.takeGreeting().empty());
    assert(!idle.userReturned(61000, 45000));

    // And a second one is not even prepared while the absence is unended.
    std::uint64_t later = 61000;
    for (int i = 0; i < 6; ++i) {
        later += 20000;
        static_cast<void>(idle.tick(later));
    }
    assert(!idle.hasGreeting());

    // Activity ends the absence, and the next one is greeted again.
    idle.noteActivity(70000);
    assert(!idle.userReturned(70000, 45000));
    assert(idle.idleForMs(70000) == 0);
}

void testIdleStopsWhenDisabled() {
    core::Persona persona;
    core::IdlePersona idle(persona);
    idle.setEnabled(false);
    assert(!idle.enabled());
    assert(!idle.tick(0).produced);
    assert(!idle.tick(900000).produced);
    assert(!idle.userReturned(900000, 1));
    assert(idle.cycles() == 0);
}

// --- Idle tool registry ------------------------------------------------------

core::IdleToolDeclaration indexingTool() {
    core::IdleToolDeclaration declaration;
    declaration.name = "index recent threads";
    declaration.summary = "Summarise recent conversations into a local index.";
    declaration.permissions = {core::ToolPermission::ReadConversations,
                                core::ToolPermission::RunGeneration};
    return declaration;
}

void testUndeclaredToolIsRefused() {
    core::IdleToolRegistry registry;
    // Nothing is known about a tool that was never declared, so every
    // capability it might need counts as outstanding. Returning an empty list
    // would read as permission granted.
    assert(!registry.permits("index recent threads"));
    assert(registry.missing("index recent threads").size() == 4);
    assert(registry.find("index recent threads") == nullptr);
    assert(registry.tools().empty());
}

void testDeclaredToolIsRefusedUntilEnabled() {
    core::IdleToolRegistry registry;
    registry.declare(indexingTool());
    assert(registry.find("index recent threads") != nullptr);
    // Declared and fully granted is still not enough: a tool has to be asked for.
    registry.grant(core::ToolPermission::ReadConversations, true);
    registry.grant(core::ToolPermission::RunGeneration, true);
    assert(!registry.enabled("index recent threads"));
    assert(!registry.permits("index recent threads"));
    // A tool that declares nothing still needs to be switched on, or an opt-in
    // registry would run anything that happened to ask for nothing.
    core::IdleToolDeclaration inert;
    inert.name = "warm the cache";
    inert.summary = "Touch a file already in memory.";
    registry.declare(inert);
    registry.grant(core::ToolPermission::ReadConversations, true);
    registry.grant(core::ToolPermission::RunGeneration, true);
    assert(registry.missing("warm the cache").empty());
    assert(!registry.permits("warm the cache"));
    registry.setEnabled("warm the cache", true);
    assert(registry.permits("warm the cache"));
}

void testMissingPermissionIsNamed() {
    core::IdleToolRegistry registry;
    registry.declare(indexingTool());
    registry.setEnabled("index recent threads", true);
    assert(!registry.permits("index recent threads"));

    registry.grant(core::ToolPermission::ReadConversations, true);
    assert(!registry.permits("index recent threads"));
    const std::vector<core::ToolPermission> outstanding =
        registry.missing("index recent threads");
    assert(outstanding.size() == 1);
    assert(outstanding.front() == core::ToolPermission::RunGeneration);

    registry.grant(core::ToolPermission::RunGeneration, true);
    assert(registry.missing("index recent threads").empty());
    assert(registry.permits("index recent threads"));

    // Revoking a grant takes effect immediately rather than at the next request.
    registry.grant(core::ToolPermission::RunGeneration, false);
    assert(!registry.permits("index recent threads"));
    assert(registry.missing("index recent threads").size() == 1);
}

void testNothingIsGrantedByDefault() {
    core::IdleToolRegistry registry;
    // Not even the capabilities that sound harmless. The user grants them, or
    // nobody does.
    for (const core::ToolPermission permission :
         {core::ToolPermission::ReadConversations, core::ToolPermission::RunGeneration,
          core::ToolPermission::WriteFiles, core::ToolPermission::Network}) {
        assert(!registry.granted(permission));
    }
    assert(std::string(core::toString(core::ToolPermission::WriteFiles))
           == "write files");
    assert(std::string(core::toString(core::ToolPermission::Network)) == "network");
}

void testReplacedToolMustBeAgreedAgain() {
    core::IdleToolRegistry registry;
    registry.declare(indexingTool());
    registry.setEnabled("index recent threads", true);
    registry.grant(core::ToolPermission::ReadConversations, true);
    registry.grant(core::ToolPermission::RunGeneration, true);
    assert(registry.permits("index recent threads"));

    // Same name, different job: it now reaches the network as well. Whatever
    // the user agreed to was the previous tool, so the new one starts off.
    core::IdleToolDeclaration widened = indexingTool();
    widened.summary = "Summarise recent conversations and publish the index.";
    widened.permissions = {core::ToolPermission::ReadConversations,
                            core::ToolPermission::RunGeneration,
                            core::ToolPermission::Network};
    registry.declare(widened);

    assert(registry.tools().size() == 1);
    assert(!registry.enabled("index recent threads"));
    assert(!registry.permits("index recent threads"));
    assert(registry.missing("index recent threads").size() == 1);
    assert(registry.missing("index recent threads").front() == core::ToolPermission::Network);
    // The grants the user gave are still grants; they are not taken away.
    assert(registry.granted(core::ToolPermission::RunGeneration));
}

// --- Presence engine ---------------------------------------------------------

void testPresenceTracksMeaning() {
    core::Presence presence;
    presence.setNow(1000);
    assert(!presence.speaking());
    assert(!presence.busy());
    assert(presence.activity() == core::PersonaActivity::StandingBy);

    presence.noteUserAction(core::UserAction::Typed);
    assert(presence.snapshot().lastUserAction == core::UserAction::Typed);
    assert(presence.snapshot().lastChangeMs == 1000);
    // Repeating the same action is not a change, and must not fake one.
    presence.setNow(1200);
    presence.noteUserAction(core::UserAction::Typed);
    assert(presence.snapshot().lastChangeMs == 1000);

    presence.noteAssistantAction(core::AssistantAction::Thinking);
    presence.setVoiceState(core::ResponseState::Speaking);
    assert(presence.speaking());
    assert(presence.activity() == core::PersonaActivity::Thinking);
    assert(presence.snapshot().voiceState == core::ResponseState::Speaking);

    // Easing needs time to have passed, or there is nothing to ease from.
    const float before = presence.intensity();
    presence.setNow(2000);
    const float after = presence.advance();
    assert(after > before);
    const float sameInstant = presence.advance();
    assert(sameInstant == after);
    presence.setNow(1000000);
    const float jumped = presence.advance();
    assert(jumped >= 0.0F && jumped <= 1.0F);

    // A clock that goes backwards must not fling the animation backwards.
    presence.setNow(10);
    assert(presence.advance() >= 0.0F);

    presence.setGenerating(true);
    assert(presence.busy());
    assert(presence.snapshot().flags.busy);

    // A drifting dial must not stomp on the fact that a turn is running: busy
    // is a fact about the generation, not a feeling.
    core::PersonaState state;
    state.calmness = 0.9F;
    state.initiative = 0.2F;
    presence.applyPersona(state);
    assert(presence.snapshot().flags.busy);
    assert(presence.snapshot().flags.calm);
    assert(!presence.snapshot().flags.proactive);
    assert(presence.snapshot().mood == core::PersonaMood::Calm);
}

// --- Voice pacing ------------------------------------------------------------

void testSpeechPlanningClausesAndPauses() {
    const core::VoicePersona& persona = core::defaultVoicePersona();
    const std::string_view text = "Understood. I checked the cache and it is fine. All good!";
    const std::vector<core::SpeechSegment> planned = core::planSpeech(text, persona, 220);

    assert(planned.size() == 3);
    assert(planned[0].isFirst);
    assert(planned[0].text == "Understood.");
    // The opening pause belongs to the first segment; a full stop earns the
    // longer breath afterwards.
    assert(planned[0].leadingPauseMs == 220);
    assert(planned[1].leadingPauseMs == persona.sentencePauseMs);
    assert(planned[2].leadingPauseMs == persona.sentencePauseMs);
    // Segments never carry the whitespace between them, so the gap between one
    // clause's end and the next clause's start has to be nothing but space.
    for (std::size_t i = 1; i < planned.size(); ++i) {
        assert(planned[i].startOffset >= planned[i - 1].endOffset);
        const std::string_view gap = text.substr(planned[i - 1].endOffset,
                                                 planned[i].startOffset - planned[i - 1].endOffset);
        assert(gap.find_first_not_of(" \t\n\r") == std::string_view::npos);
        assert(planned[i].text == text.substr(planned[i].startOffset,
                                              planned[i].endOffset - planned[i].startOffset));
    }

    // A comma is a clause, not a sentence, and gets the shorter gap -- but only
    // once the clause is long enough to be one.
    const auto soft = core::planSpeech("This is a long first clause, and a second one follows.",
                                       persona);
    assert(soft.size() == 2);
    assert(soft[0].text == "This is a long first clause,");
    assert(soft[1].text == "and a second one follows.");
    assert(soft[1].leadingPauseMs == persona.clausePauseMs);

    // "Well, no" must not become two segments: a clause shorter than the
    // minimum stays attached to what follows it.
    const auto shortClause = core::planSpeech("Well, not at all today.", persona);
    assert(shortClause.size() == 1);
    assert(shortClause.front().text == "Well, not at all today.");

    // Closing quotes stay with the sentence, including all bytes of U+201D.
    for (const std::string_view closing : {"\"", "'", ")", "]", "}", "\xe2\x80\x9d", "\xe2\x80\x9d)"}) {
        const std::string sentence = std::string("He said go.") + std::string(closing);
        const auto quoted = core::planSpeech(sentence + " Then he left.", persona);
        assert(quoted.size() == 2);
        assert(quoted[0].text == sentence);
        assert(quoted[0].endOffset == sentence.size());
        assert(quoted[1].text == "Then he left.");
        assert(quoted[1].startOffset == sentence.size() + 1);
        assert(quoted[1].leadingPauseMs == persona.sentencePauseMs);
    }

    assert(core::planSpeech("", persona).empty());
    assert(core::planSpeech("   ", persona).empty());
}

void testClauseStartBefore() {
    const std::string_view sentence = "Hi. There.";
    assert(core::clauseStartBefore(sentence, 0) == 0);
    assert(core::clauseStartBefore(sentence, 5) == 4);

    const std::string_view soft = "This is a long first clause, and a second one follows.";
    assert(core::clauseStartBefore(soft, 40) == 29);
    // Before the comma the whole thing is still one clause.
    assert(core::clauseStartBefore(soft, 10) == 0);
    assert(core::clauseStartBefore("", 5) == 0);
    assert(core::clauseStartBefore("no terminator", 99) == 0);
}

// The reason speech can start before generation finishes: the first clause is
// playable the moment those tokens exist, and the offsets it reports are
// absolute so the spoken cursor can be advanced by exactly what was spoken.
void testNextSpeechSegmentTracksTheSpokenCursor() {
    core::VoiceSession session;
    const core::ResponseId id = session.queueResponse();
    const core::GenerationId generation = session.beginGeneration(id);
    assert(session.appendText(id, generation, "Hi. There we go."));

    const auto first = session.nextSpeechSegment(id, 220);
    assert(first.has_value());
    assert(first->text == "Hi.");
    assert(first->isFirst);
    assert(first->leadingPauseMs == 220);
    assert(first->startOffset == 0);
    assert(first->endOffset == 3);

    assert(session.advancePlayback(id, first->endOffset));
    const auto second = session.nextSpeechSegment(id);
    assert(second.has_value());
    assert(!second->isFirst);
    assert(second->text == "There we go.");
    // The cursor sat on the space, so the next clause begins past it, and its
    // end lands exactly at the end of the generated text.
    assert(second->startOffset == 4);
    assert(second->endOffset == 16);
    assert(second->leadingPauseMs == session.voicePersona().sentencePauseMs);

    assert(session.advancePlayback(id, second->endOffset));
    assert(!session.nextSpeechSegment(id).has_value());
    assert(!session.nextSpeechSegment(9999).has_value());
}

void testVoicePersonaIsReplaceable() {
    core::VoiceSession session;
    assert(session.voicePersona().voiceId == core::defaultVoicePersona().voiceId);

    core::VoicePersona brisk;
    brisk.voiceId = "test-brisk";
    brisk.rate = 1.2F;
    brisk.clausePauseMs = 40;
    brisk.sentencePauseMs = 90;
    session.setVoicePersona(brisk);

    const core::ResponseId id = session.queueResponse();
    const core::GenerationId generation = session.beginGeneration(id);
    assert(session.appendText(id, generation, "One. Two."));
    const std::size_t eventsBefore = session.events().size();
    const auto segment = session.nextSpeechSegment(id);
    assert(segment.has_value());
    assert(segment->leadingPauseMs == brisk.sentencePauseMs);

    // A stricter voice is a pacing change only: asking what to say next is a
    // question, not a transition, so the timeline and its event log are
    // untouched until something is actually played.
    assert(session.find(id)->state() == core::ResponseState::Generating);
    assert(session.events().size() == eventsBefore);
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
    testPersonaPresenceLineIsStable();
    testPersonaDialsStayInRange();
    testPersonaAcknowledgementRotates();
    testPersonaAnticipation();
    testPersonaStatusWhispers();
    testPersonaSessionTopic();
    testIdleStaysSilentWhileTheUserIsPresent();
    testIdleDefaultPolicyIsLocalOnly();
    testIdleOnlyProducesPermittedWork();
    testIdleDriftsDialsButNeverRamps();
    testIdleGreetsOncePerAbsence();
    testIdleStopsWhenDisabled();
    testUndeclaredToolIsRefused();
    testDeclaredToolIsRefusedUntilEnabled();
    testMissingPermissionIsNamed();
    testNothingIsGrantedByDefault();
    testReplacedToolMustBeAgreedAgain();
    testPresenceTracksMeaning();
    testSpeechPlanningClausesAndPauses();
    testClauseStartBefore();
    testNextSpeechSegmentTracksTheSpokenCursor();
    testVoicePersonaIsReplaceable();
    return 0;
}
