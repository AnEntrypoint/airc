#include "common/paths.hpp"
#include "common/text.hpp"
#include "common/time.hpp"
#include "harness/transcript.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ccb {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr std::size_t result_limit = 4000;

std::string safe_dump(const json& value) {
    return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string truncate_code_points(const std::string& text, std::size_t limit) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) continue;
        if (count == limit) return text.substr(0, i);
        ++count;
    }
    return text;
}

std::string string_member(const json& object, const char* key, const std::string& fallback = "") {
    if (!object.is_object()) return fallback;
    auto it = object.find(key);
    if (it == object.end() || !it->is_string()) return fallback;
    return it->get<std::string>();
}

json member_or_null(const json& object, const char* key) {
    if (!object.is_object()) return json(nullptr);
    auto it = object.find(key);
    return it == object.end() ? json(nullptr) : *it;
}

std::string python_str(const json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_null()) return "None";
    return safe_dump(value);
}

Event make_event(Kind kind, std::string text) {
    Event event;
    event.kind = kind;
    event.text = std::move(text);
    return event;
}

std::string absolute_cwd(const std::string& cwd) {
    std::error_code ec;
    fs::path absolute = fs::absolute(fs::path(cwd), ec).lexically_normal();
    if (!absolute.has_filename()) absolute = absolute.parent_path();
    return absolute.string();
}

std::vector<fs::path> children(const fs::path& dir) {
    std::vector<fs::path> found;
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec) return found;
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        found.push_back(it->path());
    }
    std::sort(found.begin(), found.end());
    return found;
}

bool dir_exists(const fs::path& path) {
    std::error_code ec;
    return fs::is_directory(path, ec);
}

