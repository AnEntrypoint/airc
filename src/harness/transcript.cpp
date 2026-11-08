#include "harness/transcript.hpp"

#include "common/paths.hpp"
#include "common/text.hpp"

#include <stdexcept>

namespace ccb {

std::filesystem::path resolve_root(const TranscriptSpec& spec) {
    if (!spec.root_env.empty()) {
        std::string from_env = env_or_empty(spec.root_env.c_str());
        if (!from_env.empty()) return std::filesystem::path(from_env);
    }
    return std::filesystem::path(replace_all(spec.root, "{home}", home_dir().string()));
}

std::unique_ptr<Transcript> make_transcript(const TranscriptSpec& spec) {
    if (spec.kind == "claude-jsonl") return make_claude_transcript(spec);
    if (spec.kind == "codex-jsonl") return make_codex_transcript(spec);
    if (spec.kind == "freebuff-json") return make_freebuff_transcript(spec);
    throw std::runtime_error("unknown transcript kind '" + spec.kind + "'");
}

}
