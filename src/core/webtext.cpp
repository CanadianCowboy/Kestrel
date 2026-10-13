#include "core/webtext.h"

#include <array>
#include <cctype>
#include <cstdint>

namespace kestrel::core {
namespace {

char lowerAscii(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

// `word` must already be lowercase.
bool startsWithIgnoringCase(std::string_view text, std::size_t at, std::string_view word) {
    if (at > text.size() || text.size() - at < word.size()) {
        return false;
    }
    for (std::size_t i = 0; i < word.size(); ++i) {
        if (lowerAscii(text[at + i]) != word[i]) {
            return false;
        }
    }
    return true;
}

std::size_t findIgnoringCase(std::string_view text, std::size_t from, std::string_view word) {
    for (std::size_t i = from; i < text.size(); ++i) {
        if (startsWithIgnoringCase(text, i, word)) {
            return i;
        }
    }
    return std::string_view::npos;
}

bool isBlockTag(std::string_view name) {
    static constexpr std::array<std::string_view, 22> kBlocks = {
        "p", "div", "br", "li", "ul", "ol", "h1", "h2", "h3", "h4", "h5", "h6",
        "tr", "section", "article", "header", "footer", "table", "blockquote", "pre", "hr", "dd"};
    for (std::string_view block : kBlocks) {
        if (name == block) {
            return true;
        }
    }
    return false;
}

void appendUtf8(std::uint32_t code, std::string& out) {
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

// Decodes the body of an entity such as "amp" or "#x2014". Unknown entities are
// left as written, so a stray ampersand in prose survives.
bool decodeEntity(std::string_view name, std::string& out) {
    if (name == "amp") {
        out = "&";
    } else if (name == "lt") {
        out = "<";
    } else if (name == "gt") {
        out = ">";
    } else if (name == "quot") {
        out = "\"";
    } else if (name == "apos") {
        out = "'";
    } else if (name == "nbsp") {
        out = " ";
    } else if (!name.empty() && name[0] == '#') {
        const bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
        const std::string_view digits = name.substr(hex ? 2 : 1);
        if (digits.empty() || digits.size() > 8) {
            return false;
        }
        std::uint32_t code = 0;
        for (char digit : digits) {
            std::uint32_t value = 0;
            if (digit >= '0' && digit <= '9') {
                value = static_cast<std::uint32_t>(digit - '0');
            } else if (hex && digit >= 'a' && digit <= 'f') {
                value = static_cast<std::uint32_t>(digit - 'a' + 10);
            } else if (hex && digit >= 'A' && digit <= 'F') {
                value = static_cast<std::uint32_t>(digit - 'A' + 10);
            } else {
                return false;
            }
            code = code * (hex ? 16U : 10U) + value;
        }
        if (code == 0 || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
            return false;
        }
        out.clear();
        appendUtf8(code, out);
    } else {
        return false;
    }
    return true;
}

} // namespace

std::string htmlToText(std::string_view html, std::size_t maxChars) {
    // First pass: strip markup and decode entities, keeping raw whitespace so the
    // second pass can decide where a line really ends.
    std::string raw;
    raw.reserve(html.size());
    std::size_t i = 0;
    while (i < html.size()) {
        const char c = html[i];
        if (c == '<') {
            if (startsWithIgnoringCase(html, i, "<!--")) {
                const std::size_t end = html.find("-->", i + 4);
                i = end == std::string_view::npos ? html.size() : end + 3;
                continue;
            }
            if (i + 1 < html.size() && html[i + 1] == '!') {
                // A doctype or other declaration: nothing in it is page text.
                const std::size_t end = html.find('>', i + 1);
                i = end == std::string_view::npos ? html.size() : end + 1;
                continue;
            }
            const std::size_t close = html.find('>', i + 1);
            if (close == std::string_view::npos) {
                break; // an unterminated tag at the end is dropped, not shown as text
            }
            const std::string_view inside = html.substr(i + 1, close - i - 1);
            const bool closing = !inside.empty() && inside[0] == '/';
            const std::string_view body = closing ? inside.substr(1) : inside;
            std::size_t nameLength = 0;
            while (nameLength < body.size() && std::isalnum(static_cast<unsigned char>(body[nameLength]))) {
                ++nameLength;
            }
            if (nameLength == 0) {
                // A lone "<" in prose, such as "a < b": keep it.
                raw += c;
                ++i;
                continue;
            }
            std::string name;
            for (std::size_t k = 0; k < nameLength; ++k) {
                name += lowerAscii(body[k]);
            }
            if (!closing && (name == "script" || name == "style")) {
                // Everything up to the matching end tag is code or styling, not text.
                const std::size_t endTag = findIgnoringCase(html, close + 1, "</" + name);
                if (endTag == std::string_view::npos) {
                    break;
                }
                const std::size_t after = html.find('>', endTag);
                i = after == std::string_view::npos ? html.size() : after + 1;
                continue;
            }
            if (isBlockTag(name)) {
                raw += '\n';
            }
            i = close + 1;
            continue;
        }
        if (c == '&') {
            const std::size_t semi = html.find(';', i);
            if (semi != std::string_view::npos && semi - i <= 10) {
                std::string decoded;
                if (decodeEntity(html.substr(i + 1, semi - i - 1), decoded)) {
                    raw += decoded;
                    i = semi + 1;
                    continue;
                }
            }
        }
        raw += c;
        ++i;
    }

    // Second pass: collapse runs of spaces to one space, and runs of line breaks to
    // at most one blank line. Leading and trailing whitespace is dropped.
    std::string out;
    out.reserve(raw.size());
    std::size_t newlines = 0;
    bool space = false;
    for (char ch : raw) {
        if (ch == '\n') {
            ++newlines;
            space = false;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(ch))) {
            if (newlines == 0) {
                space = true;
            }
            continue;
        }
        if (newlines > 0) {
            if (!out.empty()) {
                out += newlines >= 2 ? "\n\n" : "\n";
            }
            newlines = 0;
        } else if (space && !out.empty()) {
            out += ' ';
        }
        space = false;
        out += ch;
    }

    if (out.size() <= maxChars) {
        return out;
    }
    // Cut on a character boundary: continuation bytes look like 10xxxxxx.
    std::size_t cut = maxChars;
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    out.resize(cut);
    out += "\n[text cut short]";
    return out;
}

} // namespace kestrel::core