bool is_file(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

std::string basename_of(const std::string& cwd) {
    return fs::path(cwd).filename().string();
}

std::string without_extension(const fs::path& path, const std::string& extension) {
    std::string name = path.filename().string();
    return name.substr(0, name.size() - extension.size());
}

SessionEntry entry_for(const std::string& id, const fs::path& path, const FileInfo& info) {
    SessionEntry entry;
    entry.session_id = id;
    entry.path = path.string();
    entry.mtime = info.mtime;
    entry.size = info.size;
    return entry;
}

fs::path project_dir(const fs::path& root, const std::string& cwd) {
    return root / slug_path(absolute_cwd(cwd));
}

std::vector<fs::path> jsonl_files(const fs::path& dir) {
    std::vector<fs::path> found;
    for (const auto& path : children(dir)) {
        if (path.extension() == ".jsonl") found.push_back(path);
    }
    return found;
}

std::optional<fs::path> find_path(const fs::path& root, const std::string& session_id) {
    if (session_id.empty() || session_id.find_first_of("/\\") != std::string::npos) return std::nullopt;
    std::optional<fs::path> best;
    double best_mtime = 0.0;
    for (const auto& dir : children(root)) {
        if (!dir_exists(dir)) continue;
        fs::path candidate = dir / (session_id + ".jsonl");
        auto info = stat_file(candidate);
        if (!info || !is_file(candidate)) continue;
        if (!best || info->mtime > best_mtime) {
            best = candidate;
            best_mtime = info->mtime;
        }
    }
    return best;
}

class ClaudeChannel : public Channel {
public:
    ClaudeChannel(fs::path root, std::string cwd, double since, bool at_end,
                  std::optional<std::string> session_id)
        : Channel(cwd, since, at_end, std::move(session_id)),
          root_(std::move(root)),
          project_(project_dir(root_, cwd)) {}

    std::vector<Event> poll(const KindSet& kinds) override {
        std::vector<Event> out;
        if (!attach()) return out;
        KindSet want = wanted(kinds);
        want.insert(Kind::TurnEnd);
        for (const auto& record : tail_records()) {
            if (before_start(record) || !record.is_object()) continue;
            std::string type = string_member(record, "type");
            if (type == "system") {
                if (string_member(record, "subtype") == "turn_duration") {
                    out.push_back(make_event(Kind::TurnEnd, ""));
                }
                continue;
            }
            if (type != "assistant" && type != "user") continue;
            json message = member_or_null(record, "message");
            if (!message.is_object()) message = json::object();
            json blocks = member_or_null(message, "content");
            if (blocks.is_null()) continue;
            if (blocks.is_string()) {
                if (want.count(Kind::You)) out.push_back(make_event(Kind::You, blocks.get<std::string>()));
                continue;
            }
            if (!blocks.is_array()) continue;
            std::string message_id = string_member(message, "id");
            if (message_id.empty()) message_id = string_member(record, "uuid");
            long long index = 0;
            json api_index = member_or_null(record, "apiBlockIndex");
            if (api_index.is_number_integer()) index = api_index.get<long long>();
            for (const auto& block : blocks) {
                if (!block.is_object()) continue;
                emit_block(block, message_id, index, want, out);
            }
        }
        return out;
    }

private:
    void emit_block(const json& block, const std::string& message_id, long long index,
                    const KindSet& want, std::vector<Event>& out) {
        std::string type = string_member(block, "type");
        if (type == "text") {
            auto& accumulated = text_blocks_[message_id];
            accumulated[index] = string_member(block, "text");
            std::string full;
            for (const auto& part : accumulated) full += part.second;
            if (!want.count(Kind::Text)) return;
            std::string piece = delta(message_id, full, text_len_);
            if (!piece.empty()) out.push_back(make_event(Kind::Text, std::move(piece)));
        } else if (type == "thinking" && want.count(Kind::Thinking)) {
            out.push_back(make_event(Kind::Thinking, string_member(block, "thinking")));
        } else if (type == "tool_use" && want.count(Kind::Tool)) {
            Event event = make_event(Kind::Tool, string_member(block, "name"));
            event.input = member_or_null(block, "input");
            out.push_back(std::move(event));
        } else if (type == "tool_result" && want.count(Kind::Result)) {
            json content = member_or_null(block, "content");
            std::string text;
            if (content.is_array()) {
                for (const auto& part : content) text += string_member(part, "text");
            } else {
                text = python_str(content);
            }
            out.push_back(make_event(Kind::Result, truncate_code_points(text, result_limit)));
        }
    }

    std::vector<fs::path> candidates() const {
        std::vector<fs::path> seen;
        if (dir_exists(project_)) seen = jsonl_files(project_);
        if (seen.empty()) {
            std::string tag = slug_path(basename_of(cwd_));
            for (const auto& dir : children(root_)) {
                if (!dir_exists(dir)) continue;
                if (!tag.empty() && dir.filename().string().find(tag) == std::string::npos) continue;
                for (const auto& path : jsonl_files(dir)) {
                    auto info = stat_file(path);
                    if (info && info->mtime + 3 >= since_) seen.push_back(path);
                }
            }
        }
        std::vector<std::pair<double, fs::path>> timed;
        for (const auto& path : seen) {
            auto info = stat_file(path);
            if (info) timed.emplace_back(info->mtime, path);
        }
        std::stable_sort(timed.begin(), timed.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<fs::path> ordered;
        for (const auto& item : timed) ordered.push_back(item.second);
        return ordered;
    }

    bool attach() {
        if (session_id_ && !session_id_->empty()) {
            const std::string& selector = *session_id_;
            fs::path as_path(selector);
            if (as_path.is_absolute() && is_file(as_path)) return bind(as_path);
            std::error_code ec;
            fs::path target = project_ / (selector + ".jsonl");
            if (fs::exists(target, ec)) return bind(target);
            if (is_uuid(selector)) {
                auto global = find_path(root_, selector);
                return global ? bind(*global) : false;
            }
        }
        auto paths = candidates();
        for (auto it = paths.rbegin(); it != paths.rend(); ++it) {
            auto chosen = newest({*it}, since_);
            if (chosen) return bind(*chosen);
        }
        return false;
    }

    fs::path root_;
    fs::path project_;
    std::map<std::string, std::map<long long, std::string>> text_blocks_;
};

class ClaudeTranscript : public Transcript {
public:
    explicit ClaudeTranscript(fs::path root) : root_(std::move(root)) {}

    std::unique_ptr<Channel> channel(const std::string& cwd, double since, bool at_end,
                                     std::optional<std::string> session_id) const override {
        return std::make_unique<ClaudeChannel>(root_, cwd, since, at_end, std::move(session_id));
    }

    std::vector<SessionEntry> sessions(const std::string& cwd) const override {
        std::vector<SessionEntry> found;
        for (const auto& path : jsonl_files(project_dir(root_, cwd))) {
            std::string id = without_extension(path, ".jsonl");
            if (id.empty() || starts_with(id, "agent-")) continue;
            auto info = stat_file(path);
            if (!info || info->size == 0) continue;
            found.push_back(entry_for(id, path, *info));
        }
        std::stable_sort(found.begin(), found.end(),
                         [](const SessionEntry& a, const SessionEntry& b) { return a.mtime < b.mtime; });
        return found;
    }

    std::optional<SessionEntry> find(const std::string& session_id) const override {
        auto path = find_path(root_, session_id);
        if (!path) return std::nullopt;
        auto info = stat_file(*path);
        if (!info) return std::nullopt;
        return entry_for(session_id, *path, *info);
    }

    void check_selector(const std::string& selector) const override {
        fs::path as_path(selector);
        if (as_path.is_absolute() && is_file(as_path)) return;
        if (is_uuid(selector) || is_session_name(selector)) return;
        throw std::invalid_argument("bad session selector '" + selector + "'");
    }

private:
    fs::path root_;
};

}

std::unique_ptr<Transcript> make_claude_transcript(const TranscriptSpec& spec) {
    return std::make_unique<ClaudeTranscript>(resolve_root(spec));
}

}
