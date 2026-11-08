#include "common/paths.hpp"
#include "common/text.hpp"
#include "common/time.hpp"
#include "harness/transcript.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
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

SessionEntry entry_for(const std::string& id, const fs::path& path, const FileInfo& info) {
    SessionEntry entry;
    entry.session_id = id;
    entry.path = path.string();
    entry.mtime = info.mtime;
    entry.size = info.size;
    return entry;
}

bool valid_chat_id(const std::string& id) {
    if (id.empty() || id.size() > 256) return false;
    auto alnum = [](char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); };
    if (!alnum(id[0])) return false;
    for (char c : id) {
        if (!alnum(c) && c != '.' && c != '_' && c != ':' && c != '-') return false;
    }
    return true;
}

fs::path project_dir(const fs::path& root, const std::string& cwd) {
    return root / "projects" / fs::path(absolute_cwd(cwd)).filename();
}

std::vector<fs::path> chat_files(const fs::path& project) {
    std::vector<fs::path> found;
    for (const auto& chat : children(project / "chats")) {
        fs::path file = chat / "chat-messages.json";
        if (dir_exists(chat) && is_file(file)) found.push_back(file);
    }
    return found;
}

json read_messages(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return json::array();
    json data = json::parse(in, nullptr, false);
    if (data.is_discarded()) return json::array();
    if (data.is_array()) return data;
    json inner = member_or_null(data, "messages");
    return inner.is_array() ? inner : json::array();
}

std::string message_key(std::size_t index, const json& message) {
    json id = member_or_null(message, "id");
    bool truthy = (id.is_string() && !id.get<std::string>().empty()) ||
                  (id.is_number() && id != 0) || id.is_array() || id.is_object() || id == true;
    if (truthy) return python_str(id);
    return "index-" + std::to_string(index);
}

std::string concatenated(const json& message, bool reasoning) {
    std::string text;
    json blocks = member_or_null(message, "blocks");
    if (!blocks.is_array()) return text;
    for (const auto& block : blocks) {
        if (!block.is_object() || string_member(block, "type") != "text") continue;
        bool is_reasoning = string_member(block, "textType") == "reasoning";
        if (is_reasoning == reasoning) text += string_member(block, "content");
    }
    return text;
}

class FreebuffChannel : public Channel {
public:
    FreebuffChannel(fs::path root, std::vector<std::string> notices, std::string cwd, double since,
                    bool at_end, std::optional<std::string> session_id)
        : Channel(cwd, since, at_end, std::move(session_id)),
          project_(project_dir(root, cwd)),
          notices_(std::move(notices)) {}

    std::vector<Event> poll(const KindSet& kinds) override {
        std::vector<Event> out;
        KindSet want = wanted(kinds);
        want.insert(Kind::TurnEnd);
        auto path = chat_path();
        if (!path) return pane_notice(want);
        if (!path_ || *path_ != *path) {
            bind(*path);
            if (at_end_) {
                seed(*path);
            } else {
                text_len_.clear();
                done_ids_.clear();
            }
        }
        json messages = read_messages(*path);
        for (std::size_t index = 0; index < messages.size(); ++index) {
            const json& message = messages[index];
            if (!message.is_object()) continue;
            std::string key = message_key(index, message);
            std::string variant = string_member(message, "variant");
            if (variant == "user") {
                if (!done_ids_.count(key) && want.count(Kind::You)) {
                    done_ids_.insert(key);
                    json content = member_or_null(message, "content");
                    out.push_back(make_event(Kind::You, content.is_null() ? std::string() : python_str(content)));
                }
                continue;
            }
            if (variant != "ai") continue;
            emit_ai(message, key, want, out);
        }
        return out;
    }

private:
    void emit_ai(const json& message, const std::string& key, const KindSet& want, std::vector<Event>& out) {
        std::string said = concatenated(message, false);
        std::string thought = concatenated(message, true);
        if (!said.empty() && want.count(Kind::Text)) {
            std::string piece = delta(key, said, text_len_);
            if (!piece.empty()) out.push_back(make_event(Kind::Text, std::move(piece)));
        }
        if (!thought.empty() && want.count(Kind::Thinking)) {
            std::string piece = delta(key + "#thinking", thought, text_len_);
            if (!piece.empty()) out.push_back(make_event(Kind::Thinking, std::move(piece)));
        }
        json complete = member_or_null(message, "isComplete");
        bool is_complete = complete == true || (complete.is_number() && complete != 0) ||
                           (complete.is_string() && !complete.get<std::string>().empty());
        if (!is_complete || done_ids_.count(key)) return;
        done_ids_.insert(key);
        json error = member_or_null(message, "userError");
        bool has_error = !error.is_null() && error != false && error != "" && error != 0;
        if (has_error && want.count(Kind::Result)) {
            out.push_back(make_event(Kind::Result, truncate_code_points(python_str(error), result_limit)));
        }
        if (want.count(Kind::TurnEnd)) out.push_back(make_event(Kind::TurnEnd, ""));
    }

