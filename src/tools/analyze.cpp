#include "tools/tools.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace ccb {

namespace {

using Codepoints = std::u32string;
using CodepointView = std::u32string_view;

constexpr char32_t escape = 0x1b;
constexpr char32_t bell = 0x07;
constexpr char32_t replacement = 0xFFFD;
constexpr int rule_glyph_minimum = 3;
constexpr std::size_t header_width = 30;

Codepoints decode_utf8(std::string_view bytes) {
    Codepoints out;
    out.reserve(bytes.size());
    const std::size_t n = bytes.size();
    std::size_t i = 0;
    while (i < n) {
        const auto lead = static_cast<unsigned char>(bytes[i]);
        if (lead < 0x80) {
            out.push_back(lead);
            ++i;
            continue;
        }
        int need = 0;
        char32_t cp = 0;
        if (lead >= 0xC2 && lead <= 0xDF) {
            need = 1;
            cp = lead & 0x1F;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            need = 2;
            cp = lead & 0x0F;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            need = 3;
            cp = lead & 0x07;
        } else {
            out.push_back(replacement);
            ++i;
            continue;
        }
        unsigned char low = 0x80;
        unsigned char high = 0xBF;
        if (lead == 0xE0) low = 0xA0;
        if (lead == 0xED) high = 0x9F;
        if (lead == 0xF0) low = 0x90;
        if (lead == 0xF4) high = 0x8F;
        std::size_t j = i + 1;
        int got = 0;
        bool complete = true;
        while (got < need) {
            if (j >= n) {
                complete = false;
                break;
            }
            const auto next = static_cast<unsigned char>(bytes[j]);
            const unsigned char lo = got == 0 ? low : 0x80;
            const unsigned char hi = got == 0 ? high : 0xBF;
            if (next < lo || next > hi) {
                complete = false;
                break;
            }
            cp = (cp << 6) | (next & 0x3F);
            ++j;
            ++got;
        }
        out.push_back(complete ? cp : replacement);
        i = j;
    }
    return out;
}

void append_utf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string encode_utf8(CodepointView text) {
    std::string out;
    out.reserve(text.size());
    for (const char32_t cp : text) append_utf8(out, cp);
    return out;
}

std::size_t codepoint_count(std::string_view utf8) {
    return static_cast<std::size_t>(std::count_if(utf8.begin(), utf8.end(), [](char c) {
        return (static_cast<unsigned char>(c) & 0xC0) != 0x80;
    }));
}

bool in_range(char32_t cp, char32_t low, char32_t high) { return cp >= low && cp <= high; }

Codepoints remove_osc(const Codepoints& text) {
    Codepoints out;
    out.reserve(text.size());
    const std::size_t n = text.size();
    std::size_t i = 0;
    while (i < n) {
        if (text[i] == escape && i + 1 < n && text[i + 1] == U']') {
            std::size_t j = i + 2;
            while (j < n && text[j] != bell && text[j] != escape) ++j;
            if (j < n && text[j] == bell) {
                i = j + 1;
                continue;
            }
            if (j + 1 < n && text[j] == escape && text[j + 1] == U'\\') {
                i = j + 2;
                continue;
            }
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

Codepoints remove_device_strings(const Codepoints& text) {
    Codepoints out;
    out.reserve(text.size());
    const std::size_t n = text.size();
    std::size_t i = 0;
    while (i < n) {
        if (text[i] == escape && i + 1 < n &&
            (text[i + 1] == U'P' || text[i + 1] == U'_' || text[i + 1] == U'^')) {
            std::size_t j = i + 2;
            while (j < n && text[j] != escape) ++j;
            if (j + 1 < n && text[j] == escape && text[j + 1] == U'\\') j += 2;
            i = j;
            continue;
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

Codepoints remove_csi(const Codepoints& text) {
    Codepoints out;
    out.reserve(text.size());
    const std::size_t n = text.size();
    std::size_t i = 0;
    while (i < n) {
        if (text[i] == escape && i + 1 < n && text[i + 1] == U'[') {
            std::size_t j = i + 2;
            while (j < n && in_range(text[j], 0x30, 0x3F)) ++j;
            while (j < n && in_range(text[j], 0x20, 0x2F)) ++j;
            if (j < n && in_range(text[j], 0x40, 0x7E)) {
                i = j + 1;
                continue;
            }
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

Codepoints remove_single_escapes(const Codepoints& text) {
    Codepoints out;
    out.reserve(text.size());
    const std::size_t n = text.size();
    std::size_t i = 0;
    while (i < n) {
        if (text[i] == escape && i + 1 < n &&
            (in_range(text[i + 1], 0x40, 0x5A) || in_range(text[i + 1], 0x5C, 0x5F))) {
            i += 2;
            continue;
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

bool is_control_to_drop(char32_t cp) {
    return in_range(cp, 0x00, 0x08) || cp == 0x0B || cp == 0x0C || in_range(cp, 0x0E, 0x1F) || cp == 0x7F;
}

bool is_space(char32_t cp) {
    return in_range(cp, 0x09, 0x0D) || in_range(cp, 0x1C, 0x20) || cp == 0x85 || cp == 0xA0 || cp == 0x1680 ||
           in_range(cp, 0x2000, 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F ||
           cp == 0x3000;
}

bool is_word(char32_t cp) {
    return in_range(cp, U'0', U'9') || in_range(cp, U'a', U'z') || in_range(cp, U'A', U'Z') || cp == U'_' ||
           cp > 0x7F;
}

CodepointView ltrim(CodepointView line) {
    std::size_t i = 0;
    while (i < line.size() && is_space(line[i])) ++i;
    return line.substr(i);
}

CodepointView rtrim(CodepointView line) {
    std::size_t n = line.size();
    while (n > 0 && is_space(line[n - 1])) --n;
    return line.substr(0, n);
}

CodepointView trim_both(CodepointView line) { return rtrim(ltrim(line)); }

bool only_from(CodepointView text, CodepointView glyphs) {
    return text.find_first_not_of(glyphs) == CodepointView::npos;
}

bool is_horizontal_rule(CodepointView line) {
    static constexpr CodepointView glyphs = U"─═━╌┄┈-—_";
    const CodepointView body = trim_both(line);
    return body.size() >= static_cast<std::size_t>(rule_glyph_minimum) && only_from(body, glyphs);
}

bool is_bar_line(CodepointView line) {
    static constexpr CodepointView glyphs =
        U"▁▂▃▅▇█▉▊▋▌▍▎▏";
    const CodepointView body = trim_both(line);
    return !body.empty() && only_from(body, glyphs);
}

bool is_empty_prompt(CodepointView line) {
    const CodepointView body = trim_both(line);
    return body.empty() || body == U"❯";
}

bool is_mode_indicator(CodepointView line) {
    const CodepointView body = ltrim(line);
    return body.size() >= 3 && body[0] == 0x23F5 && body[1] == 0x23F5 && is_space(body[2]);
}

bool is_spinner_line(CodepointView line) {
    static constexpr CodepointView glyphs = U"✻✽✢✳✶·*";
    const CodepointView body = ltrim(line);
    return body.size() >= 2 && glyphs.find(body[0]) != CodepointView::npos && is_space(body[1]);
}

bool has_prefix(CodepointView text, CodepointView prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

bool is_banner(CodepointView line) {
    const CodepointView body = ltrim(line);
    return has_prefix(body, U"Claude Code v") || has_prefix(body, U"/workspace") || has_prefix(body, U"hf:") ||
           has_prefix(body, U"claude-");
}

bool is_token_counter(CodepointView line) {
    CodepointView body = ltrim(line);
    if (!has_prefix(body, U"token")) return false;
    body.remove_prefix(5);
    if (!body.empty() && body[0] == U's') body.remove_prefix(1);
    return body.empty() || !is_word(body[0]);
}

bool is_elapsed_time(CodepointView line) {
    CodepointView body = ltrim(line);
    std::size_t i = 0;
    const auto skip_digits = [&] {
        while (i < body.size() && in_range(body[i], U'0', U'9')) ++i;
    };
    const std::size_t digits_start = i;
    skip_digits();
    if (i == digits_start) return false;
    if (i < body.size() && body[i] == U'm') ++i;
    if (i < body.size() && body[i] == U' ') ++i;
    skip_digits();
    if (i < body.size() && body[i] == U's') ++i;
    return trim_both(body.substr(i)).empty();
}

bool looks_chrome(CodepointView line) {
    return is_horizontal_rule(line) || is_bar_line(line) || is_empty_prompt(line) || is_mode_indicator(line) ||
           is_spinner_line(line) || has_prefix(ltrim(line), U"esc to interrupt") ||
           has_prefix(ltrim(line), U"? for shortcuts") || is_banner(line) || is_token_counter(line) ||
           is_elapsed_time(line);
}

CodepointView last_terminal_rewrite(CodepointView raw_line) {
    const std::size_t cr = raw_line.rfind(U'\r');
    return cr == CodepointView::npos ? raw_line : raw_line.substr(cr + 1);
}

std::string read_binary(const std::string& path, bool& ok) {
    std::ifstream in(path, std::ios::binary);
    ok = static_cast<bool>(in);
    if (!ok) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool write_text(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out << content;
    return static_cast<bool>(out);
}

std::string last_lines(const std::string& text, int count) {
    if (count <= 0) return text;
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (true) {
        const std::size_t newline = text.find('\n', start);
        if (newline == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, newline - start));
        start = newline + 1;
    }
    const std::size_t keep = std::min(lines.size(), static_cast<std::size_t>(count));
    std::string out;
    for (std::size_t i = lines.size() - keep; i < lines.size(); ++i) {
        if (i != lines.size() - keep) out.push_back('\n');
        out += lines[i];
    }
    return out;
}

void show_view(const std::string& name, const std::string& text, int tail) {
    std::ostringstream out;
    out << '\n'
        << std::string(header_width, '=') << ' ' << name << " (" << codepoint_count(text) << " chars) "
        << std::string(header_width, '=') << '\n'
        << last_lines(text, tail) << '\n';
    const std::string rendered = out.str();
    std::fwrite(rendered.data(), 1, rendered.size(), stdout);
}

}

std::string strip_ansi(std::string_view raw) {
    Codepoints text = decode_utf8(raw);
    text = remove_osc(text);
    text = remove_device_strings(text);
    text = remove_csi(text);
    text = remove_single_escapes(text);
    text.erase(std::remove_if(text.begin(), text.end(), is_control_to_drop), text.end());
    return encode_utf8(text);
}

std::string to_text(std::string_view stripped) {
    const Codepoints text = decode_utf8(stripped);
    Codepoints out;
    bool previous_blank = false;
    bool first = true;
    std::size_t start = 0;
    while (true) {
        const std::size_t newline = text.find(U'\n', start);
        const std::size_t end = newline == Codepoints::npos ? text.size() : newline;
        const CodepointView line = rtrim(last_terminal_rewrite(CodepointView(text).substr(start, end - start)));
        start = end + 1;
        const bool last = newline == Codepoints::npos;
        if (!looks_chrome(line)) {
            const bool blank = trim_both(line).empty();
            if (!(blank && previous_blank)) {
                previous_blank = blank;
                if (!first) out.push_back(U'\n');
                out.append(line);
                first = false;
            }
        }
        if (last) break;
    }
    return encode_utf8(out);
}

int run_analyze(const AnalyzeOptions& options) {
    bool readable = false;
    const std::string raw = read_binary(options.path, readable);
    if (!readable) {
        std::fprintf(stderr, "cannot read %s\n", options.path.c_str());
        return 1;
    }
    const std::string stripped = strip_ansi(raw);
    const std::string text = to_text(stripped);

    if (!options.out.empty()) {
        if (!write_text(options.out + ".strip", stripped) || !write_text(options.out + ".text", text)) {
            std::fprintf(stderr, "cannot write %s.strip / %s.text\n", options.out.c_str(), options.out.c_str());
            return 1;
        }
        std::fprintf(stderr, "wrote %s.strip and %s.text\n", options.out.c_str(), options.out.c_str());
    }
    if (options.view == "strip" || options.view == "both") show_view("STRIP", stripped, options.tail);
    if (options.view == "text" || options.view == "both") show_view("TEXT", text, options.tail);
    return 0;
}

}
