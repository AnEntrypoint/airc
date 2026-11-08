#include "common/text.hpp"

#include <cstddef>

namespace ccb {

namespace {

bool is_ascii_alnum(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool is_hex(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

bool is_shell_safe(unsigned char c) {
    if (is_ascii_alnum(c)) {
        return true;
    }
    switch (c) {
    case '_': case '@': case '%': case '+': case '=': case ':': case ',': case '.': case '/': case '-':
        return true;
    default:
        return false;
    }
}

bool is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

bool is_continuation(unsigned char c) {
    return (c & 0xC0) == 0x80;
}

std::size_t utf8_sequence_length(std::string_view text, std::size_t at) {
    auto lead = static_cast<unsigned char>(text[at]);
    std::size_t length = 0;
    unsigned min_code = 0;
    unsigned code = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
        min_code = 0x80;
        code = lead & 0x1F;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        min_code = 0x800;
        code = lead & 0x0F;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        min_code = 0x10000;
        code = lead & 0x07;
    } else {
        return 1;
    }
    if (at + length > text.size()) {
        return 1;
    }
    for (std::size_t i = 1; i < length; ++i) {
        auto next = static_cast<unsigned char>(text[at + i]);
        if (!is_continuation(next)) {
            return 1;
        }
        code = (code << 6) | (next & 0x3F);
    }
    if (code < min_code || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
        return 1;
    }
    return length;
}

}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

bool contains(std::string_view text, std::string_view needle) {
    return text.find(needle) != std::string_view::npos;
}

std::string_view trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && is_space(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    while (end > begin && is_space(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    std::size_t at = 0;
    while (at < text.size()) {
        char c = text[at];
        if (c == '\n' || c == '\r') {
            lines.emplace_back(text.substr(start, at - start));
            if (c == '\r' && at + 1 < text.size() && text[at + 1] == '\n') {
                ++at;
            }
            ++at;
            start = at;
            continue;
        }
        ++at;
    }
    if (start < text.size()) {
        lines.emplace_back(text.substr(start));
    }
    return lines;
}

std::string replace_all(std::string text, std::string_view from, std::string_view to) {
    if (from.empty()) {
        return text;
    }
    std::size_t at = 0;
    while ((at = text.find(from, at)) != std::string::npos) {
        text.replace(at, from.size(), to);
        at += to.size();
    }
    return text;
}

std::string shell_quote(std::string_view text) {
    if (text.empty()) {
        return "''";
    }
    bool safe = true;
    for (unsigned char c : text) {
        if (!is_shell_safe(c)) {
            safe = false;
            break;
        }
    }
    if (safe) {
        return std::string(text);
    }
    std::string quoted = "'";
    for (char c : text) {
        if (c == '\'') {
            quoted += "'\"'\"'";
        } else {
            quoted += c;
        }
    }
    quoted += '\'';
    return quoted;
}

std::string slug_path(std::string_view path) {
    std::string slug;
    slug.reserve(path.size());
    std::size_t at = 0;
    while (at < path.size()) {
        auto c = static_cast<unsigned char>(path[at]);
        if (is_ascii_alnum(c)) {
            slug += static_cast<char>(c);
            ++at;
            continue;
        }
        slug += '-';
        at += c < 0x80 ? 1 : utf8_sequence_length(path, at);
    }
    return slug;
}

bool is_uuid(std::string_view text) {
    if (text.size() != 36) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        auto c = static_cast<unsigned char>(text[i]);
        bool dash_slot = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash_slot ? c != '-' : !is_hex(c)) {
            return false;
        }
    }
    return true;
}

bool is_session_name(std::string_view text) {
    if (text.empty() || text.size() > 128 || !is_ascii_alnum(static_cast<unsigned char>(text[0]))) {
        return false;
    }
    for (std::size_t i = 1; i < text.size(); ++i) {
        auto c = static_cast<unsigned char>(text[i]);
        if (!(is_ascii_alnum(c) || c == ' ' || c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

}
