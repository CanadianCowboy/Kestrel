// Pins what a `secret-tool search --unlock` report means, so a regression fails
// a build rather than a user's keyring.
//
// The store asks this question on every profile load and on every "forget
// everything", and it cannot ask it without libsecret, a D-Bus session and an
// unlocked keyring -- none of which a build agent has. The classification is
// therefore separated from the command that produces it (see
// src/storage/secretsearch.h) and tested here as text in and text out, which
// means it runs identically on all three CI platforms.
//
// Each fixture below is the shape GNOME/libsecret's tool/secret-tool.c
// produces, not a shape invented to be convenient. Where a fixture is a case
// libsecret does not itself produce, it says so, because the whole argument
// rests on absence never being inferred from silence.

#include "storage/secretsearch.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

using kestrel::storage::SecretSearchOutcome;
using kestrel::storage::classifySecretSearch;

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) {
        return;
    }
    std::cout << "  FAIL " << what << "\n";
    ++g_failures;
}

// A report for an item `search` read the secret out of. Every line here is one
// on_retrieve_secret actually prints.
const char* kReadableReport =
    "[2]\n"
    "label = Kestrel profile\n"
    "secret = 6e6f742068657265\n"
    "created = 2026-09-26 12:00:00\n"
    "modified = 2026-09-26 12:00:05\n"
    "schema = org.freedesktop.Secret.Generic\n"
    "attribute.account = kestrel\n"
    "attribute.service = com.kestrel.profile\n";

// The same item, found and reported in full but with no "secret = " line,
// because secret_retrievable_retrieve_secret failed. This is the locked
// keyring, and it is why `lookup` cannot be used: `lookup` would have printed
// one GError line and exited 1, which is exactly the shape a miss does not have
// but the pre-fix code could not prove.
const char* kUnreadableReport =
    "[2]\n"
    "label = Kestrel profile\n"
    "created = 2026-09-26 12:00:00\n"
    "modified = 2026-09-26 12:00:05\n"
    "schema = org.freedesktop.Secret.Generic\n"
    "attribute.account = kestrel\n"
    "attribute.service = com.kestrel.profile\n"
    "secret-tool: Prompt was dismissed\n";

// searchv_sync itself failed, so the status is non-zero and the GError is the
// only thing printed.
const char* kFailedReport =
    "secret-tool: The name org.freedesktop.secrets was not provided by any "
    ".service files\n";

void testAMissIsAbsentAndSilent() {
    // searchv_sync returned no items and no error: nothing printed, status 0.
    // This is the only case in which an empty error is correct, and the
    // distinction from the next test is the whole subject of this file.
    const auto report = classifySecretSearch(0, "");
    check(report.outcome == SecretSearchOutcome::absent,
          "an empty report with a zero status is absent");
    check(report.reason.empty(), "and carries no reason to show anyone");
}

void testAReadableProfileComesBackIntact() {
    const auto report = classifySecretSearch(0, kReadableReport);
    check(report.outcome == SecretSearchOutcome::readable, "a report with a value is readable");
    check(report.secret == "6e6f742068657265", "and the value is the one after the prefix");
    // The prefix is followed by one space, and getting that wrong would
    // silently hex-decode a shifted string rather than fail.
    check(report.secret == "6e6f742068657265" && report.secret.size() == 16,
          "the value has no leading space and no trailing newline");
}

void testTheValueIsFoundWhereverItSits() {
    // The secret line is not first, not last, and not the only line, and a
    // report may carry more than one item. The value belongs to the item asked
    // about, which is the first one `search` prints without --all.
    const std::string report =
        std::string("[1]\nlabel = something else\nsecret = deadbeef\n") + kReadableReport;
    const auto classified = classifySecretSearch(0, report);
    check(classified.outcome == SecretSearchOutcome::readable, "a value anywhere in the report is found");
    check(classified.secret == "deadbeef", "and the first one is the one that counts");
}

