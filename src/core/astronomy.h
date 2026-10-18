#pragma once

#include <string>
#include <string_view>

namespace kestrel::core::astronomy {

// Small, dependency-free astronomy arithmetic behind the tools Kestrel offers.
// Everything here is local and deterministic: no catalogue lookup, no network,
// no clock. A caller supplies the date, angle, or quantity, and gets a number
// back together with the assumption it was computed under.
//
// These functions compute; they do not know objects. A tool that turns
// coordinates into a name would be a catalogue, and that is a different,
// permissioned thing.

// The Julian Day of an ISO 8601 UTC instant. Accepts "YYYY-MM-DD" or
// "YYYY-MM-DDTHH:MM[:SS[.fff]]", with an optional trailing Z. A malformed date
// is rejected rather than guessed at, because a wrong epoch silently changes
// every number derived from it.
//
// The result is on the UTC scale. TT differs from UTC by about 69 s in 2026,
// and a barycentric epoch (BJD_TDB) additionally needs the light-travel time to
// the solar-system barycenter, so callers should state the scale they used.
[[nodiscard]] bool parseIsoUtc(std::string_view text, double& julianDay, std::string& error) noexcept;

// "HH:MM:SS.s", "HHhMMmSS.ss", or decimal hours. Hours must be in [0, 24).
[[nodiscard]] bool parseRightAscension(std::string_view text, double& hours, std::string& error) noexcept;

// "+DD:MM:SS.s", "-DD MM SS.s", "DDdMMmSS.ss", or signed decimal degrees.
// Degrees must be in [-90, 90]; an unsigned value is taken as north.
[[nodiscard]] bool parseDeclination(std::string_view text, double& degrees, std::string& error) noexcept;

[[nodiscard]] std::string formatRightAscension(double hours);
[[nodiscard]] std::string formatDeclination(double degrees);

// Great-circle separation in degrees. Haversine rather than the law of cosines,
// so two positions an arcsecond apart do not come back as rounding noise.
[[nodiscard]] double angularSeparationDeg(double raHoursA, double decDegreesA,
                                          double raHoursB, double decDegreesB) noexcept;

// Length conversion between the units an astronomy answer is usually phrased
// in. False and a filled `error` for an unknown unit, because a silent unit
// mismatch is a wrong answer that looks right.
[[nodiscard]] bool convertLength(double value, std::string_view from, std::string_view to,
                                 double& result, std::string& error) noexcept;

// Flux ratio F1/F2 for a magnitude difference m1 - m2. Five magnitudes is a
// factor of 100; the sign follows the magnitude scale, so a fainter first
// source gives a ratio below one.
[[nodiscard]] double fluxRatioFromMagnitudeDifference(double deltaMagnitude) noexcept;

// Distance in parsecs from the distance modulus m - M, ignoring absorption.
// Whether extinction matters is the caller's judgement to state.
[[nodiscard]] double distanceParsecsFromDistanceModulus(double distanceModulus) noexcept;

} // namespace kestrel::core::astronomy
