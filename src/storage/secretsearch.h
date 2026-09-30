#pragma once

#include <string>
#include <string_view>

namespace kestrel::storage {

// What a `secret-tool search --unlock` run says about one item.
//
// This exists because `lookup` cannot answer the question the store actually
// has. `secret-tool lookup` exits 1 for a miss and exits 1 for a failure,
// printing a reason only in the second case, so a caller has to infer "nothing
// is stored" from silence -- and a failure that happens to be silent is then
// read as an empty profile, which the next save writes over.
//
// `search` separates the two at the source, and the four cases below are the
// four things it can say. None of them is inferred from the absence of output;
// each is a positive statement, so a tool that fails quietly is still a
// failure rather than a miss.
//
// The behaviours are read out of GNOME/libsecret's tool/secret-tool.c:
//
//   absent     searchv_sync returns no items and no error. Nothing is printed
//              and the status is 0. libsecret's positive answer to "no such
//              profile", which is a first run and not a problem.
//   readable   the item is found and secret_retrievable_retrieve_secret
//              succeeds, so a line beginning "secret = " carries the value.
//   unreadable the item is found and reported in full -- path, label, dates,
//              attributes -- but the retrieval failed, so there is no
//              "secret = " line. That is a locked keyring, and on_retrieve_secret
//              prints the reason. The process still exits 0: the callback's
//              failure never reaches the return value, which is exactly why
//              the status alone could not have told this apart from a miss.
//   failed     searchv_sync itself failed -- no secret service, no session bus.
//              The GError is printed and the status is 1. Whether anything is
//              stored is unknown, and unknown is not the same as absent.
enum class SecretSearchOutcome {
    absent,
    readable,
    unreadable,
    failed,
};

struct SecretSearchReport {
    SecretSearchOutcome outcome = SecretSearchOutcome::failed;

    // The hex-encoded secret. Set only when the outcome is readable.
    std::string secret;

    // What the tool printed, which is the reason for an unreadable item and for
    // a failure. Left empty when there was genuinely nothing to say, which is
    // the `absent` case and a silent failure; the second of those is why
    // `failed` still carries a reason the caller can show.
    std::string reason;
};

// Classifies one run. `status` is the command's exit status and `report`
// everything it printed, standard error included.
//
// No secret-tool, no keyring and no platform involved, which is what lets this
// be tested on the CI machines that have neither.
[[nodiscard]] SecretSearchReport classifySecretSearch(int status, std::string_view report);

} // namespace kestrel::storage
