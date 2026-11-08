#pragma once

#include "harness/transcript.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ccb {

inline constexpr std::string_view paste_open = "\x1b[200~";
inline constexpr std::string_view paste_close = "\x1b[201~";

struct ArgRule {
    std::string env;
    std::optional<std::string> equals;
    std::vector<std::string> append;
};

struct Harness {
    std::string name;
    std::vector<std::string> argv;
    std::vector<ArgRule> argv_env;
    std::vector<std::string> env_strip;
    std::string term = "xterm-256color";
    double submit_delay = 0.0;
    std::vector<std::string> ready_markers;
    std::vector<std::string> busy_markers;
    std::optional<std::vector<std::string>> resume_form;
    std::optional<std::vector<std::string>> continue_form;
    std::optional<std::vector<std::string>> fork_form;
    std::optional<std::vector<std::string>> fork_continue_form;
    std::shared_ptr<const Transcript> transcript;
    bool is_default = false;

    bool pane_ready(std::string_view pane) const;
    bool pane_busy(std::string_view pane) const;
    std::vector<std::string> child_environment() const;
    std::string bracketed_paste(std::string_view text) const;
    std::vector<std::string> resume_args(const std::string& session_id) const;
    std::vector<std::string> continue_args() const;
    std::vector<std::string> fork_args(const std::string& session_id) const;
    std::vector<std::string> fork_continue_args() const;
};

}