void testALockedKeyringIsUnreadableRatherThanAbsent() {
    // The case the store lost profiles over. Note the status: 0, because
    // on_retrieve_secret's failure never reaches secret_tool_action_search's
    // return value. Reading that status as "nothing stored" is the bug.
    const auto report = classifySecretSearch(0, kUnreadableReport);
    check(report.outcome == SecretSearchOutcome::unreadable,
          "an item reported without a value is unreadable");
    check(!report.reason.empty(), "and says what the keyring said about it");
    check(report.secret.empty(), "and there is no value to mistake for one");
}

void testAFailureIsNeverAMiss() {
    const auto report = classifySecretSearch(1, kFailedReport);
    check(report.outcome == SecretSearchOutcome::failed, "a non-zero status is a failure");
    check(!report.reason.empty(), "and carries what was printed");
    check(report.secret.empty(), "and no value");
}

void testAFailureThatSaidNothingIsStillAFailure() {
    // Not a case libsecret produces -- nothing in secret-tool.c exits non-zero
    // in silence. It is here because it is the one that must never be mistaken
    // for a miss anyway: absence is only ever concluded from a positive report,
    // so a quiet failure cannot become an empty profile with a save behind it.
    const auto report = classifySecretSearch(1, "");
    check(report.outcome == SecretSearchOutcome::failed, "a silent non-zero is still a failure");
    check(!report.reason.empty(), "and still says something, because silence is not a diagnosis");
}

void testANonZeroStatusWinsOverAnyReport() {
    // A tool that printed an item and then failed has not established that the
    // item is readable, and it has certainly not established that it is absent.
    const auto report = classifySecretSearch(1, kReadableReport);
    check(report.outcome == SecretSearchOutcome::failed,
          "a non-zero status is a failure even when a value was printed");
    check(report.secret.empty(), "and the value is not handed on from a failed run");
}

void testTrailingNewlinesAreNotContent() {
    // A shell pipeline adds a newline the tool did not print, and Windows adds
    // carriage returns with them. Neither may turn an absent report into an
    // unreadable one, which is the failure that would report a locked keyring
    // for a profile that was never there.
    for (const char* ending : {"\n", "\r\n", "\n\n", "\r"}) {
        const std::string report = std::string(ending);
        check(classifySecretSearch(0, report).outcome == SecretSearchOutcome::absent,
              std::string("a report of only whitespace is absent: ending ") + ending);
    }
    // And a real report keeps its value with the same endings.
    const std::string crlf = std::string(kReadableReport) + "\r\n\r\n";
    check(classifySecretSearch(0, crlf).secret == "6e6f742068657265",
          "a value survives trailing carriage returns");
}

void testAnEmptyStoredValueIsStillAValue() {
    // Someone sealed an empty blob. The report carries the prefix and nothing
    // after it, which is a readable report with an empty secret -- not an
    // absent one, and not a decode failure.
    const auto report = classifySecretSearch(0, "[2]\nlabel = x\nsecret = \n");
    check(report.outcome == SecretSearchOutcome::readable, "an empty value is still a value");
    check(report.secret.empty(), "and it is empty");
}

void testALineThatMerelyContainsThePrefixIsNotAValue() {
    // The prefix is matched at the start of a line, not anywhere in it, so a
    // label or an attribute that happens to read "secret = ..." cannot be
    // decoded as the secret.
    const auto report = classifySecretSearch(0, "[2]\nlabel = secret = not a value\n");
    check(report.outcome == SecretSearchOutcome::unreadable,
          "the prefix only counts at the start of a line");
}

} // namespace

int main() {
    std::cout << "secret search tests\n";
    testAMissIsAbsentAndSilent();
    testAReadableProfileComesBackIntact();
    testTheValueIsFoundWhereverItSits();
    testALockedKeyringIsUnreadableRatherThanAbsent();
    testAFailureIsNeverAMiss();
    testAFailureThatSaidNothingIsStillAFailure();
    testANonZeroStatusWinsOverAnyReport();
    testTrailingNewlinesAreNotContent();
    testAnEmptyStoredValueIsStillAValue();
    testALineThatMerelyContainsThePrefixIsNotAValue();

    if (g_failures == 0) {
        std::cout << "secret search tests passed\n";
        return 0;
    }
    std::cout << g_failures << " secret search check(s) failed\n";
    return 1;
}
