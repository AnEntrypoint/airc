#pragma once

#include "harness/channel.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ccb {

struct SessionEntry {
    std::string session_id;
    std::string path;
    double mtime = 0.0;
    std::uintmax_t size = 0;
    std::string title;
    std::string cwd;
};

struct TranscriptSpec {
    std::string kind;
    std::string root;
    std::string root_env;
    std::vector<std::string> notices;
};

class Transcript {
public:
    virtual ~Transcript() = default;
    virtual std::unique_ptr<Channel> channel(const std::string& cwd, double since, bool at_end,
                                             std::optional<std::string> session_id) const = 0;
    virtual std::vector<SessionEntry> sessions(const std::string& cwd) const = 0;
    virtual std::optional<SessionEntry> find(const std::string& session_id) const = 0;
    virtual void check_selector(const std::string& selector) const = 0;
};

std::filesystem::path resolve_root(const TranscriptSpec& spec);
std::unique_ptr<Transcript> make_transcript(const TranscriptSpec& spec);
std::unique_ptr<Transcript> make_claude_transcript(const TranscriptSpec& spec);
std::unique_ptr<Transcript> make_codex_transcript(const TranscriptSpec& spec);
std::unique_ptr<Transcript> make_freebuff_transcript(const TranscriptSpec& spec);

}
