#include "core/astronomy.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace kestrel::core::astronomy {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;
constexpr double kRadToDeg = 180.0 / kPi;

// Julian Date at 1970-01-01T00:00:00 UTC, and the MJD offset. Both are exact
// by definition, so they are constants rather than magic numbers in a formula.
constexpr double kUnixEpochJulianDay = 2440587.5;
constexpr double kModifiedJulianOffset = 2400000.5;

// One Julian year of light travel, from an exact c and 365.25 days.
constexpr double kLightYearMeters = 299792458.0 * 31557600.0;
// The IAU 2012 nominal solar radius and the Earth's equatorial radius.
constexpr double kSolarRadiusMeters = 6.957e8;
constexpr double kEarthRadiusMeters = 6.3781e6;
// The parsec follows from one astronomical unit subtending one arcsecond.
constexpr double kAstronomicalUnitMeters = 1.495978707e11;
constexpr double kParsecMeters = 3.0856775814913673e16;

std::string_view trim(std::string_view text) noexcept {
    const auto isBlank = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    while (!text.empty() && isBlank(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && isBlank(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

char lowerAscii(char c) noexcept {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

// Unit names are ASCII, so a lowercase comparison needs no locale.
std::string lowered(std::string_view text) {
    std::string name(text);
    for (char& c : name) {
        c = lowerAscii(c);
    }
    return name;
}

bool parseNumber(std::string_view text, double& out) noexcept {
    if (text.empty()) {
        return false;
    }
    const std::string owned(text);
    char* end = nullptr;
    const double value = std::strtod(owned.c_str(), &end);
    if (end == owned.c_str() || end != owned.c_str() + owned.size() || !std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

bool parseDigits(std::string_view text, int& out) noexcept {
    if (text.empty() || text.size() > 6) {
        return false;
    }
    int value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    out = value;
    return true;
}

int daysInMonth(int year, int month) noexcept {
    static constexpr std::array<int, 12> kDays{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) {
        return 0;
    }
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
        return 29;
    }
    return kDays[static_cast<std::size_t>(month - 1)];
}

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's
// days_from_civil). Integer arithmetic keeps the epoch exact at midnight, which
// is the one place a floating-point shortcut would drift.
long long daysFromCivil(int year, unsigned month, unsigned day) noexcept {
    year -= month <= 2 ? 1 : 0;
    const long long era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
    const unsigned shiftedMonth = month > 2 ? month - 3U : month + 9U;
    const unsigned dayOfYear = (153U * shiftedMonth + 2U) / 5U + day - 1U;
    const unsigned dayOfEra = yearOfEra * 365U + yearOfEra / 4U - yearOfEra / 100U + dayOfYear;
    return era * 146097LL + static_cast<long long>(dayOfEra) - 719468LL;
}

// Splits "05h34m31.94s", "05:34:31.94", or "+22 00 52.2" into numeric parts.
// The separators are the ones astronomy uses for sexagesimal values; anything
// else is rejected rather than dropped, so a typo cannot become a number.
bool splitSexagesimal(std::string_view text, std::vector<double>& parts, std::string& error) {
    parts.clear();
    std::string current;
    const auto flush = [&]() -> bool {
        if (current.empty()) {
            return true;
        }
        double value = 0.0;
        if (!parseNumber(current, value)) {
            error = "One component is not a number.";
            return false;
        }
        parts.push_back(value);
        current.clear();
        return true;
    };
    for (const char c : text) {
        const bool separator = c == ':' || c == ' ' || c == '\t' || c == ',' || c == 'h' || c == 'H'
            || c == 'd' || c == 'D' || c == 'm' || c == 'M' || c == 's' || c == 'S';
        if (separator) {
            if ((c == ':' || c == ' ') && current.empty() && !parts.empty()) {
                error = "A sexagesimal value has an empty component.";
                return false;
            }
            if (!flush()) {
                return false;
            }
            continue;
        }
        const bool digit = (c >= '0' && c <= '9') || c == '.' || c == '+' || c == '-';
        if (!digit) {
            error = "Use digits with ':' or a single letter between the components.";
            return false;
        }
        current.push_back(c);
    }
    if (!flush()) {
        return false;
    }
    if (parts.empty() || parts.size() > 3) {
        error = "A sexagesimal value has one to three components.";
        return false;
    }
    return true;
}

double sexagesimalToDecimal(const std::vector<double>& parts, double scale) noexcept {
    double value = parts[0];
    if (parts.size() > 1) {
        value += parts[1] / 60.0;
    }
    if (parts.size() > 2) {
        value += parts[2] / 3600.0;
    }
    return value * scale;
}

bool validateSexagesimal(const std::vector<double>& parts, std::string& error) {
    for (std::size_t i = 1; i < parts.size(); ++i) {
        if (!(parts[i] >= 0.0 && parts[i] < 60.0)) {
            error = i == 1 ? "Minutes must be in [0, 60)." : "Seconds must be in [0, 60).";
            return false;
        }
    }
    return true;
}

} // namespace

bool parseIsoUtc(std::string_view text, double& julianDay, std::string& error) noexcept {
    error.clear();
    const std::string_view input = trim(text);
    if (input.size() < 10) {
        error = "Pass an ISO 8601 UTC date such as 2026-10-17 or 2026-10-17T21:04:00Z.";
        return false;
    }
    int year = 0;
    int month = 0;
    int day = 0;
    if (input[4] != '-' || input[7] != '-'
        || !parseDigits(input.substr(0, 4), year)
        || !parseDigits(input.substr(5, 2), month)
        || !parseDigits(input.substr(8, 2), day)) {
        error = "Pass an ISO 8601 UTC date such as 2026-10-17 or 2026-10-17T21:04:00Z.";
        return false;
    }
    if (month < 1 || month > 12 || day < 1 || day > daysInMonth(year, month)) {
        error = "The month and day must describe a real calendar date.";
        return false;
    }

    double fraction = 0.0;
    if (input.size() > 10) {
        if (input[10] != 'T' && input[10] != 't' && input[10] != ' ') {
            error = "Separate the date and time with T, for example 2026-10-17T21:04:00Z.";
            return false;
        }
        std::string_view clock = input.substr(11);
        if (!clock.empty() && (clock.back() == 'Z' || clock.back() == 'z')) {
            clock.remove_suffix(1);
        }
        if (clock.empty() || clock.size() < 5 || clock[2] != ':') {
            error = "Use a 24-hour time such as 21:04 or 21:04:00.";
            return false;
        }
        int hour = 0;
        int minute = 0;
        if (!parseDigits(clock.substr(0, 2), hour) || !parseDigits(clock.substr(3, 2), minute)) {
            error = "Use a 24-hour time such as 21:04 or 21:04:00.";
            return false;
        }
        double second = 0.0;
        if (clock.size() > 5) {
            if (clock[5] != ':' || !parseNumber(clock.substr(6), second)) {
                error = "Use a 24-hour time such as 21:04 or 21:04:00.";
                return false;
            }
        }
        if (hour > 23 || minute > 59 || !(second >= 0.0 && second < 61.0)) {
            error = "The time of day is out of range; use 00:00 to 23:59.";
            return false;
        }
        fraction = (static_cast<double>(hour) + static_cast<double>(minute) / 60.0 + second / 3600.0) / 24.0;
    }

    julianDay = kUnixEpochJulianDay + static_cast<double>(daysFromCivil(year, static_cast<unsigned>(month),
                                                                       static_cast<unsigned>(day)))
        + fraction;
    return true;
}

bool parseRightAscension(std::string_view text, double& hours, std::string& error) noexcept {
    error.clear();
    const std::string_view input = trim(text);
    if (input.empty()) {
        error = "Pass a right ascension such as 05:34:31.94 or 83.63.";
        return false;
    }
    if (input.front() == '-' || input.front() == '+') {
        error = "Right ascension has no sign; it runs eastward from 0 to 24 hours.";
        return false;
    }
    std::vector<double> parts;
    if (!splitSexagesimal(input, parts, error)) {
        return false;
    }
    if (!validateSexagesimal(parts, error)) {
        return false;
    }
    const double value = sexagesimalToDecimal(parts, 1.0);
    if (!(value >= 0.0 && value < 24.0)) {
        error = "Right ascension must be in [0, 24) hours; use 0 to 24, not degrees.";
        return false;
    }
    hours = value;
    return true;
}

bool parseDeclination(std::string_view text, double& degrees, std::string& error) noexcept {
    error.clear();
    std::string_view input = trim(text);
    if (input.empty()) {
        error = "Pass a declination such as +22:00:52.2 or -5.39.";
        return false;
    }
    bool negative = false;
    if (input.front() == '+' || input.front() == '-') {
        negative = input.front() == '-';
        input.remove_prefix(1);
    }
    std::vector<double> parts;
    if (!splitSexagesimal(input, parts, error)) {
        return false;
    }
    if (!validateSexagesimal(parts, error)) {
        return false;
    }
    const double value = sexagesimalToDecimal(parts, 1.0);
    if (value > 90.0) {
        error = "Declination must be in [-90, 90] degrees.";
        return false;
    }
    degrees = negative ? -value : value;
    return true;
}

std::string formatRightAscension(double hours) {
    constexpr double kCentisecondsPerDay = 24.0 * 3600.0 * 100.0;
    double total = hours * 3600.0 * 100.0;
    total = std::fmod(std::round(total), kCentisecondsPerDay);
    if (total < 0.0) {
        total += kCentisecondsPerDay;
    }
    const long long centiseconds = static_cast<long long>(total);
    const long long hour = centiseconds / 360000;
    const long long minute = (centiseconds / 6000) % 60;
    const double second = static_cast<double>(centiseconds % 6000) / 100.0;
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%02lldh%02lldm%05.2fs", hour, minute, second);
    return buffer;
}

std::string formatDeclination(double degrees) {
    const char sign = degrees < 0.0 ? '-' : '+';
    const double magnitude = std::round(std::abs(degrees) * 3600.0 * 100.0);
    const long long centiseconds = static_cast<long long>(magnitude);
    const long long degree = centiseconds / 360000;
    const long long minute = (centiseconds / 6000) % 60;
    const double second = static_cast<double>(centiseconds % 6000) / 100.0;
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%c%02lldd%02lldm%05.2fs", sign, degree, minute, second);
    return buffer;
}

double angularSeparationDeg(double raHoursA, double decDegreesA,
                            double raHoursB, double decDegreesB) noexcept {
    const double decA = decDegreesA * kDegToRad;
    const double decB = decDegreesB * kDegToRad;
    const double deltaRightAscension = (raHoursB - raHoursA) * 15.0 * kDegToRad;
    const double sinHalfDec = std::sin((decB - decA) / 2.0);
    const double sinHalfRa = std::sin(deltaRightAscension / 2.0);
    const double haversine = sinHalfDec * sinHalfDec
        + std::cos(decA) * std::cos(decB) * sinHalfRa * sinHalfRa;
    const double clamped = std::clamp(haversine, 0.0, 1.0);
    return 2.0 * std::asin(std::sqrt(clamped)) * kRadToDeg;
}

bool convertLength(double value, std::string_view from, std::string_view to,
                   double& result, std::string& error) noexcept {
    error.clear();
    if (!std::isfinite(value)) {
        error = "The value must be a finite number.";
        return false;
    }
    struct Unit {
        std::string_view name;
        double meters;
    };
    static constexpr std::array<Unit, 11> kUnits{{
        {"m", 1.0},
        {"km", 1.0e3},
        {"au", kAstronomicalUnitMeters},
        {"ly", kLightYearMeters},
        {"light-year", kLightYearMeters},
        {"lightyear", kLightYearMeters},
        {"pc", kParsecMeters},
        {"parsec", kParsecMeters},
        {"kpc", kParsecMeters * 1.0e3},
        {"mpc", kParsecMeters * 1.0e6},
        {"rsun", kSolarRadiusMeters},
    }};
    const auto factor = [](std::string_view name) -> double {
        const std::string key = lowered(trim(name));
        if (key == "rearth" || key == "earth-radius") {
            return kEarthRadiusMeters;
        }
        if (key == "solar-radius") {
            return kSolarRadiusMeters;
        }
        for (const Unit& unit : kUnits) {
            if (unit.name == key) {
                return unit.meters;
            }
        }
        return 0.0;
    };
    const double fromFactor = factor(from);
    const double toFactor = factor(to);
    if (fromFactor <= 0.0 || toFactor <= 0.0) {
        error = "Use one of m, km, au, ly, pc, kpc, mpc, rsun, rearth, or solar-radius.";
        return false;
    }
    result = value * fromFactor / toFactor;
    return std::isfinite(result);
}

double fluxRatioFromMagnitudeDifference(double deltaMagnitude) noexcept {
    return std::pow(10.0, -0.4 * deltaMagnitude);
}

double distanceParsecsFromDistanceModulus(double distanceModulus) noexcept {
    return std::pow(10.0, (distanceModulus + 5.0) / 5.0);
}

} // namespace kestrel::core::astronomy
