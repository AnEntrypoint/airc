#include "harness/harness.hpp"

#include "common/text.hpp"

#include <algorithm>
#include <stdexcept>

#if defined(_WIN32)
#include <windows.h>
#else
extern char** environ;
#endif

namespace ccb {

namespace {

bool any_marker(const std::vector<std::string>& markers, std::string_view pane) {
    return std::any_of(markers.begin(), markers.end(),
                       [pane](const std::string& marker) { return contains(pane, marker); });
}

std::vector<std::string> process_environment() {
    std::vector<std::string> entries;
#if defined(_WIN32)
    wchar_t* block = GetEnvironmentStringsW();
    if (block == nullptr) {
        return entries;
    }
    for (const wchar_t* entry = block; *entry != L'\0';) {
        std::wstring_view wide(entry);
        int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
                                         nullptr, nullptr);
        std::string narrow(static_cast<std::size_t>(length), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), narrow.data(), length, nullptr,
                            nullptr);
        entries.push_back(std::move(narrow));
        entry += wide.size() + 1;
    }
    FreeEnvironmentStringsW(block);
#else
    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
        entries.emplace_back(*entry);
    }
#endif
    return entries;
}

std::string_view entry_key(std::string_view entry) {
    auto equals = entry.find('=', 1);
    return equals == std::string_view::npos ? entry : entry.substr(0, equals);
}

std::vector<std::string> substitute_session(const std::vector<std::string>& form, const std::string& session_id) {
    std::vector<std::string> words;
    words.reserve(form.size());
    for (const auto& word : form) {
        words.push_back(replace_all(word, "{session}", session_id));
    }
    return words;
}

const std::vector<std::string>& require_form(const std::optional<std::vector<std::string>>& form,
                                             const std::string& harness_name, const char* form_name) {
    if (!form) {
        throw std::runtime_error("harness '" + harness_name + "' has no " + form_name + " form");
    }
    return *form;
}

}

bool Harness::pane_ready(std::string_view pane) const {
    return any_marker(ready_markers, pane);
}

bool Harness::pane_busy(std::string_view pane) const {
    return any_marker(busy_markers, pane);
}

std::vector<std::string> Harness::child_environment() const {
    std::vector<std::string> result;
    bool term_present = false;
    for (auto& entry : process_environment()) {
        auto key = entry_key(entry);
        if (std::find(env_strip.begin(), env_strip.end(), key) != env_strip.end()) {
            continue;
        }
        if (key == "TERM") {
            auto value = std::string_view(entry).substr(key.size() + (key.size() < entry.size() ? 1 : 0));
            term_present = true;
            if (value.empty() || value == "dumb") {
                result.push_back("TERM=" + term);
                continue;
            }
        }
        result.push_back(std::move(entry));
    }
    if (!term_present) {
        result.push_back("TERM=" + term);
    }
    return result;
}

std::string Harness::bracketed_paste(std::string_view text) const {
    std::string bytes;
    bytes.reserve(paste_open.size() + text.size() + paste_close.size());
    bytes += paste_open;
    bytes += text;
    bytes += paste_close;
    return bytes;
}

std::vector<std::string> Harness::resume_args(const std::string& session_id) const {
    if (transcript) {
        transcript->check_selector(session_id);
    }
    return substitute_session(require_form(resume_form, name, "resume"), session_id);
}

std::vector<std::string> Harness::continue_args() const {
    return require_form(continue_form, name, "continue");
}

std::vector<std::string> Harness::fork_args(const std::string& session_id) const {
    if (transcript) {
        transcript->check_selector(session_id);
    }
    return substitute_session(require_form(fork_form, name, "fork"), session_id);
}

std::vector<std::string> Harness::fork_continue_args() const {
    return require_form(fork_continue_form, name, "fork_continue");
}

}