    std::optional<fs::path> newest_chat() const { return newest(chat_files(project_), 0.0); }

    std::optional<fs::path> chat_path() const {
        if (session_id_ && !session_id_->empty()) {
            std::error_code ec;
            fs::path file = project_ / "chats" / *session_id_ / "chat-messages.json";
            if (fs::exists(file, ec)) return file;
        }
        return newest_chat();
    }

    void seed(const fs::path& path) {
        text_len_.clear();
        done_ids_.clear();
        json messages = read_messages(path);
        for (std::size_t index = 0; index < messages.size(); ++index) {
            const json& message = messages[index];
            if (!message.is_object()) continue;
            std::string key = message_key(index, message);
            done_ids_.insert(key);
            if (string_member(message, "variant") != "ai") continue;
            text_len_[key] = concatenated(message, false).size();
            text_len_[key + "#thinking"] = concatenated(message, true).size();
        }
    }

    std::vector<Event> pane_notice(const KindSet& want) {
        std::vector<Event> out;
        if (!pane_) return out;
        for (const auto& line : split_lines(pane_())) {
            bool matches = std::any_of(notices_.begin(), notices_.end(),
                                       [&](const std::string& notice) { return contains(line, notice); });
            if (!matches) continue;
            std::string trimmed(trim(line));
            std::string key = "notice:" + trimmed;
            if (done_ids_.count(key)) return out;
            done_ids_.insert(key);
            if (want.count(Kind::Text)) out.push_back(make_event(Kind::Text, trimmed));
            if (want.count(Kind::TurnEnd)) out.push_back(make_event(Kind::TurnEnd, ""));
            return out;
        }
        return out;
    }

    fs::path project_;
    std::vector<std::string> notices_;
};

class FreebuffTranscript : public Transcript {
public:
    FreebuffTranscript(fs::path root, std::vector<std::string> notices)
        : root_(std::move(root)), notices_(std::move(notices)) {}

    std::unique_ptr<Channel> channel(const std::string& cwd, double since, bool at_end,
                                     std::optional<std::string> session_id) const override {
        return std::make_unique<FreebuffChannel>(root_, notices_, cwd, since, at_end, std::move(session_id));
    }

    std::vector<SessionEntry> sessions(const std::string& cwd) const override {
        std::vector<SessionEntry> found;
        for (const auto& file : chat_files(project_dir(root_, cwd))) {
            std::string id = file.parent_path().filename().string();
            if (!valid_chat_id(id)) continue;
            auto info = stat_file(file);
            if (!info || read_messages(file).empty()) continue;
            found.push_back(entry_for(id, file, *info));
        }
        std::stable_sort(found.begin(), found.end(),
                         [](const SessionEntry& a, const SessionEntry& b) { return a.mtime < b.mtime; });
        return found;
    }

    std::optional<SessionEntry> find(const std::string&) const override { return std::nullopt; }

    void check_selector(const std::string& selector) const override {
        if (valid_chat_id(selector)) return;
        throw std::invalid_argument("bad conversation id '" + selector + "'");
    }

private:
    fs::path root_;
    std::vector<std::string> notices_;
};

}

std::unique_ptr<Transcript> make_freebuff_transcript(const TranscriptSpec& spec) {
    return std::make_unique<FreebuffTranscript>(resolve_root(spec), spec.notices);
}

}
